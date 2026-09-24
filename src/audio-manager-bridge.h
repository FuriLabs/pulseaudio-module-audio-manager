/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#ifndef AUDIO_MANAGER_BRIDGE_H
#define AUDIO_MANAGER_BRIDGE_H

#include <stdbool.h>

#include <pulsecore/core.h>
#include <pulsecore/card.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>

typedef enum pa_audio_manager_bridge_target {
    PA_AUDIO_MANAGER_BRIDGE_NONE = 0,
    PA_AUDIO_MANAGER_BRIDGE_LOCAL,
    PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH,
    PA_AUDIO_MANAGER_BRIDGE_USB,
} pa_audio_manager_bridge_target;

typedef struct pa_audio_manager_bridge pa_audio_manager_bridge;

typedef struct pa_audio_manager_bridge_device {
    pa_sink *sink;
    pa_source *source;
} pa_audio_manager_bridge_device;

bool
pa_audio_manager_bridge_find_device_on_card(pa_core *core,
                                            pa_audio_manager_bridge_target target,
                                            const char *card_name,
                                            pa_audio_manager_bridge_device *device);

bool
pa_audio_manager_bridge_target_available(pa_core *core,
                                         pa_audio_manager_bridge_target target);

bool
pa_audio_manager_bridge_bluetooth_hfp_available(pa_core *core);

bool
pa_audio_manager_bridge_prepare_bluetooth(pa_core *core,
                                          char **card_name,
                                          char **restore_profile);

void
pa_audio_manager_bridge_restore_bluetooth(pa_core *core,
                                          const char *card_name,
                                          const char *restore_profile);

bool
pa_audio_manager_bridge_sink_is_target(pa_sink *sink,
                                       pa_audio_manager_bridge_target target);

bool
pa_audio_manager_bridge_source_is_target(pa_source *source,
                                         pa_audio_manager_bridge_target target);

pa_audio_manager_bridge *
pa_audio_manager_bridge_new(pa_core *core,
                            pa_audio_manager_bridge_target target,
                            pa_source *call_downlink,
                            pa_sink *call_uplink,
                            unsigned latency_msec);

pa_audio_manager_bridge *
pa_audio_manager_bridge_new_on_card(pa_core *core,
                                    pa_audio_manager_bridge_target target,
                                    const char *card_name,
                                    pa_source *call_downlink,
                                    pa_sink *call_uplink,
                                    unsigned latency_msec);

void
pa_audio_manager_bridge_free(pa_audio_manager_bridge *bridge);

bool
pa_audio_manager_bridge_uses_sink(const pa_audio_manager_bridge *bridge,
                                  const pa_sink *sink);

bool
pa_audio_manager_bridge_uses_source(const pa_audio_manager_bridge *bridge,
                                    const pa_source *source);

const char *
pa_audio_manager_bridge_target_to_string(pa_audio_manager_bridge_target target);

#endif /* AUDIO_MANAGER_BRIDGE_H */
