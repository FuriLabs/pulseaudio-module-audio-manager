/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include "audio-manager-util.h"

#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

bool
pa_audio_manager_sample_format_to_am(pa_sample_format_t format,
                                     AudioManagerSampleFormat *am_format)
{
    pa_assert(am_format);

    switch (format) {
    case PA_SAMPLE_S16LE:
        *am_format = AUDIO_MANAGER_SAMPLE_S16_LE;
        return true;
    case PA_SAMPLE_S24_32LE:
        *am_format = AUDIO_MANAGER_SAMPLE_S24_LE;
        return true;
    case PA_SAMPLE_S32LE:
        *am_format = AUDIO_MANAGER_SAMPLE_S32_LE;
        return true;
    default:
        return false;
    }
}

bool
pa_audio_manager_sample_format_from_am(AudioManagerSampleFormat format,
                                       pa_sample_format_t *pa_format)
{
    pa_assert(pa_format);

    switch (format) {
    case AUDIO_MANAGER_SAMPLE_S16_LE:
        *pa_format = PA_SAMPLE_S16LE;
        return true;
    case AUDIO_MANAGER_SAMPLE_S24_LE:
        *pa_format = PA_SAMPLE_S24_32LE;
        return true;
    case AUDIO_MANAGER_SAMPLE_S32_LE:
        *pa_format = PA_SAMPLE_S32LE;
        return true;
    default:
        return false;
    }
}

bool
pa_audio_manager_capture_role_parse(const char *value,
                                    AudioManagerCaptureRole *role)
{
    pa_assert(role);

    if (value == NULL || pa_streq(value, "default"))
        *role = AUDIO_MANAGER_CAPTURE_ROLE_DEFAULT;
    else if (pa_streq(value, "camcorder"))
        *role = AUDIO_MANAGER_CAPTURE_ROLE_CAMCORDER;
    else if (pa_streq(value, "voice-recognition"))
        *role = AUDIO_MANAGER_CAPTURE_ROLE_VOICE_RECOGNITION;
    else if (pa_streq(value, "unprocessed"))
        *role = AUDIO_MANAGER_CAPTURE_ROLE_UNPROCESSED;
    else
        return false;

    return true;
}

const char *
pa_audio_manager_capture_role_to_string(AudioManagerCaptureRole role)
{
    switch (role) {
    case AUDIO_MANAGER_CAPTURE_ROLE_DEFAULT:
        return "default";
    case AUDIO_MANAGER_CAPTURE_ROLE_CAMCORDER:
        return "camcorder";
    case AUDIO_MANAGER_CAPTURE_ROLE_VOICE_RECOGNITION:
        return "voice-recognition";
    case AUDIO_MANAGER_CAPTURE_ROLE_UNPROCESSED:
        return "unprocessed";
    default:
        return "default";
    }
}

const char *
pa_audio_manager_playback_role_to_string(AudioManagerPlaybackRole role)
{
    switch (role) {
    case AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT:
        return "default";
    case AUDIO_MANAGER_PLAYBACK_ROLE_LOW_LATENCY:
        return "low-latency";
    case AUDIO_MANAGER_PLAYBACK_ROLE_POWER_SAVING:
        return "power-saving";
    default:
        return "default";
    }
}

const char *
pa_audio_manager_output_device_to_string(AudioManagerOutputDevice device)
{
    switch (device) {
    case AUDIO_MANAGER_OUTPUT_RECEIVER:
        return "receiver";
    case AUDIO_MANAGER_OUTPUT_SPEAKER:
        return "speaker";
    case AUDIO_MANAGER_OUTPUT_HEADPHONES:
        return "headphones";
    case AUDIO_MANAGER_OUTPUT_HEADSET:
        return "headset";
    case AUDIO_MANAGER_OUTPUT_BLUETOOTH:
        return "bluetooth";
    case AUDIO_MANAGER_OUTPUT_USB:
        return "usb";
    case AUDIO_MANAGER_OUTPUT_NONE:
    default:
        return "none";
    }
}

const char *
pa_audio_manager_input_device_to_string(AudioManagerInputDevice device)
{
    switch (device) {
    case AUDIO_MANAGER_INPUT_BUILTIN_MIC:
        return "builtin-mic";
    case AUDIO_MANAGER_INPUT_HEADSET_MIC:
        return "headset-mic";
    case AUDIO_MANAGER_INPUT_BLUETOOTH:
        return "bluetooth";
    case AUDIO_MANAGER_INPUT_USB:
        return "usb";
    case AUDIO_MANAGER_INPUT_NONE:
    default:
        return "none";
    }
}
