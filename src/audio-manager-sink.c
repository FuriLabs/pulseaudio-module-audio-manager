/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include "audio-manager-sink.h"

#include "audio-manager-shared.h"
#include "audio-manager-util.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <pulse/rtclock.h>
#include <pulse/sample.h>
#include <pulse/util.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-util.h>
#include <pulsecore/device-port.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_audio_manager_shared *shared;
    AudioManagerStream *stream;
    AudioManagerStreamConfig requested_config;
    AudioManagerStreamConfig stream_config;
    AudioManagerPlaybackRole playback_role;

    size_t period_bytes;
    uint8_t *mix_buffer;
    size_t mix_buffer_size;
};

enum {
    PA_AUDIO_MANAGER_SINK_MESSAGE_SET_PLAYBACK_ROLE = PA_SINK_MESSAGE_MAX,
};

struct playback_role_request {
    AudioManagerPlaybackRole role;
};

static void
userdata_free(struct userdata *u);

static pa_usec_t
stream_latency(struct userdata *u)
{
    gint64 delay_frames = 0;
    int ret;

    pa_assert(u);
    pa_assert(u->stream);

    ret = audio_manager_stream_get_delay(u->stream, &delay_frames);
    if (ret < 0 || delay_frames < 0)
        return pa_bytes_to_usec((uint64_t)u->stream_config.period_size *
                                u->stream_config.frame_bytes,
                                &u->sink->sample_spec);

    return pa_bytes_to_usec((uint64_t)delay_frames *
                            u->stream_config.frame_bytes,
                            &u->sink->sample_spec);
}

static bool
output_needs_dual_mono(struct userdata *u)
{
    AudioManagerOutputDevice device;

    pa_assert(u);
    pa_assert(u->shared);
    pa_assert(u->sink);

    if (u->sink->sample_spec.format != PA_SAMPLE_S16LE ||
        u->sink->sample_spec.channels != 2)
        return false;

    device = audio_manager_get_output_device(pa_audio_manager_shared_manager(u->shared));

    return device == AUDIO_MANAGER_OUTPUT_RECEIVER ||
           device == AUDIO_MANAGER_OUTPUT_SPEAKER;
}

static void
stereo_s16le_to_dual_mono(void *data, size_t bytes)
{
    int16_t *samples = data;
    size_t frames;
    size_t i;

    pa_assert(data);

    frames = bytes / (sizeof(int16_t) * 2U);
    for (i = 0; i < frames; i++) {
        int16_t left = samples[i * 2U];
        int16_t right = samples[i * 2U + 1U];
        int16_t mono = (int16_t)((left / 2) + (right / 2));

        samples[i * 2U] = mono;
        samples[i * 2U + 1U] = mono;
    }
}

static int
thread_write_period(struct userdata *u)
{
    pa_memchunk chunk;
    const uint8_t *data;
    const uint8_t *rendered;
    size_t offset = 0;
    bool memblock_acquired = false;
    int ret = -1;

    pa_assert(u);
    pa_assert(u->sink);
    pa_assert(u->stream);

    pa_sink_render_full(u->sink, u->period_bytes, &chunk);

    rendered = pa_memblock_acquire_chunk(&chunk);
    memblock_acquired = true;

    if (output_needs_dual_mono(u)) {
        if (u->mix_buffer_size < chunk.length) {
            u->mix_buffer = pa_xrealloc(u->mix_buffer, chunk.length);
            u->mix_buffer_size = chunk.length;
        }

        memcpy(u->mix_buffer, rendered, chunk.length);
        pa_memblock_release(chunk.memblock);
        memblock_acquired = false;

        stereo_s16le_to_dual_mono(u->mix_buffer, chunk.length);
        data = u->mix_buffer;
    } else
        data = rendered;

    while (offset < chunk.length) {
        gssize written;

        written = audio_manager_stream_write(u->stream,
                                             data + offset,
                                             chunk.length - offset);
        if (written < 0) {
            pa_log_error("audio-manager playback write failed: %zd", written);
            goto finish;
        }

        if (written == 0) {
            pa_log_error("audio-manager playback write returned zero bytes");
            goto finish;
        }

        offset += (size_t)written;
    }

    ret = 0;

finish:
    if (memblock_acquired)
        pa_memblock_release(chunk.memblock);
    pa_memblock_unref(chunk.memblock);
    return ret;
}

static void
process_rewind(struct userdata *u)
{
    pa_assert(u);
    pa_assert(u->sink);

    u->sink->thread_info.rewind_nbytes = 0;
    pa_sink_process_rewind(u->sink, 0);
}

static void
thread_func(void *userdata)
{
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("audio-manager sink thread starting");

    if (u->core->realtime_scheduling)
        pa_thread_make_realtime(u->core->realtime_priority);

    pa_thread_mq_install(&u->thread_mq);

    for (;;) {
        int ret;

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
                process_rewind(u);

            if (thread_write_period(u) < 0)
                goto fail;

            pa_rtpoll_set_timer_relative(u->rtpoll, 0);
        } else
            pa_rtpoll_set_timer_disabled(u->rtpoll);

        ret = pa_rtpoll_run(u->rtpoll);
        if (ret < 0)
            goto fail;
        if (ret == 0)
            goto finish;
    }

fail:
    pa_asyncmsgq_post(u->thread_mq.outq,
                      PA_MSGOBJECT(u->core),
                      PA_CORE_MESSAGE_UNLOAD_MODULE,
                      u->module,
                      0,
                      NULL,
                      NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("audio-manager sink thread shutting down");
}

static int
suspend_stream(struct userdata *u)
{
    int ret;

    pa_assert(u);

    ret = audio_manager_stream_stop(u->stream);
    if (ret < 0) {
        pa_log_error("failed to stop audio-manager playback stream: %d", ret);
        return ret;
    }

    pa_log_debug("audio-manager playback stream suspended");
    return 0;
}

static int
resume_stream(struct userdata *u)
{
    int ret;

    pa_assert(u);

    ret = audio_manager_stream_start(u->stream);
    if (ret < 0) {
        pa_log_error("failed to start audio-manager playback stream: %d", ret);
        return ret;
    }

    pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
    pa_log_debug("audio-manager playback stream resumed");
    return 0;
}

static int
sink_set_state_in_io_thread_cb(pa_sink *sink,
                               pa_sink_state_t new_state,
                               pa_suspend_cause_t new_suspend_cause)
{
    struct userdata *u;

    pa_assert(sink);
    pa_assert_se(u = sink->userdata);
    (void)new_suspend_cause;

    if (new_state == sink->thread_info.state)
        return 0;

    switch (new_state) {
    case PA_SINK_SUSPENDED:
        if (PA_SINK_IS_OPENED(sink->thread_info.state))
            return suspend_stream(u);
        break;
    case PA_SINK_IDLE:
    case PA_SINK_RUNNING:
        if (sink->thread_info.state == PA_SINK_SUSPENDED)
            return resume_stream(u);
        pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
        break;
    case PA_SINK_UNLINKED:
        if (PA_SINK_IS_OPENED(sink->thread_info.state))
            suspend_stream(u);
        break;
    case PA_SINK_INIT:
    case PA_SINK_INVALID_STATE:
        break;
    }

    return 0;
}

static bool
stream_config_matches_sink(const AudioManagerStreamConfig *config,
                           const pa_sample_spec *sample_spec)
{
    pa_sample_format_t format;

    pa_assert(config);
    pa_assert(sample_spec);

    if (!pa_audio_manager_sample_format_from_am(config->format, &format))
        return false;

    return format == sample_spec->format &&
           config->rate == sample_spec->rate &&
           config->channels == sample_spec->channels;
}

static AudioManagerStream *
open_playback_stream(struct userdata *u,
                     AudioManagerPlaybackRole role,
                     AudioManagerStreamConfig *actual,
                     GError **error)
{
    AudioManagerStreamConfig requested;
    AudioManagerStream *stream;
    int ret;

    pa_assert(u);
    pa_assert(actual);

    requested = u->requested_config;
    requested.playback_role = role;

    stream = audio_manager_stream_open(pa_audio_manager_shared_manager(u->shared),
                                       &requested,
                                       error);
    if (stream == NULL)
        return NULL;

    *actual = (AudioManagerStreamConfig) AUDIO_MANAGER_STREAM_CONFIG_INIT;
    ret = audio_manager_stream_get_config(stream, actual);
    if (ret < 0) {
        audio_manager_stream_close(stream);
        if (error != NULL && *error == NULL)
            g_set_error(error,
                        g_quark_from_static_string("pulse-audio-manager-sink"),
                        -ret,
                        "failed to query negotiated playback stream: %d",
                        ret);
        return NULL;
    }

    return stream;
}

static AudioManagerStream *
open_playback_stream_with_fallback(struct userdata *u,
                                   AudioManagerPlaybackRole role,
                                   AudioManagerStreamConfig *actual,
                                   GError **error)
{
    const AudioManagerCapabilities *capabilities;
    AudioManagerStream *stream;

    pa_assert(u);
    pa_assert(actual);

    stream = open_playback_stream(u, role, actual, error);
    if (stream != NULL || role != AUDIO_MANAGER_PLAYBACK_ROLE_LOW_LATENCY)
        return stream;

    capabilities = pa_audio_manager_shared_capabilities(u->shared);
    if (!(capabilities->playback_roles &
          AUDIO_MANAGER_PLAYBACK_ROLE_MASK(AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT)))
        return NULL;

    pa_log_warn("low latency audio-manager playback unavailable, falling back to default");
    if (error != NULL)
        g_clear_error(error);

    return open_playback_stream(u,
                                AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT,
                                actual,
                                error);
}

static int
restore_playback_role(struct userdata *u,
                      AudioManagerPlaybackRole role,
                      bool restart)
{
    AudioManagerStreamConfig config = AUDIO_MANAGER_STREAM_CONFIG_INIT;
    AudioManagerStream *stream;
    GError *error = NULL;
    size_t period_bytes;
    int ret;

    pa_assert(u);

    stream = open_playback_stream_with_fallback(u, role, &config, &error);
    period_bytes = (size_t)config.period_size * config.frame_bytes;
    if (stream == NULL ||
        period_bytes == 0 ||
        !stream_config_matches_sink(&config, &u->sink->sample_spec)) {
        if (stream != NULL)
            audio_manager_stream_close(stream);
        pa_log_error("failed to restore audio-manager playback role %s%s%s",
                     pa_audio_manager_playback_role_to_string(role),
                     error != NULL ? ": " : "",
                     error != NULL ? error->message : "");
        g_clear_error(&error);
        return -EIO;
    }

    u->stream = stream;
    u->stream_config = config;
    u->period_bytes = period_bytes;
    u->playback_role = config.playback_role;

    if (!restart)
        return 0;

    ret = audio_manager_stream_start(u->stream);
    if (ret < 0) {
        audio_manager_stream_close(u->stream);
        u->stream = NULL;
        return ret;
    }

    pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
    return 0;
}

static int
set_playback_role_in_io_thread(struct userdata *u,
                               AudioManagerPlaybackRole role)
{
    AudioManagerStream *new_stream = NULL;
    AudioManagerStreamConfig new_config = AUDIO_MANAGER_STREAM_CONFIG_INIT;
    AudioManagerPlaybackRole old_role;
    size_t new_period_bytes;
    bool was_opened;
    GError *error = NULL;
    int ret;

    pa_assert(u);
    pa_assert(u->sink);

    if (role == u->playback_role)
        return 0;

    old_role = u->playback_role;
    was_opened = PA_SINK_IS_OPENED(u->sink->thread_info.state);

    if (was_opened) {
        ret = audio_manager_stream_stop(u->stream);
        if (ret < 0)
            return ret;
    }

    audio_manager_stream_close(u->stream);
    u->stream = NULL;

    new_stream = open_playback_stream_with_fallback(u, role, &new_config, &error);
    new_period_bytes = (size_t)new_config.period_size * new_config.frame_bytes;
    if (new_stream == NULL ||
        new_period_bytes == 0 ||
        !stream_config_matches_sink(&new_config, &u->sink->sample_spec)) {
        if (new_stream != NULL)
            audio_manager_stream_close(new_stream);

        if (error != NULL) {
            pa_log_warn("unable to switch audio-manager playback role to %s: %s",
                        pa_audio_manager_playback_role_to_string(role),
                        error->message);
            g_clear_error(&error);
        } else {
            pa_log_warn("playback role %s negotiated an incompatible sink configuration",
                        pa_audio_manager_playback_role_to_string(role));
        }

        ret = restore_playback_role(u, old_role, was_opened);
        return ret < 0 ? ret : -EINVAL;
    }

    u->stream = new_stream;
    u->stream_config = new_config;
    u->period_bytes = new_period_bytes;
    u->playback_role = new_config.playback_role;

    if (was_opened) {
        ret = audio_manager_stream_start(u->stream);
        if (ret < 0) {
            audio_manager_stream_close(u->stream);
            u->stream = NULL;
            if (restore_playback_role(u, old_role, true) < 0)
                return -EIO;
            return ret;
        }
        pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
    }

    return 0;
}

static int
sink_process_msg(pa_msgobject *object,
                 int code,
                 void *data,
                 int64_t offset,
                 pa_memchunk *chunk)
{
    pa_sink *sink = PA_SINK(object);
    struct userdata *u = sink->userdata;

    if (code == PA_SINK_MESSAGE_GET_LATENCY) {
        *((pa_usec_t *)data) = stream_latency(u);
        return 0;
    }

    if (code == PA_AUDIO_MANAGER_SINK_MESSAGE_SET_PLAYBACK_ROLE) {
        const struct playback_role_request *request = data;

        pa_assert(request);
        return set_playback_role_in_io_thread(u, request->role);
    }

    return pa_sink_process_msg(object, code, data, offset, chunk);
}

static AudioManagerPlaybackRole
choose_initial_playback_role(const AudioManagerCapabilities *capabilities)
{
    pa_assert(capabilities);

    if (capabilities->playback_roles &
        AUDIO_MANAGER_PLAYBACK_ROLE_MASK(AUDIO_MANAGER_PLAYBACK_ROLE_LOW_LATENCY))
        return AUDIO_MANAGER_PLAYBACK_ROLE_LOW_LATENCY;
    if (capabilities->playback_roles &
        AUDIO_MANAGER_PLAYBACK_ROLE_MASK(AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT))
        return AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT;

    return AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT;
}

static AudioManagerOutputDevice
choose_initial_output_device(const AudioManagerCapabilities *capabilities)
{
    pa_assert(capabilities);

    if (capabilities->output_devices &
        AUDIO_MANAGER_OUTPUT_DEVICE_MASK(AUDIO_MANAGER_OUTPUT_SPEAKER))
        return AUDIO_MANAGER_OUTPUT_SPEAKER;
    if (capabilities->output_devices &
        AUDIO_MANAGER_OUTPUT_DEVICE_MASK(AUDIO_MANAGER_OUTPUT_RECEIVER))
        return AUDIO_MANAGER_OUTPUT_RECEIVER;
    if (capabilities->output_devices &
        AUDIO_MANAGER_OUTPUT_DEVICE_MASK(AUDIO_MANAGER_OUTPUT_HEADPHONES))
        return AUDIO_MANAGER_OUTPUT_HEADPHONES;
    if (capabilities->output_devices &
        AUDIO_MANAGER_OUTPUT_DEVICE_MASK(AUDIO_MANAGER_OUTPUT_HEADSET))
        return AUDIO_MANAGER_OUTPUT_HEADSET;

    return AUDIO_MANAGER_OUTPUT_NONE;
}

pa_sink *
pa_audio_manager_sink_new_full(pa_module *module,
                               const char *driver,
                               const pa_audio_manager_sink_options *options)
{
    struct userdata *u = NULL;
    AudioManagerStreamConfig requested = AUDIO_MANAGER_STREAM_CONFIG_INIT;
    AudioManagerSampleFormat am_format;
    AudioManagerPlaybackRole role;
    AudioManagerOutputDevice output_device;
    pa_sink_new_data data;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    const char *output_device_name;
    const AudioManagerCapabilities *capabilities;
    GError *error = NULL;
    char *thread_name = NULL;
    pa_sample_format_t negotiated_format;
    int ret;

    pa_assert(module);
    pa_assert(driver);

    sample_spec.format = PA_SAMPLE_S16LE;
    sample_spec.rate = 48000;
    sample_spec.channels = 2;
    pa_channel_map_init_stereo(&channel_map);

    if (!pa_audio_manager_sample_format_to_am(sample_spec.format, &am_format)) {
        pa_log_error("unsupported audio-manager sink format '%s'",
                     pa_sample_format_to_string(sample_spec.format));
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = module->core;
    u->module = module;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, module->core->mainloop, u->rtpoll);

    if (options != NULL && options->shared != NULL)
        u->shared = pa_audio_manager_shared_ref(options->shared);
    else
        u->shared = pa_audio_manager_shared_get(module->core);

    if (u->shared == NULL)
        goto fail;

    capabilities = pa_audio_manager_shared_capabilities(u->shared);
    role = choose_initial_playback_role(capabilities);
    if (!(capabilities->playback_roles & AUDIO_MANAGER_PLAYBACK_ROLE_MASK(role))) {
        pa_log_error("audio-manager backend exposes no playback role");
        goto fail;
    }

    if (options != NULL && options->initial_device != AUDIO_MANAGER_OUTPUT_NONE)
        output_device = options->initial_device;
    else
        output_device = choose_initial_output_device(capabilities);

    if (output_device == AUDIO_MANAGER_OUTPUT_NONE ||
        !(capabilities->output_devices & AUDIO_MANAGER_OUTPUT_DEVICE_MASK(output_device))) {
        pa_log_error("audio-manager backend exposes no usable local output device");
        goto fail;
    }

    output_device_name = pa_audio_manager_output_device_to_string(output_device);

    if (options == NULL || options->set_initial_device) {
        ret = audio_manager_set_output_device(pa_audio_manager_shared_manager(u->shared),
                                              output_device);
        if (ret < 0) {
            pa_log_error("failed to select audio-manager output device '%s': %d",
                         output_device_name, ret);
            goto fail;
        }
    }

    requested.direction = AUDIO_MANAGER_STREAM_PLAYBACK;
    requested.format = am_format;
    requested.rate = sample_spec.rate;
    requested.channels = sample_spec.channels;
    requested.period_size = 0;
    requested.period_count = 0;
    requested.playback_role = role;
    requested.capture_role = AUDIO_MANAGER_CAPTURE_ROLE_DEFAULT;
    u->requested_config = requested;
    u->playback_role = role;

    u->stream = open_playback_stream_with_fallback(u, role, &u->stream_config, &error);
    if (u->stream == NULL) {
        pa_log_error("failed to open audio-manager playback stream: %s",
                     error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        goto fail;
    }

    if (!pa_audio_manager_sample_format_from_am(u->stream_config.format,
                                                &negotiated_format)) {
        pa_log_error("audio-manager returned unsupported playback format %d",
                     u->stream_config.format);
        goto fail;
    }

    sample_spec.format = negotiated_format;
    sample_spec.rate = u->stream_config.rate;
    sample_spec.channels = u->stream_config.channels;
    u->playback_role = u->stream_config.playback_role;

    if (channel_map.channels != sample_spec.channels &&
        pa_channel_map_init_auto(&channel_map,
                                 sample_spec.channels,
                                 PA_CHANNEL_MAP_ALSA) == NULL) {
        pa_log_error("failed to create channel map for %u playback channels",
                     sample_spec.channels);
        goto fail;
    }

    u->period_bytes = (size_t)u->stream_config.period_size * u->stream_config.frame_bytes;
    if (u->period_bytes == 0) {
        pa_log_error("audio-manager returned an invalid playback period size");
        goto fail;
    }

    pa_sink_new_data_init(&data);
    data.driver = driver;
    data.module = module;
    data.suspend_cause = PA_SUSPEND_IDLE;
    pa_sink_new_data_set_name(&data, "audio-manager-output");
    pa_sink_new_data_set_sample_spec(&data, &sample_spec);
    pa_sink_new_data_set_channel_map(&data, &channel_map);

    if (options != NULL) {
        pa_device_port *port;
        void *state = NULL;

        data.card = options->card;

        if (options->ports != NULL) {
            PA_HASHMAP_FOREACH(port, options->ports, state) {
                if (port->direction != PA_DIRECTION_OUTPUT)
                    continue;

                pa_assert_se(pa_hashmap_put(data.ports,
                                            port->name,
                                            pa_device_port_ref(port)) >= 0);
            }
        }

        if (options->active_port != NULL)
            pa_sink_new_data_set_port(&data, options->active_port);
    }

    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "sound");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_API, "audio-manager");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Output");
    pa_proplist_sets(data.proplist,
                     "audio_manager.playback_role",
                     pa_audio_manager_playback_role_to_string(u->playback_role));
    pa_proplist_sets(data.proplist, "audio_manager.output_device", output_device_name);

    u->sink = pa_sink_new(module->core,
                          &data,
                          PA_SINK_HARDWARE | PA_SINK_LATENCY);
    pa_sink_new_data_done(&data);
    if (u->sink == NULL) {
        pa_log_error("failed to create audio-manager sink");
        goto fail;
    }

    u->sink->userdata = u;
    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;
    if (options != NULL)
        u->sink->set_port = options->set_port;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);
    pa_sink_set_max_rewind(u->sink, 0);
    pa_sink_set_max_request(u->sink, u->period_bytes);
    pa_sink_set_fixed_latency(u->sink,
                              pa_bytes_to_usec((uint64_t)u->stream_config.period_size *
                                               u->stream_config.frame_bytes,
                                               &sample_spec));

    thread_name = pa_sprintf_malloc("audio-manager-sink-%s", u->sink->name);
    if ((u->thread = pa_thread_new(thread_name, thread_func, u)) == NULL) {
        pa_log_error("failed to create audio-manager sink thread");
        goto fail;
    }
    pa_xfree(thread_name);
    thread_name = NULL;

    pa_sink_put(u->sink);

    pa_log_info("created audio-manager sink '%s': %s %u Hz %u ch, period=%u x %u",
                u->sink->name,
                pa_sample_format_to_string(sample_spec.format),
                sample_spec.rate,
                sample_spec.channels,
                u->stream_config.period_size,
                u->stream_config.period_count);

    return u->sink;

fail:
    g_clear_error(&error);
    pa_xfree(thread_name);
    if (u != NULL)
        userdata_free(u);
    return NULL;
}

pa_sink *
pa_audio_manager_sink_new(pa_module *module,
                          const char *driver)
{
    return pa_audio_manager_sink_new_full(module, driver, NULL);
}

int
pa_audio_manager_sink_set_playback_role(pa_sink *sink,
                                        AudioManagerPlaybackRole role)
{
    struct userdata *u;
    struct playback_role_request request;
    const AudioManagerCapabilities *capabilities;
    int ret;

    pa_sink_assert_ref(sink);
    pa_assert_se(u = sink->userdata);

    capabilities = pa_audio_manager_shared_capabilities(u->shared);
    if (!(capabilities->playback_roles & AUDIO_MANAGER_PLAYBACK_ROLE_MASK(role)))
        return -ENOTSUP;

    request.role = role;
    ret = pa_asyncmsgq_send(u->thread_mq.inq,
                            PA_MSGOBJECT(sink),
                            PA_AUDIO_MANAGER_SINK_MESSAGE_SET_PLAYBACK_ROLE,
                            &request,
                            0,
                            NULL);
    if (ret < 0)
        return ret;

    pa_sink_set_fixed_latency(sink,
                              pa_bytes_to_usec((uint64_t)u->stream_config.period_size *
                                               u->stream_config.frame_bytes,
                                               &sink->sample_spec));
    pa_sink_set_max_request(sink, u->period_bytes);
    pa_proplist_sets(sink->proplist,
                     "audio_manager.playback_role",
                     pa_audio_manager_playback_role_to_string(u->playback_role));

    pa_log_info("audio-manager playback role changed to %s",
                pa_audio_manager_playback_role_to_string(u->playback_role));
    return 0;
}

AudioManagerPlaybackRole
pa_audio_manager_sink_get_playback_role(pa_sink *sink)
{
    struct userdata *u;

    pa_sink_assert_ref(sink);
    pa_assert_se(u = sink->userdata);
    return u->playback_role;
}

void
pa_audio_manager_sink_free(pa_sink *sink)
{
    struct userdata *u;

    pa_sink_assert_ref(sink);
    pa_assert_se(u = sink->userdata);

    userdata_free(u);
}

static void
userdata_free(struct userdata *u)
{
    pa_assert(u);

    if (u->sink != NULL)
        pa_sink_unlink(u->sink);

    if (u->thread != NULL) {
        pa_asyncmsgq_send(u->thread_mq.inq,
                          NULL,
                          PA_MESSAGE_SHUTDOWN,
                          NULL,
                          0,
                          NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->stream != NULL) {
        audio_manager_stream_stop(u->stream);
        audio_manager_stream_close(u->stream);
    }

    if (u->sink != NULL)
        pa_sink_unref(u->sink);

    if (u->rtpoll != NULL)
        pa_rtpoll_free(u->rtpoll);

    if (u->shared != NULL)
        pa_audio_manager_shared_unref(u->shared);

    pa_xfree(u->mix_buffer);
    pa_xfree(u);
}
