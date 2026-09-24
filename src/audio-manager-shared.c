/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include "audio-manager-shared.h"

#include <glib.h>

#include <pulse/xmalloc.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/refcnt.h>
#include <pulsecore/shared.h>

#define PA_AUDIO_MANAGER_FURIOS_CONFIG "/usr/lib/furios/device/audio-manager.conf"
#define PA_AUDIO_MANAGER_SYSTEM_CONFIG "/etc/audio-manager/audio-manager.conf"
#define PA_AUDIO_MANAGER_DEFAULT_CONFIG "/usr/share/audio-manager/audio-manager.conf"

#define SHARED_NAME "audio-manager.shared.v1"

struct pa_audio_manager_shared {
    PA_REFCNT_DECLARE;

    pa_core *core;
    char *config_path;

    GMainContext *context;
    GMainLoop *loop;
    GThread *thread;

    AudioManager *manager;
    AudioManagerCapabilities capabilities;
};

static const char *
default_config_path(void)
{
    if (g_file_test(PA_AUDIO_MANAGER_FURIOS_CONFIG, G_FILE_TEST_IS_REGULAR))
        return PA_AUDIO_MANAGER_FURIOS_CONFIG;

    if (g_file_test(PA_AUDIO_MANAGER_SYSTEM_CONFIG, G_FILE_TEST_IS_REGULAR))
        return PA_AUDIO_MANAGER_SYSTEM_CONFIG;

    return PA_AUDIO_MANAGER_DEFAULT_CONFIG;
}

static gpointer
main_context_thread(gpointer data)
{
    pa_audio_manager_shared *shared = data;

    g_main_context_push_thread_default(shared->context);
    g_main_loop_run(shared->loop);
    g_main_context_pop_thread_default(shared->context);

    return NULL;
}

static void
shared_free(pa_audio_manager_shared *shared)
{
    pa_assert(shared);

    if (shared->loop != NULL) {
        g_main_loop_quit(shared->loop);
        g_main_context_wakeup(shared->context);
    }

    if (shared->thread != NULL)
        g_thread_join(shared->thread);

    if (shared->manager != NULL)
        audio_manager_free(shared->manager);

    if (shared->loop != NULL)
        g_main_loop_unref(shared->loop);

    if (shared->context != NULL)
        g_main_context_unref(shared->context);

    pa_xfree(shared->config_path);
    pa_xfree(shared);
}

pa_audio_manager_shared *
pa_audio_manager_shared_get(pa_core *core)
{
    pa_audio_manager_shared *shared;
    AudioManagerConfig config = AUDIO_MANAGER_CONFIG_INIT;
    GError *error = NULL;
    const char *config_path;
    int ret;

    pa_assert(core);

    config_path = default_config_path();

    if ((shared = pa_shared_get(core, SHARED_NAME)) != NULL) {
        if (!pa_streq(shared->config_path, config_path)) {
            pa_log_error("audio-manager is already open with config '%s', requested '%s'",
                         shared->config_path, config_path);
            return NULL;
        }

        return pa_audio_manager_shared_ref(shared);
    }

    shared = pa_xnew0(pa_audio_manager_shared, 1);
    PA_REFCNT_INIT(shared);
    shared->core = core;
    shared->config_path = pa_xstrdup(config_path);
    shared->context = g_main_context_new();
    shared->loop = g_main_loop_new(shared->context, false);

    config.config_path = shared->config_path;
    config.main_context = shared->context;

    shared->manager = audio_manager_new(&config, &error);
    if (shared->manager == NULL) {
        pa_log_error("failed to create audio-manager: %s",
                     error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        shared_free(shared);
        return NULL;
    }

    shared->capabilities = (AudioManagerCapabilities) AUDIO_MANAGER_CAPABILITIES_INIT;

    ret = audio_manager_get_capabilities(shared->manager,
                                         &shared->capabilities);
    if (ret < 0) {
        pa_log_error("failed to query audio-manager capabilities: %d", ret);
        shared_free(shared);
        return NULL;
    }

    if (!(shared->capabilities.flags & AUDIO_MANAGER_CAP_STREAMS)) {
        pa_log_error("audio-manager backend '%s' does not support generic PCM streams",
                     audio_manager_get_backend_name(shared->manager));
        shared_free(shared);
        return NULL;
    }

    shared->thread = g_thread_new("audio-manager-main", main_context_thread, shared);
    if (shared->thread == NULL) {
        pa_log_error("failed to start audio-manager GLib main context thread");
        shared_free(shared);
        return NULL;
    }

    if (pa_shared_set(core, SHARED_NAME, shared) < 0) {
        pa_log_error("failed to register shared audio-manager instance");
        shared_free(shared);
        return NULL;
    }

    pa_log_info("opened audio-manager backend '%s' using '%s'",
                audio_manager_get_backend_name(shared->manager),
                shared->config_path);

    return shared;
}

pa_audio_manager_shared *
pa_audio_manager_shared_ref(pa_audio_manager_shared *shared)
{
    pa_assert(shared);
    pa_assert(PA_REFCNT_VALUE(shared) >= 1);

    PA_REFCNT_INC(shared);
    return shared;
}

void
pa_audio_manager_shared_unref(pa_audio_manager_shared *shared)
{
    pa_assert(shared);
    pa_assert(PA_REFCNT_VALUE(shared) >= 1);

    if (PA_REFCNT_DEC(shared) > 0)
        return;

    pa_assert_se(pa_shared_remove(shared->core, SHARED_NAME) >= 0);
    shared_free(shared);
}

AudioManager *
pa_audio_manager_shared_manager(pa_audio_manager_shared *shared)
{
    pa_assert(shared);

    return shared->manager;
}

const AudioManagerCapabilities *
pa_audio_manager_shared_capabilities(pa_audio_manager_shared *shared)
{
    pa_assert(shared);

    return &shared->capabilities;
}
