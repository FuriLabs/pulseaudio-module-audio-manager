/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#ifndef AUDIO_MANAGER_SHARED_H
#define AUDIO_MANAGER_SHARED_H

#include <audio-manager/audio-manager.h>
#include <pulsecore/core.h>

typedef struct pa_audio_manager_shared pa_audio_manager_shared;

pa_audio_manager_shared *
pa_audio_manager_shared_get(pa_core *core);

pa_audio_manager_shared *
pa_audio_manager_shared_ref(pa_audio_manager_shared *shared);

void
pa_audio_manager_shared_unref(pa_audio_manager_shared *shared);

AudioManager *
pa_audio_manager_shared_manager(pa_audio_manager_shared *shared);

const AudioManagerCapabilities *
pa_audio_manager_shared_capabilities(pa_audio_manager_shared *shared);

#endif /* AUDIO_MANAGER_SHARED_H */
