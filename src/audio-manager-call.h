/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#ifndef AUDIO_MANAGER_CALL_H
#define AUDIO_MANAGER_CALL_H

#include "audio-manager-shared.h"

#include <stdbool.h>

#include <pulsecore/core.h>
#include <pulsecore/card.h>
#include <pulsecore/module.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>

typedef struct pa_audio_manager_call_pcm pa_audio_manager_call_pcm;

pa_audio_manager_call_pcm *
pa_audio_manager_call_pcm_new(pa_module *module,
                              pa_audio_manager_shared *shared,
                              pa_card *card,
                              const char *uplink_sink_name,
                              const char *downlink_source_name);

void
pa_audio_manager_call_pcm_free(pa_audio_manager_call_pcm *call);

pa_sink *
pa_audio_manager_call_pcm_uplink_sink(pa_audio_manager_call_pcm *call);

pa_source *
pa_audio_manager_call_pcm_downlink_source(pa_audio_manager_call_pcm *call);

bool
pa_audio_manager_call_pcm_failed(pa_audio_manager_call_pcm *call);

#endif /* AUDIO_MANAGER_CALL_H */
