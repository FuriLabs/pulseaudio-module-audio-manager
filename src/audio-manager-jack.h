/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#ifndef AUDIO_MANAGER_JACK_H
#define AUDIO_MANAGER_JACK_H

#include <stdbool.h>

#include <pulsecore/core.h>

typedef struct pa_audio_manager_jack pa_audio_manager_jack;

typedef void (*pa_audio_manager_jack_changed_cb)(void *userdata,
                                                 bool known,
                                                 bool headphone,
                                                 bool microphone);

pa_audio_manager_jack *
pa_audio_manager_jack_new(pa_core *core,
                          const char *detection,
                          const char *evdev_path,
                          const char *extcon_path,
                          pa_audio_manager_jack_changed_cb changed_cb,
                          void *userdata);

void
pa_audio_manager_jack_free(pa_audio_manager_jack *jack);

#endif /* AUDIO_MANAGER_JACK_H */
