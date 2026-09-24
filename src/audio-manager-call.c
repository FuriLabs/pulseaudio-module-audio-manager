/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include "audio-manager-call.h"

#include "audio-manager-util.h"

#include <stdbool.h>
#include <stdint.h>

#include <pulse/proplist.h>
#include <pulse/rtclock.h>
#include <pulse/sample.h>
#include <pulse/util.h>
#include <pulse/xmalloc.h>

#include <pulsecore/atomic.h>
#include <pulsecore/core.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>

struct call_direction {
    struct pa_audio_manager_call_pcm *owner;
    AudioManagerCallStream *stream;
    AudioManagerCallStreamConfig config;
    size_t period_bytes;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    bool thread_mq_initialized;
    pa_rtpoll *rtpoll;

    pa_sink *sink;
    pa_source *source;
};

struct pa_audio_manager_call_pcm {
    pa_module *module;
    pa_core *core;
    pa_audio_manager_shared *shared;
    pa_card *card;

    struct call_direction uplink;
    struct call_direction downlink;
    pa_atomic_t failed;
};

static pa_usec_t
call_buffer_latency(const AudioManagerCallStreamConfig *config,
                    const pa_sample_spec *sample_spec)
{
    uint64_t bytes;

    pa_assert(config);
    pa_assert(sample_spec);

    bytes = (uint64_t)config->period_size *
            config->period_count *
            config->frame_bytes;
    return pa_bytes_to_usec(bytes, sample_spec);
}

static int
call_stream_start(struct call_direction *direction)
{
    int ret;

    pa_assert(direction);
    pa_assert(direction->stream);

    ret = audio_manager_call_stream_start(direction->stream);
    if (ret < 0)
        pa_log_error("failed to start audio-manager call stream: %d", ret);
    return ret;
}

static int
call_stream_stop(struct call_direction *direction)
{
    int ret;

    pa_assert(direction);
    pa_assert(direction->stream);

    ret = audio_manager_call_stream_stop(direction->stream);
    if (ret < 0)
        pa_log_warn("failed to stop audio-manager call stream: %d", ret);
    return ret;
}

static void
uplink_process_rewind(struct call_direction *direction)
{
    pa_assert(direction);
    pa_assert(direction->sink);

    direction->sink->thread_info.rewind_nbytes = 0;
    pa_sink_process_rewind(direction->sink, 0);
}

static int
uplink_write_period(struct call_direction *direction)
{
    pa_memchunk chunk;
    size_t offset = 0;

    pa_assert(direction);
    pa_assert(direction->sink);
    pa_assert(direction->stream);

    pa_sink_render_full(direction->sink, direction->period_bytes, &chunk);

    while (offset < chunk.length) {
        const uint8_t *data;
        gssize written;

        data = pa_memblock_acquire_chunk(&chunk);
        written = audio_manager_call_stream_write(direction->stream,
                                                  data + offset,
                                                  chunk.length - offset);
        pa_memblock_release(chunk.memblock);

        if (written <= 0) {
            pa_log_error("audio-manager call uplink write failed: %zd", written);
            pa_memblock_unref(chunk.memblock);
            return -1;
        }

        offset += (size_t)written;
    }

    pa_memblock_unref(chunk.memblock);
    return 0;
}

static void
uplink_thread_func(void *userdata)
{
    struct call_direction *direction = userdata;
    pa_core *core;

    pa_assert(direction);
    pa_assert(direction->owner);
    core = direction->owner->core;

    pa_log_debug("audio-manager call uplink thread starting");

    if (core->realtime_scheduling)
        pa_thread_make_realtime(core->realtime_priority);

    pa_thread_mq_install(&direction->thread_mq);

    for (;;) {
        int ret;

        if (PA_SINK_IS_OPENED(direction->sink->thread_info.state)) {
            if (PA_UNLIKELY(direction->sink->thread_info.rewind_requested))
                uplink_process_rewind(direction);

            if (uplink_write_period(direction) < 0)
                goto fail;

            pa_rtpoll_set_timer_relative(direction->rtpoll, 0);
        } else
            pa_rtpoll_set_timer_disabled(direction->rtpoll);

        ret = pa_rtpoll_run(direction->rtpoll);
        if (ret < 0)
            goto fail;
        if (ret == 0)
            goto finish;
    }

fail:
    pa_atomic_inc(&direction->owner->failed);
    pa_asyncmsgq_wait_for(direction->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("audio-manager call uplink thread shutting down");
}

static int
uplink_set_state_in_io_thread_cb(pa_sink *sink,
                                 pa_sink_state_t new_state,
                                 pa_suspend_cause_t new_suspend_cause)
{
    struct call_direction *direction;

    pa_assert(sink);
    pa_assert_se(direction = sink->userdata);
    (void)new_suspend_cause;

    if (new_state == sink->thread_info.state)
        return 0;

    switch (new_state) {
    case PA_SINK_SUSPENDED:
        if (PA_SINK_IS_OPENED(sink->thread_info.state))
            return call_stream_stop(direction);
        break;
    case PA_SINK_IDLE:
    case PA_SINK_RUNNING:
        if (sink->thread_info.state == PA_SINK_SUSPENDED) {
            int ret = call_stream_start(direction);
            if (ret < 0)
                return ret;
        }
        pa_rtpoll_set_timer_absolute(direction->rtpoll, pa_rtclock_now());
        break;
    case PA_SINK_UNLINKED:
        if (PA_SINK_IS_OPENED(sink->thread_info.state))
            call_stream_stop(direction);
        break;
    case PA_SINK_INIT:
    case PA_SINK_INVALID_STATE:
        break;
    }

    return 0;
}

static int
uplink_process_msg(pa_msgobject *object,
                   int code,
                   void *data,
                   int64_t offset,
                   pa_memchunk *chunk)
{
    pa_sink *sink = PA_SINK(object);
    struct call_direction *direction = sink->userdata;

    if (code == PA_SINK_MESSAGE_GET_LATENCY) {
        *((pa_usec_t *)data) = call_buffer_latency(&direction->config,
                                                   &sink->sample_spec);
        return 0;
    }

    return pa_sink_process_msg(object, code, data, offset, chunk);
}

static int
downlink_read_period(struct call_direction *direction)
{
    pa_memchunk chunk;
    void *data;
    gssize bytes_read;

    pa_assert(direction);
    pa_assert(direction->source);
    pa_assert(direction->stream);

    chunk.index = 0;
    chunk.length = direction->period_bytes;
    chunk.memblock = pa_memblock_new(direction->owner->core->mempool, chunk.length);

    data = pa_memblock_acquire(chunk.memblock);
    bytes_read = audio_manager_call_stream_read(direction->stream,
                                                data,
                                                chunk.length);
    pa_memblock_release(chunk.memblock);

    if (bytes_read < 0) {
        pa_log_error("audio-manager call downlink read failed: %zd", bytes_read);
        pa_memblock_unref(chunk.memblock);
        return -1;
    }

    if (bytes_read > 0) {
        chunk.length = (size_t)bytes_read;
        pa_source_post(direction->source, &chunk);
    }

    pa_memblock_unref(chunk.memblock);
    return 0;
}

static void
downlink_thread_func(void *userdata)
{
    struct call_direction *direction = userdata;
    pa_core *core;

    pa_assert(direction);
    pa_assert(direction->owner);
    core = direction->owner->core;

    pa_log_debug("audio-manager call downlink thread starting");

    if (core->realtime_scheduling)
        pa_thread_make_realtime(core->realtime_priority);

    pa_thread_mq_install(&direction->thread_mq);

    for (;;) {
        int ret;

        if (PA_SOURCE_IS_OPENED(direction->source->thread_info.state)) {
            if (downlink_read_period(direction) < 0)
                goto fail;
            pa_rtpoll_set_timer_relative(direction->rtpoll, 0);
        } else
            pa_rtpoll_set_timer_disabled(direction->rtpoll);

        ret = pa_rtpoll_run(direction->rtpoll);
        if (ret < 0)
            goto fail;
        if (ret == 0)
            goto finish;
    }

fail:
    pa_atomic_inc(&direction->owner->failed);
    pa_asyncmsgq_wait_for(direction->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("audio-manager call downlink thread shutting down");
}

static int
downlink_set_state_in_io_thread_cb(pa_source *source,
                                   pa_source_state_t new_state,
                                   pa_suspend_cause_t new_suspend_cause)
{
    struct call_direction *direction;

    pa_assert(source);
    pa_assert_se(direction = source->userdata);
    (void)new_suspend_cause;

    if (new_state == source->thread_info.state)
        return 0;

    switch (new_state) {
    case PA_SOURCE_SUSPENDED:
        if (PA_SOURCE_IS_OPENED(source->thread_info.state))
            return call_stream_stop(direction);
        break;
    case PA_SOURCE_IDLE:
    case PA_SOURCE_RUNNING:
        if (source->thread_info.state == PA_SOURCE_SUSPENDED) {
            int ret = call_stream_start(direction);
            if (ret < 0)
                return ret;
        }
        pa_rtpoll_set_timer_absolute(direction->rtpoll, pa_rtclock_now());
        break;
    case PA_SOURCE_UNLINKED:
        if (PA_SOURCE_IS_OPENED(source->thread_info.state))
            call_stream_stop(direction);
        break;
    case PA_SOURCE_INIT:
    case PA_SOURCE_INVALID_STATE:
        break;
    }

    return 0;
}

static int
downlink_process_msg(pa_msgobject *object,
                     int code,
                     void *data,
                     int64_t offset,
                     pa_memchunk *chunk)
{
    pa_source *source = PA_SOURCE(object);
    struct call_direction *direction = source->userdata;

    if (code == PA_SOURCE_MESSAGE_GET_LATENCY) {
        *((pa_usec_t *)data) = call_buffer_latency(&direction->config,
                                                   &source->sample_spec);
        return 0;
    }

    return pa_source_process_msg(object, code, data, offset, chunk);
}

static bool
call_config_to_sample_spec(const AudioManagerCallStreamConfig *config,
                           pa_sample_spec *sample_spec,
                           pa_channel_map *channel_map)
{
    pa_sample_format_t format;

    pa_assert(config);
    pa_assert(sample_spec);
    pa_assert(channel_map);

    if (!pa_audio_manager_sample_format_from_am(config->format, &format))
        return false;

    sample_spec->format = format;
    sample_spec->rate = config->rate;
    sample_spec->channels = config->channels;

    return pa_sample_spec_valid(sample_spec) &&
           pa_channel_map_init_auto(channel_map,
                                    sample_spec->channels,
                                    PA_CHANNEL_MAP_ALSA) != NULL;
}

static int
open_direction_stream(struct pa_audio_manager_call_pcm *call,
                      struct call_direction *direction,
                      AudioManagerCallStreamDirection stream_direction)
{
    GError *error = NULL;
    int ret;

    pa_assert(call);
    pa_assert(direction);

    direction->owner = call;
    direction->rtpoll = pa_rtpoll_new();
    if (direction->rtpoll == NULL)
        return -1;
    pa_thread_mq_init(&direction->thread_mq, call->core->mainloop, direction->rtpoll);
    direction->thread_mq_initialized = true;

    direction->stream = audio_manager_call_stream_open(pa_audio_manager_shared_manager(call->shared),
                                                       stream_direction,
                                                       &error);
    if (direction->stream == NULL) {
        pa_log_error("failed to open audio-manager call %s stream: %s",
                     stream_direction == AUDIO_MANAGER_CALL_STREAM_UPLINK ? "uplink" : "downlink",
                     error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        return -1;
    }

    direction->config = (AudioManagerCallStreamConfig) AUDIO_MANAGER_CALL_STREAM_CONFIG_INIT;
    ret = audio_manager_call_stream_get_config(direction->stream, &direction->config);
    if (ret < 0) {
        pa_log_error("failed to query audio-manager call stream configuration: %d", ret);
        return -1;
    }

    direction->period_bytes = (size_t)direction->config.period_size *
                              direction->config.frame_bytes;
    if (direction->period_bytes == 0) {
        pa_log_error("audio-manager call stream returned an invalid period size");
        return -1;
    }

    return 0;
}

static int
create_uplink_sink(struct pa_audio_manager_call_pcm *call,
                   const char *name)
{
    struct call_direction *direction = &call->uplink;
    pa_sink_new_data data;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    char *thread_name = NULL;

    if (!call_config_to_sample_spec(&direction->config, &sample_spec, &channel_map)) {
        pa_log_error("audio-manager call uplink returned an unsupported PCM format");
        return -1;
    }

    pa_sink_new_data_init(&data);
    data.driver = __FILE__;
    data.module = call->module;
    data.card = call->card;
    data.suspend_cause = PA_SUSPEND_IDLE;
    pa_sink_new_data_set_name(&data, name);
    pa_sink_new_data_set_sample_spec(&data, &sample_spec);
    pa_sink_new_data_set_channel_map(&data, &channel_map);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "abstract");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_API, "audio-manager");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Voice Call Uplink");
    pa_proplist_sets(data.proplist, "audio_manager.call.direction", "uplink");
    pa_proplist_sets(data.proplist, "audio_manager.call.transport", "hostful");

    direction->sink = pa_sink_new(call->core,
                                  &data,
                                  PA_SINK_LATENCY);
    pa_sink_new_data_done(&data);
    if (direction->sink == NULL)
        return -1;

    direction->sink->userdata = direction;
    direction->sink->parent.process_msg = uplink_process_msg;
    direction->sink->set_state_in_io_thread = uplink_set_state_in_io_thread_cb;
    pa_sink_set_asyncmsgq(direction->sink, direction->thread_mq.inq);
    pa_sink_set_rtpoll(direction->sink, direction->rtpoll);
    pa_sink_set_max_rewind(direction->sink, 0);
    pa_sink_set_max_request(direction->sink, direction->period_bytes);
    pa_sink_set_fixed_latency(direction->sink,
                              call_buffer_latency(&direction->config, &sample_spec));

    thread_name = pa_sprintf_malloc("audio-manager-call-uplink");
    direction->thread = pa_thread_new(thread_name, uplink_thread_func, direction);
    pa_xfree(thread_name);
    if (direction->thread == NULL)
        return -1;

    pa_sink_put(direction->sink);
    return 0;
}

static int
create_downlink_source(struct pa_audio_manager_call_pcm *call,
                       const char *name)
{
    struct call_direction *direction = &call->downlink;
    pa_source_new_data data;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    char *thread_name = NULL;

    if (!call_config_to_sample_spec(&direction->config, &sample_spec, &channel_map)) {
        pa_log_error("audio-manager downlink returned an unsupported PCM format");
        return -1;
    }

    pa_source_new_data_init(&data);
    data.driver = __FILE__;
    data.module = call->module;
    data.card = call->card;
    data.suspend_cause = PA_SUSPEND_IDLE;
    pa_source_new_data_set_name(&data, name);
    pa_source_new_data_set_sample_spec(&data, &sample_spec);
    pa_source_new_data_set_channel_map(&data, &channel_map);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "abstract");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_API, "audio-manager");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Voice Call Downlink");
    pa_proplist_sets(data.proplist, "audio_manager.call.direction", "downlink");
    pa_proplist_sets(data.proplist, "audio_manager.call.transport", "hostful");

    direction->source = pa_source_new(call->core,
                                      &data,
                                      PA_SOURCE_LATENCY);
    pa_source_new_data_done(&data);
    if (direction->source == NULL)
        return -1;

    direction->source->userdata = direction;
    direction->source->parent.process_msg = downlink_process_msg;
    direction->source->set_state_in_io_thread = downlink_set_state_in_io_thread_cb;
    pa_source_set_asyncmsgq(direction->source, direction->thread_mq.inq);
    pa_source_set_rtpoll(direction->source, direction->rtpoll);
    pa_source_set_max_rewind(direction->source, 0);
    pa_source_set_fixed_latency(direction->source,
                                call_buffer_latency(&direction->config, &sample_spec));

    thread_name = pa_sprintf_malloc("audio-manager-call-downlink");
    direction->thread = pa_thread_new(thread_name, downlink_thread_func, direction);
    pa_xfree(thread_name);
    if (direction->thread == NULL)
        return -1;

    pa_source_put(direction->source);
    return 0;
}

static void
free_direction(struct call_direction *direction)
{
    if (direction == NULL)
        return;

    if (direction->source != NULL)
        pa_source_unlink(direction->source);
    if (direction->sink != NULL)
        pa_sink_unlink(direction->sink);

    if (direction->thread != NULL) {
        pa_asyncmsgq_send(direction->thread_mq.inq,
                          NULL,
                          PA_MESSAGE_SHUTDOWN,
                          NULL,
                          0,
                          NULL);
        pa_thread_free(direction->thread);
        direction->thread = NULL;
    }

    if (direction->thread_mq_initialized) {
        pa_thread_mq_done(&direction->thread_mq);
        direction->thread_mq_initialized = false;
    }

    if (direction->stream != NULL) {
        audio_manager_call_stream_stop(direction->stream);
        audio_manager_call_stream_close(direction->stream);
        direction->stream = NULL;
    }

    if (direction->source != NULL) {
        pa_source_unref(direction->source);
        direction->source = NULL;
    }
    if (direction->sink != NULL) {
        pa_sink_unref(direction->sink);
        direction->sink = NULL;
    }

    if (direction->rtpoll != NULL) {
        pa_rtpoll_free(direction->rtpoll);
        direction->rtpoll = NULL;
    }
}

pa_audio_manager_call_pcm *
pa_audio_manager_call_pcm_new(pa_module *module,
                              pa_audio_manager_shared *shared,
                              pa_card *card,
                              const char *uplink_sink_name,
                              const char *downlink_source_name)
{
    pa_audio_manager_call_pcm *call;

    pa_assert(module);
    pa_assert(shared);
    pa_assert(card);

    call = pa_xnew0(pa_audio_manager_call_pcm, 1);
    call->module = module;
    call->core = module->core;
    call->shared = pa_audio_manager_shared_ref(shared);
    call->card = card;

    if (open_direction_stream(call,
                              &call->downlink,
                              AUDIO_MANAGER_CALL_STREAM_DOWNLINK) < 0)
        goto fail;

    if (open_direction_stream(call,
                              &call->uplink,
                              AUDIO_MANAGER_CALL_STREAM_UPLINK) < 0)
        goto fail;

    if (create_downlink_source(call,
                               downlink_source_name != NULL ?
                               downlink_source_name : "audio-manager-call-downlink") < 0)
        goto fail;

    if (create_uplink_sink(call,
                           uplink_sink_name != NULL ?
                           uplink_sink_name : "audio-manager-call-uplink") < 0)
        goto fail;

    pa_log_info("created call host PCM endpoints: uplink='%s' downlink='%s'",
                call->uplink.sink->name,
                call->downlink.source->name);
    return call;

fail:
    pa_audio_manager_call_pcm_free(call);
    return NULL;
}

void
pa_audio_manager_call_pcm_free(pa_audio_manager_call_pcm *call)
{
    if (call == NULL)
        return;

    free_direction(&call->uplink);
    free_direction(&call->downlink);

    if (call->shared != NULL)
        pa_audio_manager_shared_unref(call->shared);

    pa_xfree(call);
}

pa_sink *
pa_audio_manager_call_pcm_uplink_sink(pa_audio_manager_call_pcm *call)
{
    return call != NULL ? call->uplink.sink : NULL;
}

pa_source *
pa_audio_manager_call_pcm_downlink_source(pa_audio_manager_call_pcm *call)
{
    return call != NULL ? call->downlink.source : NULL;
}

bool
pa_audio_manager_call_pcm_failed(pa_audio_manager_call_pcm *call)
{
    return call != NULL && pa_atomic_load(&call->failed) > 0;
}
