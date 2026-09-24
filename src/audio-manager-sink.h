/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#ifndef AUDIO_MANAGER_SINK_H
#define AUDIO_MANAGER_SINK_H

#include <stdbool.h>

#include "audio-manager-shared.h"

#include <pulsecore/core.h>
#include <pulsecore/card.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/module.h>
#include <pulsecore/sink.h>

typedef struct pa_audio_manager_sink_options {
    pa_audio_manager_shared *shared;
    pa_card *card;
    pa_hashmap *ports;
    const char *active_port;
    int (*set_port)(pa_sink *sink, pa_device_port *port);
    AudioManagerOutputDevice initial_device;
    bool set_initial_device;
} pa_audio_manager_sink_options;

#define PA_AUDIO_MANAGER_SINK_OPTIONS_INIT { .initial_device = AUDIO_MANAGER_OUTPUT_NONE, .set_initial_device = true }

pa_sink *
pa_audio_manager_sink_new(pa_module *module,
                          const char *driver);

pa_sink *
pa_audio_manager_sink_new_full(pa_module *module,
                               const char *driver,
                               const pa_audio_manager_sink_options *options);

int
pa_audio_manager_sink_set_playback_role(pa_sink *sink,
                                        AudioManagerPlaybackRole role);

AudioManagerPlaybackRole
pa_audio_manager_sink_get_playback_role(pa_sink *sink);

void
pa_audio_manager_sink_free(pa_sink *sink);

#endif /* AUDIO_MANAGER_SINK_H */
