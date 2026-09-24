/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#ifndef AUDIO_MANAGER_SOURCE_H
#define AUDIO_MANAGER_SOURCE_H

#include <stdbool.h>

#include "audio-manager-shared.h"

#include <pulsecore/core.h>
#include <pulsecore/card.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/module.h>
#include <pulsecore/source.h>

typedef struct pa_audio_manager_source_options {
    pa_audio_manager_shared *shared;
    pa_card *card;
    pa_hashmap *ports;
    const char *active_port;
    int (*set_port)(pa_source *source, pa_device_port *port);
    AudioManagerInputDevice initial_device;
    bool set_initial_device;
} pa_audio_manager_source_options;

#define PA_AUDIO_MANAGER_SOURCE_OPTIONS_INIT { .initial_device = AUDIO_MANAGER_INPUT_NONE, .set_initial_device = true }

pa_source *
pa_audio_manager_source_new(pa_module *module,
                            const char *driver);

pa_source *
pa_audio_manager_source_new_full(pa_module *module,
                                 const char *driver,
                                 const pa_audio_manager_source_options *options);

int
pa_audio_manager_source_set_capture_role(pa_source *source,
                                         AudioManagerCaptureRole role);

AudioManagerCaptureRole
pa_audio_manager_source_get_capture_role(pa_source *source);

void
pa_audio_manager_source_free(pa_source *source);

#endif /* AUDIO_MANAGER_SOURCE_H */
