/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include "audio-manager-source.h"

#include "audio-manager-shared.h"
#include "audio-manager-util.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
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

#define STREAM_RECOVERY_INTERVAL_USEC (PA_USEC_PER_SEC)

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_source *source;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_audio_manager_shared *shared;
    AudioManagerStream *stream;
    AudioManagerStreamConfig requested_config;
    AudioManagerStreamConfig stream_config;
    AudioManagerCaptureRole capture_role;

    size_t period_bytes;
};

enum {
    PA_AUDIO_MANAGER_SOURCE_MESSAGE_SET_CAPTURE_ROLE = PA_SOURCE_MESSAGE_MAX,
};

struct capture_role_request {
    AudioManagerCaptureRole role;
};

static void
userdata_free(struct userdata *u)
{
    pa_assert(u);

    if (u->source != NULL)
        pa_source_unlink(u->source);

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

    if (u->source != NULL)
        pa_source_unref(u->source);

    if (u->rtpoll != NULL)
        pa_rtpoll_free(u->rtpoll);

    if (u->shared != NULL)
        pa_audio_manager_shared_unref(u->shared);

    pa_xfree(u);
}

static pa_usec_t
stream_latency(struct userdata *u)
{
    gint64 delay_frames = 0;
    int ret;

    pa_assert(u);

    if (u->stream == NULL)
        return 0;

    ret = audio_manager_stream_get_delay(u->stream, &delay_frames);
    if (ret < 0 || delay_frames < 0)
        return pa_bytes_to_usec((uint64_t)u->stream_config.period_size *
                                u->stream_config.frame_bytes,
                                &u->source->sample_spec);

    return pa_bytes_to_usec((uint64_t)delay_frames *
                            u->stream_config.frame_bytes,
                            &u->source->sample_spec);
}

static int
thread_read_period(struct userdata *u)
{
    pa_memchunk chunk;
    void *data;
    gssize bytes_read;

    pa_assert(u);
    pa_assert(u->source);
    pa_assert(u->stream);

    chunk.index = 0;
    chunk.length = u->period_bytes;
    chunk.memblock = pa_memblock_new(u->core->mempool, chunk.length);

    data = pa_memblock_acquire(chunk.memblock);
    bytes_read = audio_manager_stream_read(u->stream, data, chunk.length);
    pa_memblock_release(chunk.memblock);

    if (bytes_read < 0) {
        pa_log_error("audio-manager capture read failed: %zd", bytes_read);
        pa_memblock_unref(chunk.memblock);
        return -1;
    }

    if (bytes_read > 0) {
        chunk.length = (size_t)bytes_read;
        pa_source_post(u->source, &chunk);
    }

    pa_memblock_unref(chunk.memblock);
    return 0;
}

static bool
stream_config_matches_source(const AudioManagerStreamConfig *config,
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
open_capture_stream(struct userdata *u,
                    AudioManagerCaptureRole role,
                    AudioManagerStreamConfig *actual,
                    GError **error)
{
    AudioManagerStreamConfig requested;
    AudioManagerStream *stream;
    int ret;

    pa_assert(u);
    pa_assert(actual);

    requested = u->requested_config;
    requested.capture_role = role;

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
                        g_quark_from_static_string("pulse-audio-manager-source"),
                        -ret,
                        "failed to query negotiated capture stream: %d",
                        ret);
        return NULL;
    }

    return stream;
}

static int
restore_capture_role(struct userdata *u,
                     AudioManagerCaptureRole role,
                     bool restart)
{
    AudioManagerStreamConfig config = AUDIO_MANAGER_STREAM_CONFIG_INIT;
    AudioManagerStream *stream;
    GError *error = NULL;
    size_t period_bytes;
    int ret;

    pa_assert(u);

    stream = open_capture_stream(u, role, &config, &error);
    period_bytes = (size_t)config.period_size * config.frame_bytes;
    if (stream == NULL ||
        period_bytes == 0 ||
        !stream_config_matches_source(&config, &u->source->sample_spec)) {
        if (stream != NULL)
            audio_manager_stream_close(stream);
        pa_log_error("failed to restore audio-manager capture role %s%s%s",
                     pa_audio_manager_capture_role_to_string(role),
                     error != NULL ? ": " : "",
                     error != NULL ? error->message : "");
        g_clear_error(&error);
        return -EIO;
    }

    u->stream = stream;
    u->stream_config = config;
    u->period_bytes = period_bytes;
    u->capture_role = config.capture_role;

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
recover_stream(struct userdata *u)
{
    int ret;

    pa_assert(u);
    pa_assert(u->source);

    if (u->stream != NULL) {
        audio_manager_stream_close(u->stream);
        u->stream = NULL;
    }

    ret = restore_capture_role(u, u->capture_role, true);
    if (ret < 0) {
        pa_log_warn("failed to recover audio-manager capture stream: %d", ret);
        return ret;
    }

    pa_log_info("recovered audio-manager capture stream");
    return 0;
}

static void
thread_func(void *userdata)
{
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("audio-manager source thread starting");

    if (u->core->realtime_scheduling)
        pa_thread_make_realtime(u->core->realtime_priority);

    pa_thread_mq_install(&u->thread_mq);

    for (;;) {
        int ret;

        if (PA_SOURCE_IS_OPENED(u->source->thread_info.state)) {
            if (u->stream == NULL) {
                if (recover_stream(u) < 0) {
                    pa_rtpoll_set_timer_relative(u->rtpoll, STREAM_RECOVERY_INTERVAL_USEC);
                    goto poll;
                }
            }

            if (thread_read_period(u) < 0) {
                pa_log_warn("audio-manager capture stream failed, attempting recovery");
                if (recover_stream(u) < 0)
                    pa_rtpoll_set_timer_relative(u->rtpoll, STREAM_RECOVERY_INTERVAL_USEC);
                else
                    pa_rtpoll_set_timer_relative(u->rtpoll, 0);
                goto poll;
            }

            pa_rtpoll_set_timer_relative(u->rtpoll, 0);
        } else
            pa_rtpoll_set_timer_disabled(u->rtpoll);

poll:
        ret = pa_rtpoll_run(u->rtpoll);
        if (ret < 0)
            goto fail;
        if (ret == 0)
            goto finish;
    }

fail:
    pa_log_error("audio-manager source rtpoll failed, unloading module");
    pa_asyncmsgq_post(u->thread_mq.outq,
                      PA_MSGOBJECT(u->core),
                      PA_CORE_MESSAGE_UNLOAD_MODULE,
                      u->module,
                      0,
                      NULL,
                      NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("audio-manager source thread shutting down");
}

static int
suspend_stream(struct userdata *u)
{
    int ret;

    pa_assert(u);

    if (u->stream == NULL)
        return 0;

    ret = audio_manager_stream_stop(u->stream);
    if (ret < 0) {
        pa_log_error("failed to stop audio-manager capture stream: %d", ret);
        return ret;
    }

    pa_log_debug("audio-manager capture stream suspended");
    return 0;
}

static int
resume_stream(struct userdata *u)
{
    int ret;

    pa_assert(u);

    if (u->stream == NULL) {
        pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
        return 0;
    }

    ret = audio_manager_stream_start(u->stream);
    if (ret < 0) {
        pa_log_error("failed to start audio-manager capture stream: %d", ret);
        return ret;
    }

    pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
    pa_log_debug("audio-manager capture stream resumed");
    return 0;
}

static int
source_set_state_in_io_thread_cb(pa_source *source,
                                 pa_source_state_t new_state,
                                 pa_suspend_cause_t new_suspend_cause)
{
    struct userdata *u;

    pa_assert(source);
    pa_assert_se(u = source->userdata);
    (void)new_suspend_cause;

    if (new_state == source->thread_info.state)
        return 0;

    switch (new_state) {
    case PA_SOURCE_SUSPENDED:
        if (PA_SOURCE_IS_OPENED(source->thread_info.state))
            return suspend_stream(u);
        break;
    case PA_SOURCE_IDLE:
    case PA_SOURCE_RUNNING:
        if (source->thread_info.state == PA_SOURCE_SUSPENDED)
            return resume_stream(u);
        pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
        break;
    case PA_SOURCE_UNLINKED:
        if (PA_SOURCE_IS_OPENED(source->thread_info.state))
            suspend_stream(u);
        break;
    case PA_SOURCE_INIT:
    case PA_SOURCE_INVALID_STATE:
        break;
    }

    return 0;
}

static int
set_capture_role_in_io_thread(struct userdata *u,
                              AudioManagerCaptureRole role)
{
    AudioManagerStream *new_stream = NULL;
    AudioManagerStreamConfig new_config = AUDIO_MANAGER_STREAM_CONFIG_INIT;
    AudioManagerCaptureRole old_role;
    size_t new_period_bytes;
    bool was_opened;
    GError *error = NULL;
    int ret;

    pa_assert(u);
    pa_assert(u->source);

    if (role == u->capture_role)
        return 0;

    old_role = u->capture_role;
    was_opened = PA_SOURCE_IS_OPENED(u->source->thread_info.state);

    if (u->stream == NULL) {
        u->capture_role = role;
        if (was_opened)
            pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
        return 0;
    }

    if (was_opened) {
        ret = audio_manager_stream_stop(u->stream);
        if (ret < 0)
            return ret;
    }

    audio_manager_stream_close(u->stream);
    u->stream = NULL;

    new_stream = open_capture_stream(u, role, &new_config, &error);
    new_period_bytes = (size_t)new_config.period_size * new_config.frame_bytes;
    if (new_stream == NULL ||
        new_period_bytes == 0 ||
        !stream_config_matches_source(&new_config, &u->source->sample_spec)) {
        if (new_stream != NULL)
            audio_manager_stream_close(new_stream);

        if (error != NULL) {
            pa_log_warn("unable to switch audio-manager capture role to %s: %s",
                        pa_audio_manager_capture_role_to_string(role),
                        error->message);
            g_clear_error(&error);
        } else {
            pa_log_warn("capture role %s negotiated an incompatible source configuration",
                        pa_audio_manager_capture_role_to_string(role));
        }

        ret = restore_capture_role(u, old_role, was_opened);
        return ret < 0 ? ret : -EINVAL;
    }

    u->stream = new_stream;
    u->stream_config = new_config;
    u->period_bytes = new_period_bytes;
    u->capture_role = new_config.capture_role;

    if (was_opened) {
        ret = audio_manager_stream_start(u->stream);
        if (ret < 0) {
            audio_manager_stream_close(u->stream);
            u->stream = NULL;
            if (restore_capture_role(u, old_role, true) < 0)
                return -EIO;
            return ret;
        }
        pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
    }

    return 0;
}

static int
source_process_msg(pa_msgobject *object,
                   int code,
                   void *data,
                   int64_t offset,
                   pa_memchunk *chunk)
{
    pa_source *source = PA_SOURCE(object);
    struct userdata *u = source->userdata;

    if (code == PA_SOURCE_MESSAGE_GET_LATENCY) {
        *((pa_usec_t *)data) = stream_latency(u);
        return 0;
    }

    if (code == PA_AUDIO_MANAGER_SOURCE_MESSAGE_SET_CAPTURE_ROLE) {
        const struct capture_role_request *request = data;

        pa_assert(request);
        return set_capture_role_in_io_thread(u, request->role);
    }

    return pa_source_process_msg(object, code, data, offset, chunk);
}

static AudioManagerCaptureRole
choose_initial_capture_role(const AudioManagerCapabilities *capabilities)
{
    pa_assert(capabilities);

    if (capabilities->capture_roles &
        AUDIO_MANAGER_CAPTURE_ROLE_MASK(AUDIO_MANAGER_CAPTURE_ROLE_DEFAULT))
        return AUDIO_MANAGER_CAPTURE_ROLE_DEFAULT;
    if (capabilities->capture_roles &
        AUDIO_MANAGER_CAPTURE_ROLE_MASK(AUDIO_MANAGER_CAPTURE_ROLE_CAMCORDER))
        return AUDIO_MANAGER_CAPTURE_ROLE_CAMCORDER;
    if (capabilities->capture_roles &
        AUDIO_MANAGER_CAPTURE_ROLE_MASK(AUDIO_MANAGER_CAPTURE_ROLE_VOICE_RECOGNITION))
        return AUDIO_MANAGER_CAPTURE_ROLE_VOICE_RECOGNITION;
    if (capabilities->capture_roles &
        AUDIO_MANAGER_CAPTURE_ROLE_MASK(AUDIO_MANAGER_CAPTURE_ROLE_UNPROCESSED))
        return AUDIO_MANAGER_CAPTURE_ROLE_UNPROCESSED;

    return AUDIO_MANAGER_CAPTURE_ROLE_DEFAULT;
}

static AudioManagerInputDevice
choose_initial_input_device(const AudioManagerCapabilities *capabilities)
{
    pa_assert(capabilities);

    if (capabilities->input_devices &
        AUDIO_MANAGER_INPUT_DEVICE_MASK(AUDIO_MANAGER_INPUT_BUILTIN_MIC))
        return AUDIO_MANAGER_INPUT_BUILTIN_MIC;
    if (capabilities->input_devices &
        AUDIO_MANAGER_INPUT_DEVICE_MASK(AUDIO_MANAGER_INPUT_HEADSET_MIC))
        return AUDIO_MANAGER_INPUT_HEADSET_MIC;

    return AUDIO_MANAGER_INPUT_NONE;
}

pa_source *
pa_audio_manager_source_new_full(pa_module *module,
                                 const char *driver,
                                 const pa_audio_manager_source_options *options)
{
    struct userdata *u = NULL;
    AudioManagerStreamConfig requested = AUDIO_MANAGER_STREAM_CONFIG_INIT;
    AudioManagerSampleFormat am_format;
    AudioManagerCaptureRole role;
    AudioManagerInputDevice input_device;
    pa_source_new_data data;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    const char *input_device_name;
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
        pa_log_error("unsupported audio-manager source format '%s'",
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
    role = choose_initial_capture_role(capabilities);
    if (!(capabilities->capture_roles & AUDIO_MANAGER_CAPTURE_ROLE_MASK(role))) {
        pa_log_error("audio-manager backend exposes no capture role");
        goto fail;
    }

    if (options != NULL && options->initial_device != AUDIO_MANAGER_INPUT_NONE)
        input_device = options->initial_device;
    else
        input_device = choose_initial_input_device(capabilities);

    if (input_device == AUDIO_MANAGER_INPUT_NONE ||
        !(capabilities->input_devices & AUDIO_MANAGER_INPUT_DEVICE_MASK(input_device))) {
        pa_log_error("audio-manager backend exposes no usable local input device");
        goto fail;
    }

    input_device_name = pa_audio_manager_input_device_to_string(input_device);

    if (options == NULL || options->set_initial_device) {
        ret = audio_manager_set_input_device(pa_audio_manager_shared_manager(u->shared),
                                             input_device);
        if (ret < 0) {
            pa_log_error("failed to select audio-manager input device '%s': %d",
                         input_device_name, ret);
            goto fail;
        }
    }

    requested.direction = AUDIO_MANAGER_STREAM_CAPTURE;
    requested.format = am_format;
    requested.rate = sample_spec.rate;
    requested.channels = sample_spec.channels;
    requested.period_size = 0;
    requested.period_count = 0;
    requested.playback_role = AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT;
    requested.capture_role = role;
    u->requested_config = requested;
    u->capture_role = role;

    u->stream = audio_manager_stream_open(pa_audio_manager_shared_manager(u->shared),
                                          &requested,
                                          &error);
    if (u->stream == NULL) {
        pa_log_error("failed to open audio-manager capture stream: %s",
                     error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        goto fail;
    }

    u->stream_config = (AudioManagerStreamConfig) AUDIO_MANAGER_STREAM_CONFIG_INIT;
    ret = audio_manager_stream_get_config(u->stream, &u->stream_config);
    if (ret < 0) {
        pa_log_error("failed to query negotiated capture stream: %d", ret);
        goto fail;
    }

    if (!pa_audio_manager_sample_format_from_am(u->stream_config.format,
                                                &negotiated_format)) {
        pa_log_error("audio-manager returned unsupported capture format %d",
                     u->stream_config.format);
        goto fail;
    }

    sample_spec.format = negotiated_format;
    sample_spec.rate = u->stream_config.rate;
    sample_spec.channels = u->stream_config.channels;
    u->capture_role = u->stream_config.capture_role;

    if (channel_map.channels != sample_spec.channels &&
        pa_channel_map_init_auto(&channel_map,
                                 sample_spec.channels,
                                 PA_CHANNEL_MAP_ALSA) == NULL) {
        pa_log_error("failed to create channel map for %u capture channels",
                     sample_spec.channels);
        goto fail;
    }

    u->period_bytes = (size_t)u->stream_config.period_size *
                      u->stream_config.frame_bytes;
    if (u->period_bytes == 0) {
        pa_log_error("audio-manager returned an invalid capture period size");
        goto fail;
    }

    pa_source_new_data_init(&data);
    data.driver = driver;
    data.module = module;
    data.suspend_cause = PA_SUSPEND_IDLE;
    pa_source_new_data_set_name(&data, "audio-manager-input");
    pa_source_new_data_set_sample_spec(&data, &sample_spec);
    pa_source_new_data_set_channel_map(&data, &channel_map);

    if (options != NULL) {
        pa_device_port *port;
        void *state = NULL;

        data.card = options->card;

        if (options->ports != NULL) {
            PA_HASHMAP_FOREACH(port, options->ports, state) {
                if (port->direction != PA_DIRECTION_INPUT)
                    continue;

                pa_assert_se(pa_hashmap_put(data.ports,
                                            port->name,
                                            pa_device_port_ref(port)) >= 0);
            }
        }

        if (options->active_port != NULL)
            pa_source_new_data_set_port(&data, options->active_port);
    }

    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "sound");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_API, "audio-manager");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Input");
    pa_proplist_sets(data.proplist,
                     "audio_manager.capture_role",
                     pa_audio_manager_capture_role_to_string(u->capture_role));
    pa_proplist_sets(data.proplist, "audio_manager.input_device", input_device_name);

    u->source = pa_source_new(module->core,
                              &data,
                              PA_SOURCE_HARDWARE | PA_SOURCE_LATENCY);
    pa_source_new_data_done(&data);
    if (u->source == NULL) {
        pa_log_error("failed to create audio-manager source");
        goto fail;
    }

    u->source->userdata = u;
    u->source->parent.process_msg = source_process_msg;
    u->source->set_state_in_io_thread = source_set_state_in_io_thread_cb;
    if (options != NULL)
        u->source->set_port = options->set_port;

    pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);
    pa_source_set_rtpoll(u->source, u->rtpoll);
    pa_source_set_max_rewind(u->source, 0);
    pa_source_set_fixed_latency(u->source,
                                pa_bytes_to_usec((uint64_t)u->stream_config.period_size *
                                                 u->stream_config.frame_bytes,
                                                 &sample_spec));

    thread_name = pa_sprintf_malloc("audio-manager-source-%s", u->source->name);
    if ((u->thread = pa_thread_new(thread_name, thread_func, u)) == NULL) {
        pa_log_error("failed to create audio-manager source thread");
        goto fail;
    }
    pa_xfree(thread_name);
    thread_name = NULL;

    pa_source_put(u->source);

    pa_log_info("created audio-manager source '%s': %s %u Hz %u ch, period=%u x %u",
                u->source->name,
                pa_sample_format_to_string(sample_spec.format),
                sample_spec.rate,
                sample_spec.channels,
                u->stream_config.period_size,
                u->stream_config.period_count);

    return u->source;

fail:
    g_clear_error(&error);
    pa_xfree(thread_name);
    if (u != NULL)
        userdata_free(u);
    return NULL;
}

pa_source *
pa_audio_manager_source_new(pa_module *module,
                            const char *driver)
{
    return pa_audio_manager_source_new_full(module, driver, NULL);
}

int
pa_audio_manager_source_set_capture_role(pa_source *source,
                                         AudioManagerCaptureRole role)
{
    struct userdata *u;
    struct capture_role_request request;
    const AudioManagerCapabilities *capabilities;
    int ret;

    pa_source_assert_ref(source);
    pa_assert_se(u = source->userdata);

    capabilities = pa_audio_manager_shared_capabilities(u->shared);
    if (!(capabilities->capture_roles & AUDIO_MANAGER_CAPTURE_ROLE_MASK(role)))
        return -ENOTSUP;

    request.role = role;
    ret = pa_asyncmsgq_send(u->thread_mq.inq,
                            PA_MSGOBJECT(source),
                            PA_AUDIO_MANAGER_SOURCE_MESSAGE_SET_CAPTURE_ROLE,
                            &request,
                            0,
                            NULL);
    if (ret < 0)
        return ret;

    pa_source_set_fixed_latency(source,
                                pa_bytes_to_usec((uint64_t)u->stream_config.period_size *
                                                 u->stream_config.frame_bytes,
                                                 &source->sample_spec));
    pa_proplist_sets(source->proplist,
                     "audio_manager.capture_role",
                     pa_audio_manager_capture_role_to_string(role));

    pa_log_info("audio-manager capture role changed to %s",
                pa_audio_manager_capture_role_to_string(role));
    return 0;
}

AudioManagerCaptureRole
pa_audio_manager_source_get_capture_role(pa_source *source)
{
    struct userdata *u;

    pa_source_assert_ref(source);
    pa_assert_se(u = source->userdata);
    return u->capture_role;
}

void
pa_audio_manager_source_free(pa_source *source)
{
    struct userdata *u;

    pa_source_assert_ref(source);
    pa_assert_se(u = source->userdata);

    userdata_free(u);
}
