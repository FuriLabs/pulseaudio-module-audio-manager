/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#ifndef AUDIO_MANAGER_UTIL_H
#define AUDIO_MANAGER_UTIL_H

#include <stdbool.h>

#include <audio-manager/audio-manager.h>
#include <pulse/sample.h>

bool
pa_audio_manager_sample_format_to_am(pa_sample_format_t format,
                                     AudioManagerSampleFormat *am_format);

bool
pa_audio_manager_sample_format_from_am(AudioManagerSampleFormat format,
                                       pa_sample_format_t *pa_format);

bool
pa_audio_manager_capture_role_parse(const char *value,
                                    AudioManagerCaptureRole *role);

const char *
pa_audio_manager_capture_role_to_string(AudioManagerCaptureRole role);

const char *
pa_audio_manager_playback_role_to_string(AudioManagerPlaybackRole role);

const char *
pa_audio_manager_output_device_to_string(AudioManagerOutputDevice device);

const char *
pa_audio_manager_input_device_to_string(AudioManagerInputDevice device);

#endif /* AUDIO_MANAGER_UTIL_H */
