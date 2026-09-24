/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#define PA_MODULE_NAME module_audio_manager_card

#include "audio-manager-bridge.h"
#include "audio-manager-call.h"
#include "audio-manager-jack.h"
#include "audio-manager-shared.h"
#include "audio-manager-sink.h"
#include "audio-manager-source.h"
#include "audio-manager-util.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <pulse/def.h>
#include <pulse/proplist.h>
#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/volume.h>
#include <pulse/xmalloc.h>
#include <pulsecore/core.h>
#include <pulsecore/card.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-util.h>
#include <pulsecore/device-port.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/idxset.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/message-handler.h>
#include <pulsecore/module.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/source-output.h>

PA_MODULE_AUTHOR("Bardia Moshiri");
PA_MODULE_DESCRIPTION("Sound card");
PA_MODULE_VERSION(PULSE_MODULE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE("");

enum profile_mode {
    PROFILE_MODE_DEFAULT,
    PROFILE_MODE_CALL,
    PROFILE_MODE_CALL_BLUETOOTH,
    PROFILE_MODE_CALL_USB,
};

struct profile_data {
    enum profile_mode mode;
};

enum port_kind {
    PORT_KIND_OUTPUT,
    PORT_KIND_INPUT,
};

struct port_data {
    enum port_kind kind;
    union {
        AudioManagerOutputDevice output;
        AudioManagerInputDevice input;
    } device;
};

struct userdata {
    pa_module *module;
    pa_audio_manager_shared *shared;
    pa_card *card;
    pa_sink *sink;
    pa_source *source;
    pa_audio_manager_jack *jack;
    pa_audio_manager_call_pcm *call_pcm;
    pa_audio_manager_bridge *bridge;
    pa_audio_manager_bridge_target bridge_target;
    AudioManagerOutputDevice output_device;
    AudioManagerInputDevice input_device;
    AudioManagerCaptureRole default_capture_role;
    bool capture_policy;
    bool route_sync_in_progress;
    unsigned external_transition_depth;
    char *bluetooth_card_name;
    char *bluetooth_restore_profile;
    char *saved_policy_default_sink;
    char *saved_policy_default_source;
    bool call_policy_defaults_saved;
    pa_card_profile *profile_default;
    pa_card_profile *profile_call;
    pa_card_profile *profile_bluetooth;
    pa_card_profile *profile_usb;
    pa_time_event *recovery_timer;
    AudioManagerCallState last_call_state;
    bool message_handler_registered;
};

static void update_external_profile_availability(struct userdata *u);
static int sync_selected_call_transport(struct userdata *u,
                                        pa_audio_manager_bridge_target target);

#define AUDIO_MANAGER_CARD_NAME "audio-manager-card"
#define AUDIO_MANAGER_CALL_UPLINK_NAME "audio-manager-call-uplink"
#define AUDIO_MANAGER_CALL_DOWNLINK_NAME "audio-manager-call-downlink"
#define AUDIO_MANAGER_BRIDGE_LATENCY_MSEC 20U
#define AUDIO_MANAGER_MESSAGE_PATH "/audio-manager-card"

#define CALL_RECOVERY_INTERVAL_USEC (PA_USEC_PER_SEC)

static const char *
output_port_name(AudioManagerOutputDevice device)
{
    switch (device) {
    case AUDIO_MANAGER_OUTPUT_RECEIVER:
        return "output-earpiece";
    case AUDIO_MANAGER_OUTPUT_SPEAKER:
        return "output-speaker";
    case AUDIO_MANAGER_OUTPUT_HEADPHONES:
        return "output-headphones";
    case AUDIO_MANAGER_OUTPUT_HEADSET:
        return "output-headset";
    case AUDIO_MANAGER_OUTPUT_NONE:
    case AUDIO_MANAGER_OUTPUT_BLUETOOTH:
    case AUDIO_MANAGER_OUTPUT_USB:
    default:
        return NULL;
    }
}

static const char *
input_port_name(AudioManagerInputDevice device)
{
    switch (device) {
    case AUDIO_MANAGER_INPUT_BUILTIN_MIC:
        return "input-internal-mic";
    case AUDIO_MANAGER_INPUT_HEADSET_MIC:
        return "input-headset-mic";
    case AUDIO_MANAGER_INPUT_NONE:
    case AUDIO_MANAGER_INPUT_BLUETOOTH:
    case AUDIO_MANAGER_INPUT_USB:
    default:
        return NULL;
    }
}

static bool
is_output_supported(const AudioManagerCapabilities *capabilities,
                    AudioManagerOutputDevice device)
{
    return (capabilities->output_devices &
            AUDIO_MANAGER_OUTPUT_DEVICE_MASK(device)) != 0;
}

static bool
is_input_supported(const AudioManagerCapabilities *capabilities,
                   AudioManagerInputDevice device)
{
    return (capabilities->input_devices &
            AUDIO_MANAGER_INPUT_DEVICE_MASK(device)) != 0;
}

static bool
is_cellular_output_supported(const AudioManagerCapabilities *capabilities,
                             AudioManagerOutputDevice device)
{
    return (capabilities->cellular_call_output_devices &
            AUDIO_MANAGER_OUTPUT_DEVICE_MASK(device)) != 0;
}

static bool
is_cellular_input_supported(const AudioManagerCapabilities *capabilities,
                            AudioManagerInputDevice device)
{
    return (capabilities->cellular_call_input_devices &
            AUDIO_MANAGER_INPUT_DEVICE_MASK(device)) != 0;
}

static AudioManagerCallTransportMask
get_cellular_route_transports(struct userdata *u,
                              AudioManagerOutputDevice output,
                              AudioManagerInputDevice input)
{
    pa_assert(u);
    pa_assert(u->shared);

    return audio_manager_get_supported_cellular_call_transports(pa_audio_manager_shared_manager(u->shared),
                                                                output,
                                                                input);
}

static bool
is_cellular_route_supported(struct userdata *u,
                            AudioManagerOutputDevice output,
                            AudioManagerInputDevice input)
{
    return get_cellular_route_transports(u, output, input) != 0;
}

static int
select_call_transport(struct userdata *u,
                      AudioManagerOutputDevice output,
                      AudioManagerInputDevice input,
                      AudioManagerCallTransport *transport)
{
    AudioManagerCallTransportMask transports;

    pa_assert(u);
    pa_assert(transport);

    transports = get_cellular_route_transports(u, output, input);
    if (transports & AUDIO_MANAGER_CALL_TRANSPORT_MASK(AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS)) {
        *transport = AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS;
        return 0;
    }
    if (transports & AUDIO_MANAGER_CALL_TRANSPORT_MASK(AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL)) {
        *transport = AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL;
        return 0;
    }

    return -ENOTSUP;
}

static AudioManagerInputDevice
call_input_for_output(const AudioManagerCapabilities *capabilities,
                      AudioManagerOutputDevice output)
{
    switch (output) {
    case AUDIO_MANAGER_OUTPUT_HEADSET:
        return is_cellular_input_supported(capabilities, AUDIO_MANAGER_INPUT_HEADSET_MIC) ?
               AUDIO_MANAGER_INPUT_HEADSET_MIC : AUDIO_MANAGER_INPUT_NONE;
    case AUDIO_MANAGER_OUTPUT_RECEIVER:
    case AUDIO_MANAGER_OUTPUT_SPEAKER:
    case AUDIO_MANAGER_OUTPUT_HEADPHONES:
        return is_cellular_input_supported(capabilities, AUDIO_MANAGER_INPUT_BUILTIN_MIC) ?
               AUDIO_MANAGER_INPUT_BUILTIN_MIC : AUDIO_MANAGER_INPUT_NONE;
    default:
        return AUDIO_MANAGER_INPUT_NONE;
    }
}

static bool
call_local_output_supported(struct userdata *u,
                            const AudioManagerCapabilities *capabilities,
                            AudioManagerOutputDevice output)
{
    AudioManagerInputDevice input;

    if (!is_output_supported(capabilities, output) ||
        !is_cellular_output_supported(capabilities, output))
        return false;

    input = call_input_for_output(capabilities, output);
    return input != AUDIO_MANAGER_INPUT_NONE &&
           is_input_supported(capabilities, input) &&
           is_cellular_route_supported(u, output, input);
}

static AudioManagerOutputDevice
choose_call_local_output(struct userdata *u,
                         const AudioManagerCapabilities *capabilities,
                         AudioManagerOutputDevice preferred)
{
    static const AudioManagerOutputDevice outputs[] = {
        AUDIO_MANAGER_OUTPUT_RECEIVER,
        AUDIO_MANAGER_OUTPUT_SPEAKER,
        AUDIO_MANAGER_OUTPUT_HEADPHONES,
        AUDIO_MANAGER_OUTPUT_HEADSET,
    };
    unsigned i;

    if (call_local_output_supported(u, capabilities, preferred))
        return preferred;

    for (i = 0; i < PA_ELEMENTSOF(outputs); i++) {
        if (call_local_output_supported(u, capabilities, outputs[i]))
            return outputs[i];
    }

    return AUDIO_MANAGER_OUTPUT_NONE;
}

static AudioManagerOutputDevice
choose_initial_output(const AudioManagerCapabilities *capabilities)
{
    if (is_output_supported(capabilities, AUDIO_MANAGER_OUTPUT_SPEAKER))
        return AUDIO_MANAGER_OUTPUT_SPEAKER;
    if (is_output_supported(capabilities, AUDIO_MANAGER_OUTPUT_RECEIVER))
        return AUDIO_MANAGER_OUTPUT_RECEIVER;
    if (is_output_supported(capabilities, AUDIO_MANAGER_OUTPUT_HEADPHONES))
        return AUDIO_MANAGER_OUTPUT_HEADPHONES;
    if (is_output_supported(capabilities, AUDIO_MANAGER_OUTPUT_HEADSET))
        return AUDIO_MANAGER_OUTPUT_HEADSET;
    return AUDIO_MANAGER_OUTPUT_NONE;
}

static AudioManagerInputDevice
choose_initial_input(const AudioManagerCapabilities *capabilities)
{
    if (is_input_supported(capabilities, AUDIO_MANAGER_INPUT_BUILTIN_MIC))
        return AUDIO_MANAGER_INPUT_BUILTIN_MIC;
    if (is_input_supported(capabilities, AUDIO_MANAGER_INPUT_HEADSET_MIC))
        return AUDIO_MANAGER_INPUT_HEADSET_MIC;
    return AUDIO_MANAGER_INPUT_NONE;
}

static pa_device_port *
new_output_port(pa_core *core,
                const char *name,
                const char *description,
                pa_device_port_type_t type,
                unsigned priority,
                pa_available_t available,
                AudioManagerOutputDevice device)
{
    pa_device_port_new_data data;
    pa_device_port *port;
    struct port_data *port_data;

    pa_device_port_new_data_init(&data);
    pa_device_port_new_data_set_name(&data, name);
    pa_device_port_new_data_set_description(&data, description);
    pa_device_port_new_data_set_direction(&data, PA_DIRECTION_OUTPUT);
    pa_device_port_new_data_set_type(&data, type);
    pa_device_port_new_data_set_available(&data, available);
    if (device == AUDIO_MANAGER_OUTPUT_HEADPHONES ||
        device == AUDIO_MANAGER_OUTPUT_HEADSET)
        pa_device_port_new_data_set_availability_group(&data, "audio-manager-wired");

    port = pa_device_port_new(core, &data, sizeof(struct port_data));
    pa_device_port_new_data_done(&data);
    if (port == NULL)
        return NULL;

    port->priority = priority;
    port_data = PA_DEVICE_PORT_DATA(port);
    port_data->kind = PORT_KIND_OUTPUT;
    port_data->device.output = device;
    return port;
}

static pa_device_port *
new_input_port(pa_core *core,
               const char *name,
               const char *description,
               pa_device_port_type_t type,
               unsigned priority,
               pa_available_t available,
               AudioManagerInputDevice device)
{
    pa_device_port_new_data data;
    pa_device_port *port;
    struct port_data *port_data;

    pa_device_port_new_data_init(&data);
    pa_device_port_new_data_set_name(&data, name);
    pa_device_port_new_data_set_description(&data, description);
    pa_device_port_new_data_set_direction(&data, PA_DIRECTION_INPUT);
    pa_device_port_new_data_set_type(&data, type);
    pa_device_port_new_data_set_available(&data, available);
    if (device == AUDIO_MANAGER_INPUT_HEADSET_MIC)
        pa_device_port_new_data_set_availability_group(&data, "audio-manager-wired");

    port = pa_device_port_new(core, &data, sizeof(struct port_data));
    pa_device_port_new_data_done(&data);
    if (port == NULL)
        return NULL;

    port->priority = priority;
    port_data = PA_DEVICE_PORT_DATA(port);
    port_data->kind = PORT_KIND_INPUT;
    port_data->device.input = device;
    return port;
}

static int
add_port(pa_card_new_data *card_data,
         pa_card_profile **profiles,
         size_t n_profiles,
         pa_device_port *port)
{
    size_t i;

    pa_assert(card_data);
    pa_assert(profiles);

    if (port == NULL)
        return -1;

    for (i = 0; i < n_profiles; i++) {
        pa_assert(profiles[i]);
        if (pa_hashmap_put(port->profiles,
                           profiles[i]->name,
                           profiles[i]) < 0) {
            pa_device_port_unref(port);
            return -1;
        }
    }

    if (pa_hashmap_put(card_data->ports, port->name, port) < 0) {
        pa_device_port_unref(port);
        return -1;
    }

    return 0;
}

static int
add_capability_ports(pa_module *module,
                     pa_card_new_data *card_data,
                     pa_card_profile **profiles,
                     size_t n_profiles,
                     const AudioManagerCapabilities *capabilities)
{
    pa_device_port *port;

    if (is_output_supported(capabilities, AUDIO_MANAGER_OUTPUT_RECEIVER)) {
        port = new_output_port(module->core,
                               "output-earpiece",
                               "Built-in Earpiece",
                               PA_DEVICE_PORT_TYPE_EARPIECE,
                               200,
                               PA_AVAILABLE_YES,
                               AUDIO_MANAGER_OUTPUT_RECEIVER);
        if (add_port(card_data, profiles, n_profiles, port) < 0)
            return -1;
    }

    if (is_output_supported(capabilities, AUDIO_MANAGER_OUTPUT_SPEAKER)) {
        port = new_output_port(module->core,
                               "output-speaker",
                               "Built-in Speaker",
                               PA_DEVICE_PORT_TYPE_SPEAKER,
                               300,
                               PA_AVAILABLE_YES,
                               AUDIO_MANAGER_OUTPUT_SPEAKER);
        if (add_port(card_data, profiles, n_profiles, port) < 0)
            return -1;
    }

    if (is_output_supported(capabilities, AUDIO_MANAGER_OUTPUT_HEADPHONES)) {
        port = new_output_port(module->core,
                               "output-headphones",
                               "Wired Headphones",
                               PA_DEVICE_PORT_TYPE_HEADPHONES,
                               400,
                               PA_AVAILABLE_UNKNOWN,
                               AUDIO_MANAGER_OUTPUT_HEADPHONES);
        if (add_port(card_data, profiles, n_profiles, port) < 0)
            return -1;
    }

    if (is_output_supported(capabilities, AUDIO_MANAGER_OUTPUT_HEADSET)) {
        port = new_output_port(module->core,
                               "output-headset",
                               "Wired Headset",
                               PA_DEVICE_PORT_TYPE_HEADSET,
                               400,
                               PA_AVAILABLE_UNKNOWN,
                               AUDIO_MANAGER_OUTPUT_HEADSET);
        if (add_port(card_data, profiles, n_profiles, port) < 0)
            return -1;
    }

    if (is_input_supported(capabilities, AUDIO_MANAGER_INPUT_BUILTIN_MIC)) {
        port = new_input_port(module->core,
                              "input-internal-mic",
                              "Built-in Microphone",
                              PA_DEVICE_PORT_TYPE_MIC,
                              200,
                              PA_AVAILABLE_YES,
                              AUDIO_MANAGER_INPUT_BUILTIN_MIC);
        if (add_port(card_data, profiles, n_profiles, port) < 0)
            return -1;
    }

    if (is_input_supported(capabilities, AUDIO_MANAGER_INPUT_HEADSET_MIC)) {
        port = new_input_port(module->core,
                              "input-headset-mic",
                              "Wired Headset Microphone",
                              PA_DEVICE_PORT_TYPE_HEADSET,
                              400,
                              PA_AVAILABLE_UNKNOWN,
                              AUDIO_MANAGER_INPUT_HEADSET_MIC);
        if (add_port(card_data, profiles, n_profiles, port) < 0)
            return -1;
    }

    return 0;
}

static bool
is_playback_role_supported(const AudioManagerCapabilities *capabilities,
                           AudioManagerPlaybackRole role)
{
    return (capabilities->playback_roles & AUDIO_MANAGER_PLAYBACK_ROLE_MASK(role)) != 0;
}

static AudioManagerPlaybackRole
choose_initial_playback_role(const AudioManagerCapabilities *capabilities)
{
    pa_assert(capabilities);

    if (is_playback_role_supported(capabilities, AUDIO_MANAGER_PLAYBACK_ROLE_LOW_LATENCY))
        return AUDIO_MANAGER_PLAYBACK_ROLE_LOW_LATENCY;
    if (is_playback_role_supported(capabilities, AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT))
        return AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT;

    return AUDIO_MANAGER_PLAYBACK_ROLE_DEFAULT;
}

static char *
supported_playback_roles_string(const AudioManagerCapabilities *capabilities)
{
    static const struct {
        AudioManagerPlaybackRole role;
        const char *name;
    } known_roles[] = {
        { AUDIO_MANAGER_PLAYBACK_ROLE_LOW_LATENCY, "low-latency" },
        { AUDIO_MANAGER_PLAYBACK_ROLE_POWER_SAVING, "power-saving" },
    };
    char *roles = pa_xstrdup("");
    size_t i;

    pa_assert(capabilities);

    for (i = 0; i < PA_ELEMENTSOF(known_roles); i++) {
        char *next;

        if (!is_playback_role_supported(capabilities, known_roles[i].role))
            continue;

        next = roles[0] != '\0' ? pa_sprintf_malloc("%s,%s", roles, known_roles[i].name) :
                                  pa_xstrdup(known_roles[i].name);
        pa_xfree(roles);
        roles = next;
    }

    return roles;
}

static void
set_port_available(struct userdata *u,
                   const char *name,
                   pa_available_t available)
{
    pa_device_port *port;

    pa_assert(u);
    pa_assert(u->card);
    pa_assert(name);

    port = pa_hashmap_get(u->card->ports, name);
    if (port != NULL)
        pa_device_port_set_available(port, available);
}

static bool
call_is_active(struct userdata *u)
{
    return u->shared != NULL && audio_manager_call_is_active(pa_audio_manager_shared_manager(u->shared));
}

static enum profile_mode
active_profile_mode(struct userdata *u)
{
    struct profile_data *data;

    pa_assert(u);

    if (u->card == NULL || u->card->active_profile == NULL)
        return PROFILE_MODE_DEFAULT;

    data = PA_CARD_PROFILE_DATA(u->card->active_profile);
    return data->mode;
}

static void
jack_changed_cb(void *userdata,
                bool known,
                bool headphone,
                bool microphone)
{
    struct userdata *u = userdata;
    pa_available_t headphones;
    pa_available_t headset;
    pa_available_t headset_mic;

    pa_assert(u);

    if (!known) {
        headphones = PA_AVAILABLE_UNKNOWN;
        headset = PA_AVAILABLE_UNKNOWN;
        headset_mic = PA_AVAILABLE_UNKNOWN;
    } else if (!headphone) {
        headphones = PA_AVAILABLE_NO;
        headset = PA_AVAILABLE_NO;
        headset_mic = PA_AVAILABLE_NO;
    } else if (microphone) {
        headphones = PA_AVAILABLE_NO;
        headset = PA_AVAILABLE_YES;
        headset_mic = PA_AVAILABLE_YES;
    } else {
        headphones = PA_AVAILABLE_YES;
        headset = PA_AVAILABLE_NO;
        headset_mic = PA_AVAILABLE_NO;
    }

    set_port_available(u, "output-headphones", headphones);
    set_port_available(u, "output-headset", headset);
    set_port_available(u, "input-headset-mic", headset_mic);

    if (known && !headphone && call_is_active(u) && u->sink != NULL) {
        AudioManagerOutputDevice current = audio_manager_get_output_device(pa_audio_manager_shared_manager(u->shared));

        if ((current == AUDIO_MANAGER_OUTPUT_HEADPHONES ||
             current == AUDIO_MANAGER_OUTPUT_HEADSET) &&
            pa_hashmap_get(u->sink->ports, "output-earpiece") != NULL &&
            pa_sink_set_port(u->sink, "output-earpiece", false) < 0)
            pa_log_warn("failed to restore active call to earpiece after wired unplug");
    }

    if (known)
        pa_log_info("wired jack state changed: connected=%s microphone=%s",
                    headphone ? "yes" : "no",
                    microphone ? "yes" : "no");
    else
        pa_log_info("wired jack state is unknown");
}

static void
update_output_device_properties(struct userdata *u, const char *device_name)
{
    pa_assert(u);
    pa_assert(device_name);

    if (u->sink != NULL)
        pa_proplist_sets(u->sink->proplist, "audio_manager.output_device", device_name);
}

static void
update_input_device_properties(struct userdata *u, const char *device_name)
{
    pa_assert(u);
    pa_assert(device_name);

    if (u->source != NULL)
        pa_proplist_sets(u->source->proplist, "audio_manager.input_device", device_name);
}

static AudioManagerCallTransport
call_transport(struct userdata *u)
{
    pa_assert(u);
    pa_assert(u->shared);

    return audio_manager_call_get_transport(pa_audio_manager_shared_manager(u->shared));
}

static AudioManagerInputDevice
paired_input_for_output(struct userdata *u,
                        AudioManagerOutputDevice output)
{
    const AudioManagerCapabilities *capabilities;

    pa_assert(u);
    pa_assert(u->shared);

    capabilities = pa_audio_manager_shared_capabilities(u->shared);

    if (output == AUDIO_MANAGER_OUTPUT_HEADSET &&
        is_cellular_input_supported(capabilities, AUDIO_MANAGER_INPUT_HEADSET_MIC))
        return AUDIO_MANAGER_INPUT_HEADSET_MIC;

    if ((output == AUDIO_MANAGER_OUTPUT_RECEIVER ||
         output == AUDIO_MANAGER_OUTPUT_SPEAKER ||
         output == AUDIO_MANAGER_OUTPUT_HEADPHONES) &&
        is_cellular_input_supported(capabilities, AUDIO_MANAGER_INPUT_BUILTIN_MIC))
        return AUDIO_MANAGER_INPUT_BUILTIN_MIC;

    return u->input_device;
}

static void
sync_sink_port_to_device(struct userdata *u,
                         AudioManagerOutputDevice output)
{
    const char *name;
    pa_device_port *port;

    pa_assert(u);

    if (u->sink == NULL)
        return;

    name = output_port_name(output);
    if (name == NULL)
        return;

    port = pa_hashmap_get(u->sink->ports, name);
    if (port == NULL || u->sink->active_port == port)
        return;

    u->route_sync_in_progress = true;
    if (pa_sink_set_port(u->sink, name, false) < 0)
        pa_log_warn("failed to synchronize PulseAudio sink port to '%s'", name);
    u->route_sync_in_progress = false;
}

static void
sync_source_port_to_device(struct userdata *u,
                           AudioManagerInputDevice input)
{
    const char *name;
    pa_device_port *port;

    pa_assert(u);

    if (u->source == NULL)
        return;

    name = input_port_name(input);
    if (name == NULL)
        return;

    port = pa_hashmap_get(u->source->ports, name);
    if (port == NULL || u->source->active_port == port)
        return;

    u->route_sync_in_progress = true;
    if (pa_source_set_port(u->source, name, false) < 0)
        pa_log_warn("failed to synchronize PulseAudio source port to '%s'", name);
    u->route_sync_in_progress = false;
}

static int
ensure_paired_call_devices(struct userdata *u)
{
    AudioManager *manager;
    AudioManagerInputDevice input;
    const char *input_name;
    int ret;

    pa_assert(u);
    pa_assert(u->shared);

    manager = pa_audio_manager_shared_manager(u->shared);
    input = paired_input_for_output(u, u->output_device);
    if (audio_manager_get_output_device(manager) == u->output_device &&
        audio_manager_get_input_device(manager) == input)
        return 0;

    ret = audio_manager_set_devices(manager, u->output_device, input);
    if (ret < 0) {
        pa_log_error("failed to pair call devices: %d", ret);
        return ret;
    }

    u->input_device = input;
    input_name = pa_audio_manager_input_device_to_string(input);
    update_input_device_properties(u, input_name);
    sync_sink_port_to_device(u, u->output_device);
    sync_source_port_to_device(u, input);
    return 0;
}

static void
stop_bridge(struct userdata *u)
{
    pa_assert(u);

    if (u->bridge != NULL) {
        pa_audio_manager_bridge_free(u->bridge);
        u->bridge = NULL;
    }
    u->bridge_target = PA_AUDIO_MANAGER_BRIDGE_NONE;
}

static void
destroy_call_pcm(struct userdata *u)
{
    pa_assert(u);

    stop_bridge(u);

    if (u->call_pcm == NULL)
        return;

    pa_audio_manager_call_pcm_free(u->call_pcm);
    u->call_pcm = NULL;
}

static int
sink_set_port_cb(pa_sink *sink, pa_device_port *port)
{
    struct userdata *u;
    struct port_data *port_data;
    AudioManagerInputDevice input;
    AudioManagerCallTransport transport;
    const char *output_name;
    const char *input_name;
    int ret;

    pa_assert(sink);
    pa_assert(port);
    pa_assert(sink->card);
    pa_assert_se(u = sink->card->userdata);

    port_data = PA_DEVICE_PORT_DATA(port);
    if (port_data->kind != PORT_KIND_OUTPUT)
        return -1;

    if (u->route_sync_in_progress) {
        u->output_device = port_data->device.output;
        update_output_device_properties(u, pa_audio_manager_output_device_to_string(u->output_device));
        return 0;
    }

    input = u->input_device;
    if (call_is_active(u) && active_profile_mode(u) == PROFILE_MODE_CALL) {
        input = paired_input_for_output(u, port_data->device.output);
        ret = select_call_transport(u, port_data->device.output, input, &transport);
        if (ret == 0) {
            destroy_call_pcm(u);
            ret = audio_manager_call_set_route(pa_audio_manager_shared_manager(u->shared),
                                               port_data->device.output,
                                               input,
                                               transport);
        }
        if (ret < 0)
            sync_selected_call_transport(u, PA_AUDIO_MANAGER_BRIDGE_LOCAL);
    } else if (call_is_active(u)) {
        ret = 0;
    } else {
        ret = audio_manager_set_output_device(pa_audio_manager_shared_manager(u->shared),
                                              port_data->device.output);
    }

    if (ret < 0) {
        pa_log_error("failed to route audio-manager output to port '%s': %d",
                     port->name, ret);
        return -1;
    }

    /* remember the local output route for when the call returns to local */
    u->output_device = port_data->device.output;
    output_name = pa_audio_manager_output_device_to_string(u->output_device);
    update_output_device_properties(u, output_name);

    if (call_is_active(u) && active_profile_mode(u) == PROFILE_MODE_CALL &&
        input != u->input_device) {
        u->input_device = input;
        input_name = pa_audio_manager_input_device_to_string(u->input_device);
        update_input_device_properties(u, input_name);
        sync_source_port_to_device(u, input);
    }

    if (call_is_active(u) && active_profile_mode(u) == PROFILE_MODE_CALL) {
        ret = sync_selected_call_transport(u, PA_AUDIO_MANAGER_BRIDGE_LOCAL);
        if (ret < 0) {
            pa_log_error("failed to synchronize call transport after output route change: %d", ret);
            return -1;
        }
    }

    pa_log_info("audio-manager output route changed to %s (%s)%s",
                port->name,
                output_name);
    return 0;
}

static int
source_set_port_cb(pa_source *source, pa_device_port *port)
{
    struct userdata *u;
    struct port_data *port_data;
    AudioManagerCallTransport transport;
    const char *device_name;
    int ret;

    pa_assert(source);
    pa_assert(port);
    pa_assert(source->card);
    pa_assert_se(u = source->card->userdata);

    port_data = PA_DEVICE_PORT_DATA(port);
    if (port_data->kind != PORT_KIND_INPUT)
        return -1;

    if (u->route_sync_in_progress) {
        u->input_device = port_data->device.input;
        update_input_device_properties(u,
            pa_audio_manager_input_device_to_string(u->input_device));
        return 0;
    }

    if (call_is_active(u) && active_profile_mode(u) == PROFILE_MODE_CALL) {
        ret = select_call_transport(u,
                                        u->output_device,
                                        port_data->device.input,
                                        &transport);
        if (ret == 0) {
            destroy_call_pcm(u);
            ret = audio_manager_call_set_route(pa_audio_manager_shared_manager(u->shared),
                                               u->output_device,
                                               port_data->device.input,
                                               transport);
        }
        if (ret < 0)
            sync_selected_call_transport(u, PA_AUDIO_MANAGER_BRIDGE_LOCAL);
    } else if (call_is_active(u))
        ret = 0;
    else
        ret = audio_manager_set_input_device(pa_audio_manager_shared_manager(u->shared),
                                             port_data->device.input);

    if (ret < 0) {
        pa_log_error("failed to route audio-manager input to port '%s': %d",
                     port->name, ret);
        return -1;
    }

    u->input_device = port_data->device.input;
    device_name = pa_audio_manager_input_device_to_string(u->input_device);
    update_input_device_properties(u, device_name);

    if (call_is_active(u) && active_profile_mode(u) == PROFILE_MODE_CALL) {
        ret = sync_selected_call_transport(u, PA_AUDIO_MANAGER_BRIDGE_LOCAL);
        if (ret < 0) {
            pa_log_error("failed to synchronize call transport after input route change: %d", ret);
            return -1;
        }
    }

    pa_log_info("audio-manager input route changed to %s (%s)",
                port->name, device_name);
    return 0;
}

static pa_volume_t
current_sink_volume(pa_sink *sink)
{
    pa_volume_t volume;

    pa_assert(sink);

    volume = pa_cvolume_avg(&sink->reference_volume);
    if (volume > PA_VOLUME_NORM)
        volume = PA_VOLUME_NORM;
    return volume;
}

static void
save_call_policy_defaults(struct userdata *u)
{
    pa_core *core;

    pa_assert(u);

    if (u->call_policy_defaults_saved)
        return;

    /* save the policy defaults so they can be restored after the call */
    core = u->module->core;
    u->saved_policy_default_sink = pa_xstrdup(core->policy_default_sink);
    u->saved_policy_default_source = pa_xstrdup(core->policy_default_source);
    u->call_policy_defaults_saved = true;
}

static bool
find_bridge_device(struct userdata *u,
                   pa_audio_manager_bridge_target target,
                   pa_audio_manager_bridge_device *device)
{
    pa_assert(u);
    pa_assert(device);

    if (target == PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH && u->bluetooth_card_name != NULL)
        return pa_audio_manager_bridge_find_device_on_card(u->module->core,
                                                           target,
                                                           u->bluetooth_card_name,
                                                           device);

    return pa_audio_manager_bridge_find_device_on_card(u->module->core, target, NULL, device);
}

static void
set_call_policy_defaults(struct userdata *u,
                         pa_audio_manager_bridge_target target)
{
    pa_audio_manager_bridge_device device = { 0 };
    pa_sink *sink = NULL;
    pa_source *source = NULL;

    pa_assert(u);

    if (!u->call_policy_defaults_saved)
        save_call_policy_defaults(u);

    if (target == PA_AUDIO_MANAGER_BRIDGE_LOCAL) {
        sink = u->sink;
        source = u->source;
    } else if (find_bridge_device(u, target, &device)) {
        sink = device.sink;
        source = device.source;
    }

    if (sink != NULL)
        pa_core_set_policy_default_sink(u->module->core, sink->name);
    if (source != NULL)
        pa_core_set_policy_default_source(u->module->core, source->name);
}

static void
restore_call_policy_defaults(struct userdata *u)
{
    pa_assert(u);

    if (!u->call_policy_defaults_saved)
        return;

    pa_core_set_policy_default_sink(u->module->core, u->saved_policy_default_sink);
    pa_core_set_policy_default_source(u->module->core, u->saved_policy_default_source);

    pa_xfree(u->saved_policy_default_sink);
    pa_xfree(u->saved_policy_default_source);
    u->saved_policy_default_sink = NULL;
    u->saved_policy_default_source = NULL;
    u->call_policy_defaults_saved = false;
}

static void
sync_call_controls(struct userdata *u)
{
    const AudioManagerCapabilities *capabilities;
    AudioManager *manager;
    int ret;

    pa_assert(u);

    if (!call_is_active(u))
        return;

    capabilities = pa_audio_manager_shared_capabilities(u->shared);
    manager = pa_audio_manager_shared_manager(u->shared);

    if ((capabilities->flags & AUDIO_MANAGER_CAP_CALL_VOLUME) &&
        call_transport(u) == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS &&
        u->sink != NULL) {
        double volume = (double)current_sink_volume(u->sink) / PA_VOLUME_NORM;

        ret = audio_manager_call_set_volume(manager, volume);
        if (ret < 0)
            pa_log_warn("failed to apply call volume: %d", ret);
    }

    if ((capabilities->flags & AUDIO_MANAGER_CAP_CALL_UPLINK_MUTE) &&
        u->source != NULL) {
        ret = audio_manager_call_set_uplink_mute(manager, u->source->muted);
        if (ret < 0)
            pa_log_warn("failed to apply call uplink mute: %d", ret);
    }

    if ((capabilities->flags & AUDIO_MANAGER_CAP_CALL_DOWNLINK_MUTE) &&
        u->sink != NULL) {
        ret = audio_manager_call_set_downlink_mute(manager, u->sink->muted);
        if (ret < 0)
            pa_log_warn("failed to apply call downlink mute: %d", ret);
    }
}

static void
end_bluetooth_session(struct userdata *u)
{
    pa_assert(u);

    if (u->bluetooth_card_name != NULL) {
        u->external_transition_depth++;
        pa_audio_manager_bridge_restore_bluetooth(u->module->core,
                                                  u->bluetooth_card_name,
                                                  u->bluetooth_restore_profile);
        u->external_transition_depth--;
        update_external_profile_availability(u);
    }

    pa_xfree(u->bluetooth_card_name);
    pa_xfree(u->bluetooth_restore_profile);
    u->bluetooth_card_name = NULL;
    u->bluetooth_restore_profile = NULL;
}

static int
prepare_bluetooth_session(struct userdata *u)
{
    pa_assert(u);

    if (u->bluetooth_card_name != NULL)
        return 0;

    u->external_transition_depth++;
    if (!pa_audio_manager_bridge_prepare_bluetooth(u->module->core,
                                                   &u->bluetooth_card_name,
                                                   &u->bluetooth_restore_profile)) {
        u->external_transition_depth--;
        update_external_profile_availability(u);
        pa_log_error("no usable Bluetooth HFP card is available");
        return -ENODEV;
    }
    u->external_transition_depth--;
    update_external_profile_availability(u);

    return 0;
}

static int
create_call_pcm(struct userdata *u)
{
    pa_assert(u);

    if (u->call_pcm != NULL)
        return 0;

    u->call_pcm = pa_audio_manager_call_pcm_new(u->module,
                                                u->shared,
                                                u->card,
                                                AUDIO_MANAGER_CALL_UPLINK_NAME,
                                                AUDIO_MANAGER_CALL_DOWNLINK_NAME);
    return u->call_pcm != NULL ? 0 : -1;
}

static AudioManagerBluetoothCallCodec
bluetooth_call_codec(const pa_audio_manager_bridge_device *device)
{
    uint32_t rate = 0;

    pa_assert(device);

    if (device->sink != NULL)
        rate = device->sink->sample_spec.rate;
    if (device->source != NULL && device->source->sample_spec.rate > rate)
        rate = device->source->sample_spec.rate;

    /* pulse's HFP endpoints expose CVSD as 8 kHz mono and mSBC as 16 kHz mono */
    return rate >= 16000U ? AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_MSBC :
                            AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_CVSD;
}

static int
start_bridge(struct userdata *u,
             pa_audio_manager_bridge_target target)
{
    pa_sink *uplink;
    pa_source *downlink;

    pa_assert(u);

    if (target == PA_AUDIO_MANAGER_BRIDGE_NONE)
        return 0;
    if (u->call_pcm == NULL)
        return -ENODEV;

    uplink = pa_audio_manager_call_pcm_uplink_sink(u->call_pcm);
    downlink = pa_audio_manager_call_pcm_downlink_source(u->call_pcm);
    if (uplink == NULL || downlink == NULL)
        return -ENODEV;

    stop_bridge(u);
    if (target == PA_AUDIO_MANAGER_BRIDGE_LOCAL && u->card != NULL && u->card->name != NULL)
        u->bridge = pa_audio_manager_bridge_new_on_card(u->module->core,
                                                        target,
                                                        u->card->name,
                                                        downlink,
                                                        uplink,
                                                        AUDIO_MANAGER_BRIDGE_LATENCY_MSEC);
    else if (target == PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH && u->bluetooth_card_name != NULL)
        u->bridge = pa_audio_manager_bridge_new_on_card(u->module->core,
                                                        target,
                                                        u->bluetooth_card_name,
                                                        downlink,
                                                        uplink,
                                                        AUDIO_MANAGER_BRIDGE_LATENCY_MSEC);
    else
        u->bridge = pa_audio_manager_bridge_new(u->module->core,
                                                target,
                                                downlink,
                                                uplink,
                                                AUDIO_MANAGER_BRIDGE_LATENCY_MSEC);
    if (u->bridge == NULL)
        return -ENODEV;

    u->bridge_target = target;
    return 0;
}

static pa_audio_manager_bridge_target
bridge_target_for_output(AudioManagerOutputDevice output)
{
    switch (output) {
    case AUDIO_MANAGER_OUTPUT_BLUETOOTH:
        return PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH;
    case AUDIO_MANAGER_OUTPUT_USB:
        return PA_AUDIO_MANAGER_BRIDGE_USB;
    default:
        return PA_AUDIO_MANAGER_BRIDGE_LOCAL;
    }
}

static int
sync_selected_call_transport(struct userdata *u,
                             pa_audio_manager_bridge_target target)
{
    AudioManager *manager;

    pa_assert(u);
    pa_assert(u->shared);

    manager = pa_audio_manager_shared_manager(u->shared);
    if (audio_manager_call_get_transport(manager) == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS) {
        destroy_call_pcm(u);
        if (u->card != NULL)
            pa_proplist_sets(u->card->proplist, "audio_manager.call.transport", "hostless");
        return 0;
    }

    if (u->card != NULL)
        pa_proplist_sets(u->card->proplist, "audio_manager.call.transport", "hostful");

    if (create_call_pcm(u) < 0) {
        pa_log_error("failed to create host PCM endpoints");
        return -EIO;
    }
    if (start_bridge(u, target) < 0) {
        pa_log_error("failed to bridge host PCM to %s",
                     pa_audio_manager_bridge_target_to_string(target));
        destroy_call_pcm(u);
        return -ENODEV;
    }

    return 0;
}

static int
configure_bluetooth_call(struct userdata *u)
{
    const AudioManagerCapabilities *capabilities;
    AudioManagerBluetoothCallConfig config = AUDIO_MANAGER_BLUETOOTH_CALL_CONFIG_INIT;
    pa_audio_manager_bridge_device device;
    AudioManager *manager;
    int ret;

    pa_assert(u);
    pa_assert(u->shared);

    capabilities = pa_audio_manager_shared_capabilities(u->shared);
    if (!(capabilities->flags & AUDIO_MANAGER_CAP_BLUETOOTH_CALL_CONFIG))
        return 0;

    if (!find_bridge_device(u, PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH, &device))
        return -ENODEV;

    config.codec = bluetooth_call_codec(&device);
    config.nrec = true;
    if (!(capabilities->bluetooth_call_codecs &
          AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_MASK(config.codec)))
        return -ENOTSUP;

    manager = pa_audio_manager_shared_manager(u->shared);
    ret = audio_manager_call_set_bluetooth_config(manager, &config);
    if (ret < 0)
        return ret;

    pa_log_info("configured Bluetooth call speech codec=%s NREC=on",
                config.codec == AUDIO_MANAGER_BLUETOOTH_CALL_CODEC_MSBC ?
                "mSBC" : "CVSD");
    return 0;
}

static int
restore_call_state(struct userdata *u,
                   bool was_active,
                   AudioManagerOutputDevice old_output,
                   AudioManagerInputDevice old_input,
                   AudioManagerCallTransport old_transport,
                   pa_audio_manager_bridge_target old_target)
{
    AudioManager *manager;
    int ret;

    pa_assert(u);
    manager = pa_audio_manager_shared_manager(u->shared);

    destroy_call_pcm(u);

    if (!was_active) {
        if (audio_manager_call_is_active(manager)) {
            ret = audio_manager_call_stop(manager);
            if (ret < 0)
                return ret;
        }
        return audio_manager_set_devices(manager, old_output, old_input);
    }

    ret = audio_manager_call_set_route(manager,
                                       old_output,
                                       old_input,
                                       old_transport);
    if (ret < 0)
        return ret;

    return sync_selected_call_transport(u, old_target);
}

static int
enter_call(struct userdata *u,
           pa_audio_manager_bridge_target target)
{
    const AudioManagerCapabilities *capabilities;
    AudioManager *manager;
    AudioManagerOutputDevice desired_output;
    AudioManagerInputDevice desired_input;
    AudioManagerOutputDevice old_output;
    AudioManagerInputDevice old_input;
    AudioManagerCallTransport transport;
    AudioManagerCallTransport old_transport = AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS;
    pa_audio_manager_bridge_target old_target;
    pa_audio_manager_bridge_device probe_device;
    const char *output_name;
    const char *input_name;
    bool was_active;
    bool bluetooth_started_here = false;
    bool policy_defaults_saved_here = false;
    int ret;
    int restore_ret;

    pa_assert(u);
    pa_assert(u->shared);

    capabilities = pa_audio_manager_shared_capabilities(u->shared);
    if (!(capabilities->flags & AUDIO_MANAGER_CAP_CELLULAR_CALL))
        return -ENOTSUP;

    manager = pa_audio_manager_shared_manager(u->shared);
    was_active = audio_manager_call_is_active(manager);
    old_output = audio_manager_get_output_device(manager);
    old_input = audio_manager_get_input_device(manager);
    if (was_active)
        old_transport = audio_manager_call_get_transport(manager);
    old_target = bridge_target_for_output(old_output);

    switch (target) {
    case PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH:
        if (!is_cellular_output_supported(capabilities, AUDIO_MANAGER_OUTPUT_BLUETOOTH) ||
            !is_cellular_input_supported(capabilities, AUDIO_MANAGER_INPUT_BLUETOOTH))
            return -ENOTSUP;
        desired_output = AUDIO_MANAGER_OUTPUT_BLUETOOTH;
        desired_input = AUDIO_MANAGER_INPUT_BLUETOOTH;
        break;
    case PA_AUDIO_MANAGER_BRIDGE_USB:
        if (!is_cellular_output_supported(capabilities, AUDIO_MANAGER_OUTPUT_USB) ||
            !is_cellular_input_supported(capabilities, AUDIO_MANAGER_INPUT_USB))
            return -ENOTSUP;
        desired_output = AUDIO_MANAGER_OUTPUT_USB;
        desired_input = AUDIO_MANAGER_INPUT_USB;
        break;
    case PA_AUDIO_MANAGER_BRIDGE_LOCAL:
        desired_output = choose_call_local_output(u, capabilities, u->output_device);
        if (desired_output == AUDIO_MANAGER_OUTPUT_NONE)
            return -ENOTSUP;
        desired_input = call_input_for_output(capabilities, desired_output);
        if (desired_input == AUDIO_MANAGER_INPUT_NONE)
            return -ENOTSUP;
        break;
    case PA_AUDIO_MANAGER_BRIDGE_NONE:
    default:
        return -EINVAL;
    }

    ret = select_call_transport(u, desired_output, desired_input, &transport);
    if (ret < 0)
        return ret;

    if (!u->call_policy_defaults_saved) {
        save_call_policy_defaults(u);
        policy_defaults_saved_here = true;
    }

    /*
     * bluetooth needs an HFP session so the headset connection and
     * negotiated speech codec exist no matter who carries the PCM
     */
    if (target == PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH) {
        if (u->bluetooth_card_name == NULL) {
            ret = prepare_bluetooth_session(u);
            if (ret < 0)
                goto fail;
            bluetooth_started_here = true;
        }
        if (!find_bridge_device(u, target, &probe_device)) {
            ret = -ENODEV;
            goto fail;
        }
        if (transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS) {
            ret = configure_bluetooth_call(u);
            if (ret < 0)
                goto fail;
        }
    }

    /*
     * hostful external routes need a pulse endpoint to bridge to
     * hostless routes are owned by the backend so no PCM bridge
     */
    if (transport == AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL &&
        target == PA_AUDIO_MANAGER_BRIDGE_USB &&
        !find_bridge_device(u, target, &probe_device)) {
        ret = -ENODEV;
        goto fail;
    }

    destroy_call_pcm(u);

    if (was_active) {
        ret = audio_manager_call_set_route(manager,
                                           desired_output,
                                           desired_input,
                                           transport);
    } else {
        ret = audio_manager_set_devices(manager,
                                        desired_output,
                                        desired_input);
        if (ret < 0)
            goto fail;

        ret = audio_manager_call_start(manager, transport);
    }

    if (ret < 0)
        goto fail;

    if (target == PA_AUDIO_MANAGER_BRIDGE_LOCAL &&
        (u->output_device != desired_output || u->input_device != desired_input)) {
        u->output_device = desired_output;
        u->input_device = desired_input;
        output_name = pa_audio_manager_output_device_to_string(desired_output);
        input_name = pa_audio_manager_input_device_to_string(desired_input);
        update_output_device_properties(u, output_name);
        update_input_device_properties(u, input_name);
        sync_sink_port_to_device(u, desired_output);
        sync_source_port_to_device(u, desired_input);
    }

    ret = sync_selected_call_transport(u, target);
    if (ret < 0)
        goto fail;

    if (target != PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH)
        end_bluetooth_session(u);

    set_call_policy_defaults(u, target);
    sync_call_controls(u);
    return 0;

fail:
    restore_ret = restore_call_state(u,
                                     was_active,
                                     old_output,
                                     old_input,
                                     old_transport,
                                     old_target);
    if (restore_ret < 0)
        pa_log_error("failed to restore previous call state: %d", restore_ret);
    if (bluetooth_started_here && old_target != PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH)
        end_bluetooth_session(u);
    if (was_active)
        set_call_policy_defaults(u, old_target);
    else if (policy_defaults_saved_here)
        restore_call_policy_defaults(u);
    return ret;
}

static int
leave_call(struct userdata *u)
{
    AudioManager *manager;
    pa_audio_manager_bridge_target old_bridge;
    int ret;

    pa_assert(u);

    if (u->shared == NULL)
        return 0;

    manager = pa_audio_manager_shared_manager(u->shared);
    old_bridge = u->bridge_target;

    /* host PCM and loopbacks must be gone before modem speech stops */
    destroy_call_pcm(u);

    if (!audio_manager_call_is_active(manager)) {
        end_bluetooth_session(u);
        restore_call_policy_defaults(u);
        return 0;
    }

    ret = audio_manager_call_stop(manager);
    if (ret < 0) {
        pa_log_error("failed to stop call: %d", ret);
        if (audio_manager_call_is_active(manager) &&
            audio_manager_call_get_transport(manager) == AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL &&
            create_call_pcm(u) == 0 &&
            old_bridge != PA_AUDIO_MANAGER_BRIDGE_NONE &&
            start_bridge(u, old_bridge) < 0)
            pa_log_error("failed to restore host PCM bridge after call stop failure");
        return ret;
    }

    ret = ensure_paired_call_devices(u);
    if (ret < 0)
        pa_log_warn("failed to restore normal audio devices after call: %d", ret);
    end_bluetooth_session(u);
    restore_call_policy_defaults(u);
    return 0;
}

static AudioManagerCaptureRole
capture_role_for_source_output(struct userdata *u,
                               pa_source_output *output)
{
    const AudioManagerCapabilities *capabilities;
    AudioManagerCaptureRole role;
    const char *value;

    pa_assert(u);
    pa_assert(output);

    capabilities = pa_audio_manager_shared_capabilities(u->shared);

    value = pa_proplist_gets(output->proplist, "audio_manager.capture_role");
    if (value != NULL &&
        pa_audio_manager_capture_role_parse(value, &role) &&
        (capabilities->capture_roles & AUDIO_MANAGER_CAPTURE_ROLE_MASK(role)))
        return role;

    value = pa_proplist_gets(output->proplist, PA_PROP_MEDIA_ROLE);
    if (value == NULL)
        return AUDIO_MANAGER_CAPTURE_ROLE_DEFAULT;

    if (pa_streq(value, "video") &&
        (capabilities->capture_roles &
         AUDIO_MANAGER_CAPTURE_ROLE_MASK(AUDIO_MANAGER_CAPTURE_ROLE_CAMCORDER)))
        return AUDIO_MANAGER_CAPTURE_ROLE_CAMCORDER;

    if (pa_streq(value, "phone") &&
        (capabilities->capture_roles &
         AUDIO_MANAGER_CAPTURE_ROLE_MASK(AUDIO_MANAGER_CAPTURE_ROLE_VOICE_RECOGNITION)))
        return AUDIO_MANAGER_CAPTURE_ROLE_VOICE_RECOGNITION;

    if (pa_streq(value, "production") &&
        (capabilities->capture_roles &
         AUDIO_MANAGER_CAPTURE_ROLE_MASK(AUDIO_MANAGER_CAPTURE_ROLE_UNPROCESSED)))
        return AUDIO_MANAGER_CAPTURE_ROLE_UNPROCESSED;

    return AUDIO_MANAGER_CAPTURE_ROLE_DEFAULT;
}

static void
update_capture_policy(struct userdata *u)
{
    pa_source_output *output;
    AudioManagerCaptureRole role;
    uint32_t idx;
    unsigned n_outputs = 0;

    pa_assert(u);

    if (!u->capture_policy || u->source == NULL)
        return;

    role = u->default_capture_role;

    PA_IDXSET_FOREACH(output, u->source->outputs, idx) {
        if (output->destination_source != NULL)
            continue;

        n_outputs++;
        if (n_outputs == 1)
            role = capture_role_for_source_output(u, output);
        else {
            role = u->default_capture_role;
            break;
        }
    }

    if (role == pa_audio_manager_source_get_capture_role(u->source))
        return;

    if (pa_audio_manager_source_set_capture_role(u->source, role) < 0)
        pa_log_warn("failed to apply capture policy role '%s'",
                    pa_audio_manager_capture_role_to_string(role));
}

static pa_hook_result_t
source_output_policy_hook(void *hook_data, void *call_data, void *slot_data)
{
    struct userdata *u = slot_data;

    (void)hook_data;
    (void)call_data;
    update_capture_policy(u);
    return PA_HOOK_OK;
}

static pa_hook_result_t
sink_volume_changed_hook(void *hook_data, void *call_data, void *slot_data)
{
    struct userdata *u = slot_data;
    pa_sink *sink = call_data;
    const AudioManagerCapabilities *capabilities;
    AudioManager *manager;
    double volume;
    int ret;

    (void)hook_data;

    if (sink != u->sink || !call_is_active(u) ||
        call_transport(u) != AUDIO_MANAGER_CALL_TRANSPORT_HOSTLESS)
        return PA_HOOK_OK;

    capabilities = pa_audio_manager_shared_capabilities(u->shared);
    if (!(capabilities->flags & AUDIO_MANAGER_CAP_CALL_VOLUME))
        return PA_HOOK_OK;

    manager = pa_audio_manager_shared_manager(u->shared);
    volume = (double)current_sink_volume(sink) / PA_VOLUME_NORM;
    ret = audio_manager_call_set_volume(manager, volume);
    if (ret < 0)
        pa_log_warn("failed to update call volume: %d", ret);

    return PA_HOOK_OK;
}

static pa_hook_result_t
sink_mute_changed_hook(void *hook_data, void *call_data, void *slot_data)
{
    struct userdata *u = slot_data;
    pa_sink *sink = call_data;
    const AudioManagerCapabilities *capabilities;
    int ret;

    (void)hook_data;

    if (sink != u->sink || !call_is_active(u))
        return PA_HOOK_OK;

    capabilities = pa_audio_manager_shared_capabilities(u->shared);
    if (!(capabilities->flags & AUDIO_MANAGER_CAP_CALL_DOWNLINK_MUTE))
        return PA_HOOK_OK;

    ret = audio_manager_call_set_downlink_mute(pa_audio_manager_shared_manager(u->shared),
                                               sink->muted);
    if (ret < 0)
        pa_log_warn("failed to update downlink mute: %d", ret);

    return PA_HOOK_OK;
}

static pa_hook_result_t
source_mute_changed_hook(void *hook_data, void *call_data, void *slot_data)
{
    struct userdata *u = slot_data;
    pa_source *source = call_data;
    const AudioManagerCapabilities *capabilities;
    int ret;

    (void)hook_data;

    if (source != u->source || !call_is_active(u))
        return PA_HOOK_OK;

    capabilities = pa_audio_manager_shared_capabilities(u->shared);
    if (!(capabilities->flags & AUDIO_MANAGER_CAP_CALL_UPLINK_MUTE))
        return PA_HOOK_OK;

    ret = audio_manager_call_set_uplink_mute(pa_audio_manager_shared_manager(u->shared),
                                             source->muted);
    if (ret < 0)
        pa_log_warn("failed to update uplink mute: %d", ret);

    return PA_HOOK_OK;
}

static void
update_external_profile_availability(struct userdata *u)
{
    bool bluetooth;
    bool usb;

    pa_assert(u);

    if (u->card == NULL || u->external_transition_depth > 0)
        return;

    bluetooth = pa_audio_manager_bridge_bluetooth_hfp_available(u->module->core);
    usb = pa_audio_manager_bridge_target_available(u->module->core,
                                                   PA_AUDIO_MANAGER_BRIDGE_USB);

    u->external_transition_depth++;
    if (u->profile_bluetooth != NULL)
        pa_card_profile_set_available(u->profile_bluetooth,
                                      bluetooth ? PA_AVAILABLE_YES : PA_AVAILABLE_NO);
    if (u->profile_usb != NULL)
        pa_card_profile_set_available(u->profile_usb,
                                      usb ? PA_AVAILABLE_YES : PA_AVAILABLE_NO);
    u->external_transition_depth--;
}

static void
restore_external_call_to_local(struct userdata *u, const char *reason)
{
    pa_card_profile *target;

    pa_assert(u);

    if (u->external_transition_depth > 0 || u->card == NULL)
        return;

    target = u->profile_call != NULL ? u->profile_call : u->profile_default;
    if (target == NULL || u->card->active_profile == target)
        return;

    pa_log_warn("%s disappeared during call. falling back to profile '%s'",
                reason, target->name);

    u->external_transition_depth++;
    if (pa_card_set_profile(u->card, target, false) < 0)
        pa_log_error("failed to restore call back to '%s'", target->name);
    u->external_transition_depth--;
}

static pa_hook_result_t
external_device_changed_hook(void *hook_data, void *call_data, void *slot_data)
{
    struct userdata *u = slot_data;

    (void)hook_data;
    (void)call_data;

    if (u->external_transition_depth == 0)
        update_external_profile_availability(u);
    return PA_HOOK_OK;
}

static pa_hook_result_t
external_sink_unlink_hook(void *hook_data, void *call_data, void *slot_data)
{
    struct userdata *u = slot_data;
    pa_sink *sink = call_data;
    enum profile_mode mode;

    (void)hook_data;

    if (u->external_transition_depth > 0)
        return PA_HOOK_OK;

    mode = active_profile_mode(u);
    if (u->bridge != NULL &&
        (mode == PROFILE_MODE_CALL_USB || mode == PROFILE_MODE_CALL_BLUETOOTH) &&
        pa_audio_manager_bridge_uses_sink(u->bridge, sink)) {
        restore_external_call_to_local(u,
                                       mode == PROFILE_MODE_CALL_USB ?
                                       "USB call device" : "Bluetooth call device");
    } else if (mode == PROFILE_MODE_CALL_BLUETOOTH &&
               u->bluetooth_card_name != NULL &&
               sink != NULL && sink->card != NULL && sink->card->name != NULL &&
               pa_streq(sink->card->name, u->bluetooth_card_name) &&
               pa_audio_manager_bridge_sink_is_target(sink,
                                                      PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH)) {
        restore_external_call_to_local(u, "Bluetooth HFP sink");
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t
external_source_unlink_hook(void *hook_data, void *call_data, void *slot_data)
{
    struct userdata *u = slot_data;
    pa_source *source = call_data;
    enum profile_mode mode;

    (void)hook_data;

    if (u->external_transition_depth > 0)
        return PA_HOOK_OK;

    mode = active_profile_mode(u);
    if (u->bridge != NULL &&
        (mode == PROFILE_MODE_CALL_USB || mode == PROFILE_MODE_CALL_BLUETOOTH) &&
        pa_audio_manager_bridge_uses_source(u->bridge, source)) {
        restore_external_call_to_local(u,
                                       mode == PROFILE_MODE_CALL_USB ?
                                       "USB call device" : "Bluetooth call device");
    } else if (mode == PROFILE_MODE_CALL_BLUETOOTH &&
               u->bluetooth_card_name != NULL &&
               source != NULL && source->card != NULL && source->card->name != NULL &&
               pa_streq(source->card->name, u->bluetooth_card_name) &&
               pa_audio_manager_bridge_source_is_target(source,
                                                        PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH)) {
        restore_external_call_to_local(u, "Bluetooth HFP source");
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t
external_card_unlink_hook(void *hook_data, void *call_data, void *slot_data)
{
    struct userdata *u = slot_data;
    pa_card *card = call_data;
    enum profile_mode mode;

    (void)hook_data;

    if (u->external_transition_depth > 0 ||
        u->bluetooth_card_name == NULL || card == NULL || card->name == NULL ||
        !pa_streq(card->name, u->bluetooth_card_name))
        return PA_HOOK_OK;

    mode = active_profile_mode(u);
    if (mode == PROFILE_MODE_CALL_BLUETOOTH)
        restore_external_call_to_local(u, "Bluetooth call card");

    return PA_HOOK_OK;
}

static void
recovery_timer_cb(pa_mainloop_api *api,
                  pa_time_event *event,
                  const struct timeval *tv,
                  void *userdata)
{
    struct userdata *u = userdata;
    AudioManager *manager;
    AudioManagerCallState state;
    enum profile_mode mode;
    pa_audio_manager_bridge_target target;
    int ret;

    (void)api;
    (void)tv;

    pa_assert(u);
    pa_assert(event);

    if (u->shared == NULL)
        return;

    manager = pa_audio_manager_shared_manager(u->shared);
    state = audio_manager_call_get_state(manager);
    mode = active_profile_mode(u);
    if (u->external_transition_depth == 0)
        update_external_profile_availability(u);

    if (audio_manager_call_is_active(manager) &&
        audio_manager_call_get_transport(manager) == AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL &&
        (mode == PROFILE_MODE_CALL ||
         mode == PROFILE_MODE_CALL_BLUETOOTH ||
         mode == PROFILE_MODE_CALL_USB) &&
        (u->call_pcm == NULL || pa_audio_manager_call_pcm_failed(u->call_pcm)) &&
        state == AUDIO_MANAGER_CALL_STATE_ACTIVE) {
        target = u->bridge_target;
        if (target == PA_AUDIO_MANAGER_BRIDGE_NONE) {
            if (mode == PROFILE_MODE_CALL_BLUETOOTH)
                target = PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH;
            else if (mode == PROFILE_MODE_CALL_USB)
                target = PA_AUDIO_MANAGER_BRIDGE_USB;
            else
                target = PA_AUDIO_MANAGER_BRIDGE_LOCAL;
        }
        pa_log_warn("call host PCM failed. recreating modem endpoints");
        destroy_call_pcm(u);
        if (create_call_pcm(u) < 0)
            pa_log_error("failed to recreate host PCM. retrying");
        else if (target != PA_AUDIO_MANAGER_BRIDGE_NONE &&
                 start_bridge(u, target) < 0) {
            pa_log_error("failed to restore %s bridge after host PCM recovery",
                         pa_audio_manager_bridge_target_to_string(target));
            if (target != PA_AUDIO_MANAGER_BRIDGE_LOCAL)
                restore_external_call_to_local(u,
                                               target == PA_AUDIO_MANAGER_BRIDGE_USB ?
                                               "USB call bridge" : "Bluetooth call bridge");
        }
    }

    if (state == AUDIO_MANAGER_CALL_STATE_ACTIVE &&
        u->last_call_state != AUDIO_MANAGER_CALL_STATE_ACTIVE &&
        audio_manager_call_is_active(manager)) {
        if (mode == PROFILE_MODE_CALL) {
            ret = ensure_paired_call_devices(u);
            if (ret < 0) {
                pa_log_warn("failed to reapply local call route after modem recovery: %d", ret);
            } else {
                ret = sync_selected_call_transport(u, PA_AUDIO_MANAGER_BRIDGE_LOCAL);
                if (ret < 0)
                    pa_log_warn("failed to synchronize local route after modem recovery: %d", ret);
            }
        }
        sync_call_controls(u);
    }

    u->last_call_state = state;
    pa_core_rttime_restart(u->module->core,
                           event,
                           pa_rtclock_now() + CALL_RECOVERY_INTERVAL_USEC);
}

static void
start_recovery_timer(struct userdata *u)
{
    pa_assert(u);

    if (u->recovery_timer != NULL)
        return;

    u->recovery_timer = pa_core_rttime_new(u->module->core,
                                           pa_rtclock_now() + CALL_RECOVERY_INTERVAL_USEC,
                                           recovery_timer_cb,
                                           u);
}

static void
stop_recovery_timer(struct userdata *u)
{
    pa_assert(u);

    if (u->recovery_timer == NULL)
        return;

    u->module->core->mainloop->time_free(u->recovery_timer);
    u->recovery_timer = NULL;
}

static int
card_set_profile_cb(pa_card *card, pa_card_profile *profile)
{
    struct userdata *u;
    struct profile_data *data;
    const char *transport;
    const char *route;
    int ret;

    pa_assert(card);
    pa_assert(profile);
    pa_assert_se(u = card->userdata);

    data = PA_CARD_PROFILE_DATA(profile);

    switch (data->mode) {
    case PROFILE_MODE_DEFAULT:
        ret = leave_call(u);
        route = "none";
        break;
    case PROFILE_MODE_CALL:
        ret = enter_call(u, PA_AUDIO_MANAGER_BRIDGE_LOCAL);
        route = "local";
        break;
    case PROFILE_MODE_CALL_BLUETOOTH:
        ret = enter_call(u, PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH);
        route = "bluetooth";
        break;
    case PROFILE_MODE_CALL_USB:
        ret = enter_call(u, PA_AUDIO_MANAGER_BRIDGE_USB);
        route = "usb";
        break;
    default:
        return -1;
    }

    if (ret < 0) {
        const char *reason = (-ret > 0 && -ret < 4096) ? strerror(-ret) : "unknown error";

        pa_log_error("failed to change audio-manager card profile to '%s': %s (%d)",
                     profile->name, reason, ret);
        return -1;
    }

    if (data->mode == PROFILE_MODE_DEFAULT) {
        stop_recovery_timer(u);
        transport = "none";
    } else {
        start_recovery_timer(u);
        transport = audio_manager_call_get_transport(pa_audio_manager_shared_manager(u->shared)) == AUDIO_MANAGER_CALL_TRANSPORT_HOSTFUL ?
                                                     "hostful" : "hostless";
    }
    pa_proplist_sets(card->proplist, "audio_manager.call.transport", transport);
    pa_proplist_sets(card->proplist, "audio_manager.call.route", route);
    pa_log_info("audio-manager card profile changed to '%s' (transport=%s route=%s)",
                profile->name, transport, route);
    return 0;
}

static int
card_message_handler(const char *object_path,
                     const char *message,
                     const pa_json_object *parameters,
                     char **response,
                     void *userdata)
{
    struct userdata *u = userdata;
    AudioManagerPlaybackRole role;
    int ret;

    pa_assert(u);
    pa_assert(message);
    pa_assert(response);
    pa_assert(pa_streq(object_path, AUDIO_MANAGER_MESSAGE_PATH));
    (void)parameters;

    if (pa_streq(message, "playback-role-low-latency"))
        role = AUDIO_MANAGER_PLAYBACK_ROLE_LOW_LATENCY;
    else if (pa_streq(message, "playback-role-power-saving"))
        role = AUDIO_MANAGER_PLAYBACK_ROLE_POWER_SAVING;
    else if (pa_streq(message, "get-playback-role")) {
        *response = pa_xstrdup(pa_audio_manager_playback_role_to_string(pa_audio_manager_sink_get_playback_role(u->sink)));
        return PA_OK;
    } else
        return -PA_ERR_NOTIMPLEMENTED;

    ret = pa_audio_manager_sink_set_playback_role(u->sink, role);
    if (ret < 0)
        return -PA_ERR_INVALID;

    pa_proplist_sets(u->card->proplist,
                     "audio_manager.playback_role",
                     pa_audio_manager_playback_role_to_string(pa_audio_manager_sink_get_playback_role(u->sink)));
    *response = pa_xstrdup(pa_audio_manager_playback_role_to_string(pa_audio_manager_sink_get_playback_role(u->sink)));
    return PA_OK;
}

static void
userdata_free(struct userdata *u)
{
    if (u == NULL)
        return;

    if (u->message_handler_registered) {
        pa_message_handler_unregister(u->module->core, AUDIO_MANAGER_MESSAGE_PATH);
        u->message_handler_registered = false;
    }

    if (u->recovery_timer != NULL) {
        u->module->core->mainloop->time_free(u->recovery_timer);
        u->recovery_timer = NULL;
    }

    if (u->jack != NULL) {
        pa_audio_manager_jack_free(u->jack);
        u->jack = NULL;
    }

    if (u->shared != NULL) {
        leave_call(u);
        end_bluetooth_session(u);
        destroy_call_pcm(u);
    }

    restore_call_policy_defaults(u);

    if (u->source != NULL) {
        pa_audio_manager_source_free(u->source);
        u->source = NULL;
    }

    if (u->sink != NULL) {
        pa_audio_manager_sink_free(u->sink);
        u->sink = NULL;
    }

    if (u->card != NULL) {
        pa_card_free(u->card);
        u->card = NULL;
    }

    if (u->shared != NULL) {
        pa_audio_manager_shared_unref(u->shared);
        u->shared = NULL;
    }

    pa_xfree(u->bluetooth_card_name);
    pa_xfree(u->bluetooth_restore_profile);
    pa_xfree(u->saved_policy_default_sink);
    pa_xfree(u->saved_policy_default_source);
    pa_xfree(u);
}

int
pa__get_n_used(pa_module *module)
{
    struct userdata *u;
    int used = 0;

    pa_assert(module);

    u = module->userdata;
    if (u == NULL)
        return 0;

    if (u->sink != NULL)
        used += pa_sink_linked_by(u->sink);
    if (u->source != NULL)
        used += pa_source_linked_by(u->source);
    if (u->call_pcm != NULL) {
        pa_sink *uplink = pa_audio_manager_call_pcm_uplink_sink(u->call_pcm);
        pa_source *downlink = pa_audio_manager_call_pcm_downlink_source(u->call_pcm);

        if (uplink != NULL)
            used += pa_sink_linked_by(uplink);
        if (downlink != NULL)
            used += pa_source_linked_by(downlink);
    }

    return used;
}

void
pa__done(pa_module *module)
{
    struct userdata *u;

    pa_assert(module);

    if ((u = module->userdata) == NULL)
        return;

    module->userdata = NULL;
    userdata_free(u);
}

int
pa__init(pa_module *module)
{
    struct userdata *u = NULL;
    pa_card_new_data card_data;
    pa_card_profile *profile;
    pa_card_profile *profiles[5];
    size_t n_profiles = 0;
    struct profile_data *profile_data;
    pa_audio_manager_sink_options sink_options = PA_AUDIO_MANAGER_SINK_OPTIONS_INIT;
    pa_audio_manager_source_options source_options = PA_AUDIO_MANAGER_SOURCE_OPTIONS_INIT;
    const AudioManagerCapabilities *capabilities;
    AudioManager *manager;
    AudioManagerPlaybackRole playback_role;
    const char *output_name;
    const char *input_name;
    const char *output_port;
    const char *input_port;
    const char *playback_role_name;
    char *supported_playback_roles = NULL;
    bool card_data_initialized = false;
    int ret;

    pa_assert(module);

    if (module->argument != NULL && module->argument[0] != '\0') {
        pa_log_error("module-audio-manager-card does not accept module arguments");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->module = module;
    module->userdata = u;

    u->shared = pa_audio_manager_shared_get(module->core);
    if (u->shared == NULL)
        goto fail;

    capabilities = pa_audio_manager_shared_capabilities(u->shared);
    manager = pa_audio_manager_shared_manager(u->shared);

    u->capture_policy =
        (capabilities->flags & AUDIO_MANAGER_CAP_CAPTURE_ROLES) != 0;

    playback_role = choose_initial_playback_role(capabilities);
    if (!is_playback_role_supported(capabilities, playback_role)) {
        pa_log_error("audio-manager backend exposes no playback role");
        goto fail;
    }
    playback_role_name = pa_audio_manager_playback_role_to_string(playback_role);

    u->output_device = choose_initial_output(capabilities);
    u->input_device = choose_initial_input(capabilities);
    if (u->output_device == AUDIO_MANAGER_OUTPUT_NONE ||
        u->input_device == AUDIO_MANAGER_INPUT_NONE) {
        pa_log_error("audio-manager backend exposes no usable local playback or capture route");
        goto fail;
    }

    output_name = pa_audio_manager_output_device_to_string(u->output_device);
    input_name = pa_audio_manager_input_device_to_string(u->input_device);
    output_port = output_port_name(u->output_device);
    input_port = input_port_name(u->input_device);

    ret = audio_manager_set_devices(manager, u->output_device, u->input_device);
    if (ret < 0) {
        pa_log_error("failed to select initial audio-manager devices %s/%s: %d",
                     output_name, input_name, ret);
        goto fail;
    }

    pa_card_new_data_init(&card_data);
    card_data_initialized = true;
    card_data.driver = __FILE__;
    card_data.module = module;

    pa_card_new_data_set_name(&card_data, AUDIO_MANAGER_CARD_NAME);
    pa_proplist_sets(card_data.proplist, PA_PROP_DEVICE_CLASS, "sound");
    pa_proplist_sets(card_data.proplist, PA_PROP_DEVICE_API, "audio-manager");
    pa_proplist_sets(card_data.proplist,
                     "audio_manager.backend",
                     audio_manager_get_backend_name(manager));
    pa_proplist_sets(card_data.proplist,
                     "audio_manager.capture_policy",
                     u->capture_policy ? "source-output" : "none");
    pa_proplist_sets(card_data.proplist,
                     "audio_manager.call.transport",
                     "none");
    pa_proplist_sets(card_data.proplist,
                     "audio_manager.call.route",
                     "none");

    supported_playback_roles = supported_playback_roles_string(capabilities);
    pa_proplist_sets(card_data.proplist,
                     "audio_manager.playback_role",
                     playback_role_name);
    pa_proplist_sets(card_data.proplist,
                     "audio_manager.playback_roles",
                     supported_playback_roles);
    pa_xfree(supported_playback_roles);
    supported_playback_roles = NULL;

    profile = pa_card_profile_new("default", "Default", sizeof(struct profile_data));
    profile->priority = 100;
    profile->available = PA_AVAILABLE_YES;
    profile->n_sinks = 1;
    profile->n_sources = 1;
    profile->max_sink_channels = 2;
    profile->max_source_channels = 2;
    profile_data = PA_CARD_PROFILE_DATA(profile);
    profile_data->mode = PROFILE_MODE_DEFAULT;
    pa_assert_se(pa_hashmap_put(card_data.profiles, profile->name, profile) >= 0);
    u->profile_default = profile;
    profiles[n_profiles++] = profile;

    if ((capabilities->flags & AUDIO_MANAGER_CAP_CELLULAR_CALL) &&
        choose_call_local_output(u, capabilities, u->output_device) != AUDIO_MANAGER_OUTPUT_NONE) {
        profile = pa_card_profile_new("voicecall",
                                      "Voice Call",
                                      sizeof(struct profile_data));
        profile->priority = 20;
        profile->available = PA_AVAILABLE_YES;
        profile->n_sinks = 2;
        profile->n_sources = 2;
        profile->max_sink_channels = 2;
        profile->max_source_channels = 2;
        profile_data = PA_CARD_PROFILE_DATA(profile);
        profile_data->mode = PROFILE_MODE_CALL;
        pa_assert_se(pa_hashmap_put(card_data.profiles, profile->name, profile) >= 0);
        u->profile_call = profile;
        profiles[n_profiles++] = profile;
    }

    if ((capabilities->cellular_call_output_devices &
         AUDIO_MANAGER_OUTPUT_DEVICE_MASK(AUDIO_MANAGER_OUTPUT_BLUETOOTH)) &&
        (capabilities->cellular_call_input_devices &
         AUDIO_MANAGER_INPUT_DEVICE_MASK(AUDIO_MANAGER_INPUT_BLUETOOTH)) &&
        get_cellular_route_transports(u,
                                      AUDIO_MANAGER_OUTPUT_BLUETOOTH,
                                      AUDIO_MANAGER_INPUT_BLUETOOTH) != 0) {
        profile = pa_card_profile_new("voicecall-bluetooth",
                                      "Voice Call Bluetooth",
                                      sizeof(struct profile_data));
        profile->priority = 9;
        profile->available = PA_AVAILABLE_UNKNOWN;
        profile->n_sinks = 2;
        profile->n_sources = 2;
        profile->max_sink_channels = 2;
        profile->max_source_channels = 2;
        profile_data = PA_CARD_PROFILE_DATA(profile);
        profile_data->mode = PROFILE_MODE_CALL_BLUETOOTH;
        pa_assert_se(pa_hashmap_put(card_data.profiles, profile->name, profile) >= 0);
        u->profile_bluetooth = profile;
        profiles[n_profiles++] = profile;
    }

    if ((capabilities->cellular_call_output_devices &
         AUDIO_MANAGER_OUTPUT_DEVICE_MASK(AUDIO_MANAGER_OUTPUT_USB)) &&
        (capabilities->cellular_call_input_devices &
         AUDIO_MANAGER_INPUT_DEVICE_MASK(AUDIO_MANAGER_INPUT_USB)) &&
        get_cellular_route_transports(u,
                                      AUDIO_MANAGER_OUTPUT_USB,
                                      AUDIO_MANAGER_INPUT_USB) != 0) {
        profile = pa_card_profile_new("voicecall-usb",
                                      "Voice Call USB Headset",
                                      sizeof(struct profile_data));
        profile->priority = 8;
        profile->available = PA_AVAILABLE_UNKNOWN;
        profile->n_sinks = 2;
        profile->n_sources = 2;
        profile->max_sink_channels = 2;
        profile->max_source_channels = 2;
        profile_data = PA_CARD_PROFILE_DATA(profile);
        profile_data->mode = PROFILE_MODE_CALL_USB;
        pa_assert_se(pa_hashmap_put(card_data.profiles, profile->name, profile) >= 0);
        u->profile_usb = profile;
        profiles[n_profiles++] = profile;
    }

    if (add_capability_ports(module,
                             &card_data,
                             profiles,
                             n_profiles,
                             capabilities) < 0) {
        pa_log_error("failed to create audio-manager card ports");
        goto fail;
    }

    if (pa_hashmap_get(card_data.ports, output_port) == NULL ||
        pa_hashmap_get(card_data.ports, input_port) == NULL) {
        pa_log_error("initial audio-manager port pair %s/%s was not created",
                     output_port, input_port);
        goto fail;
    }

    u->card = pa_card_new(module->core, &card_data);
    pa_card_new_data_done(&card_data);
    card_data_initialized = false;
    if (u->card == NULL) {
        pa_log_error("failed to create audio-manager card");
        goto fail;
    }

    u->card->userdata = u;
    u->card->set_profile = card_set_profile_cb;
    pa_card_choose_initial_profile(u->card);
    pa_card_put(u->card);

    sink_options.shared = u->shared;
    sink_options.card = u->card;
    sink_options.ports = u->card->ports;
    sink_options.active_port = output_port;
    sink_options.set_port = sink_set_port_cb;
    sink_options.initial_device = u->output_device;
    sink_options.set_initial_device = false;

    u->sink = pa_audio_manager_sink_new_full(module,
                                             __FILE__,
                                             &sink_options);
    if (u->sink == NULL) {
        pa_log_error("failed to create audio-manager card sink");
        goto fail;
    }

    source_options.shared = u->shared;
    source_options.card = u->card;
    source_options.ports = u->card->ports;
    source_options.active_port = input_port;
    source_options.set_port = source_set_port_cb;
    source_options.initial_device = u->input_device;
    source_options.set_initial_device = false;

    u->source = pa_audio_manager_source_new_full(module,
                                                 __FILE__,
                                                 &source_options);
    if (u->source == NULL) {
        pa_log_error("failed to create audio-manager card source");
        goto fail;
    }
    u->default_capture_role = pa_audio_manager_source_get_capture_role(u->source);
    pa_proplist_sets(u->card->proplist,
                     "audio_manager.playback_role",
                     pa_audio_manager_playback_role_to_string(pa_audio_manager_sink_get_playback_role(u->sink)));

    pa_message_handler_register(module->core,
                                AUDIO_MANAGER_MESSAGE_PATH,
                                "Audio Manager card controls",
                                card_message_handler,
                                u);
    u->message_handler_registered = true;

    pa_module_hook_connect(module,
                           &module->core->hooks[PA_CORE_HOOK_SINK_VOLUME_CHANGED],
                           PA_HOOK_NORMAL,
                           (pa_hook_cb_t)sink_volume_changed_hook,
                           u);
    pa_module_hook_connect(module,
                           &module->core->hooks[PA_CORE_HOOK_SINK_MUTE_CHANGED],
                           PA_HOOK_NORMAL,
                           (pa_hook_cb_t)sink_mute_changed_hook,
                           u);
    pa_module_hook_connect(module,
                           &module->core->hooks[PA_CORE_HOOK_SOURCE_MUTE_CHANGED],
                           PA_HOOK_NORMAL,
                           (pa_hook_cb_t)source_mute_changed_hook,
                           u);

    pa_module_hook_connect(module,
                           &module->core->hooks[PA_CORE_HOOK_SINK_UNLINK],
                           PA_HOOK_EARLY,
                           (pa_hook_cb_t)external_sink_unlink_hook,
                           u);
    pa_module_hook_connect(module,
                           &module->core->hooks[PA_CORE_HOOK_SOURCE_UNLINK],
                           PA_HOOK_EARLY,
                           (pa_hook_cb_t)external_source_unlink_hook,
                           u);
    pa_module_hook_connect(module,
                           &module->core->hooks[PA_CORE_HOOK_SINK_PUT],
                           PA_HOOK_LATE,
                           (pa_hook_cb_t)external_device_changed_hook,
                           u);
    pa_module_hook_connect(module,
                           &module->core->hooks[PA_CORE_HOOK_SOURCE_PUT],
                           PA_HOOK_LATE,
                           (pa_hook_cb_t)external_device_changed_hook,
                           u);
    pa_module_hook_connect(module,
                           &module->core->hooks[PA_CORE_HOOK_SINK_UNLINK_POST],
                           PA_HOOK_LATE,
                           (pa_hook_cb_t)external_device_changed_hook,
                           u);
    pa_module_hook_connect(module,
                           &module->core->hooks[PA_CORE_HOOK_SOURCE_UNLINK_POST],
                           PA_HOOK_LATE,
                           (pa_hook_cb_t)external_device_changed_hook,
                           u);
    pa_module_hook_connect(module,
                           &module->core->hooks[PA_CORE_HOOK_CARD_PUT],
                           PA_HOOK_LATE,
                           (pa_hook_cb_t)external_device_changed_hook,
                           u);
    pa_module_hook_connect(module,
                           &module->core->hooks[PA_CORE_HOOK_CARD_PROFILE_AVAILABLE_CHANGED],
                           PA_HOOK_LATE,
                           (pa_hook_cb_t)external_device_changed_hook,
                           u);
    pa_module_hook_connect(module,
                           &module->core->hooks[PA_CORE_HOOK_CARD_UNLINK],
                           PA_HOOK_EARLY,
                           (pa_hook_cb_t)external_card_unlink_hook,
                           u);

    if (u->capture_policy) {
        pa_module_hook_connect(module,
                               &module->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_PUT],
                               PA_HOOK_LATE,
                               (pa_hook_cb_t)source_output_policy_hook,
                               u);
        pa_module_hook_connect(module,
                               &module->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK_POST],
                               PA_HOOK_LATE,
                               (pa_hook_cb_t)source_output_policy_hook,
                               u);
        pa_module_hook_connect(module,
                               &module->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_PROPLIST_CHANGED],
                               PA_HOOK_LATE,
                               (pa_hook_cb_t)source_output_policy_hook,
                               u);
        pa_module_hook_connect(module,
                               &module->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_MOVE_FINISH],
                               PA_HOOK_LATE,
                               (pa_hook_cb_t)source_output_policy_hook,
                               u);
    }

    if (pa_hashmap_get(u->card->ports, "output-headphones") != NULL ||
        pa_hashmap_get(u->card->ports, "output-headset") != NULL ||
        pa_hashmap_get(u->card->ports, "input-headset-mic") != NULL)
        u->jack = pa_audio_manager_jack_new(module->core,
                                            "auto",
                                            "auto",
                                            "auto",
                                            jack_changed_cb,
                                            u);

    update_external_profile_availability(u);
    u->last_call_state = audio_manager_call_get_state(manager);

    pa_log_info("created audio-manager card '%s' using backend '%s', route=%s/%s, call=%s",
                u->card->name,
                audio_manager_get_backend_name(manager),
                output_name,
                input_name,
                (capabilities->flags & AUDIO_MANAGER_CAP_CELLULAR_CALL) ? "yes" : "no");

    return 0;

fail:
    pa_xfree(supported_playback_roles);
    if (card_data_initialized)
        pa_card_new_data_done(&card_data);
    if (u != NULL) {
        module->userdata = NULL;
        userdata_free(u);
    }
    return -1;
}
