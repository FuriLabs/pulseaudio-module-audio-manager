/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include "audio-manager-bridge.h"

#include <errno.h>
#include <string.h>
#include <strings.h>

#include <pulse/proplist.h>
#include <pulse/xmalloc.h>
#include <pulsecore/core.h>
#include <pulsecore/card.h>
#include <pulsecore/core-util.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/idxset.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/module.h>

struct pa_audio_manager_bridge {
    pa_core *core;
    pa_module *downlink_loopback;
    pa_module *uplink_loopback;
    pa_sink *target_sink;
    pa_source *target_source;
};

static bool
property_is(const pa_proplist *proplist, const char *key, const char *value)
{
    const char *actual;

    pa_assert(proplist);
    pa_assert(key);
    pa_assert(value);

    actual = pa_proplist_gets(proplist, key);
    return actual != NULL && pa_streq(actual, value);
}

static bool
property_contains_ci(const pa_proplist *proplist,
                     const char *key,
                     const char *needle)
{
    const char *actual;
    size_t actual_len;
    size_t needle_len;
    size_t i;

    pa_assert(proplist);
    pa_assert(key);
    pa_assert(needle);

    actual = pa_proplist_gets(proplist, key);
    if (actual == NULL)
        return false;

    actual_len = strlen(actual);
    needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > actual_len)
        return false;

    for (i = 0; i + needle_len <= actual_len; i++) {
        if (strncasecmp(actual + i, needle, needle_len) == 0)
            return true;
    }

    return false;
}

static bool
is_monitor_source(pa_source *source)
{
    const char *device_class;

    pa_assert(source);

    device_class = pa_proplist_gets(source->proplist, PA_PROP_DEVICE_CLASS);
    return device_class != NULL && pa_streq(device_class, "monitor");
}

static bool
is_bluetooth_hfp_proplist(pa_proplist *proplist)
{
    if (!property_is(proplist, PA_PROP_DEVICE_API, "bluez") &&
        !property_is(proplist, PA_PROP_DEVICE_BUS, "bluetooth"))
        return false;

    if (property_contains_ci(proplist, "bluetooth.protocol", "handsfree") ||
        property_contains_ci(proplist, "bluetooth.protocol", "headset") ||
        property_contains_ci(proplist, "bluetooth.profile", "handsfree") ||
        property_contains_ci(proplist, "bluetooth.profile", "headset"))
        return true;

    return property_contains_ci(proplist, PA_PROP_DEVICE_INTENDED_ROLES, "phone");
}

static bool
is_usb_proplist(pa_proplist *proplist)
{
    const char *path;

    if (property_is(proplist, PA_PROP_DEVICE_BUS, "usb"))
        return true;

    path = pa_proplist_gets(proplist, PA_PROP_DEVICE_BUS_PATH);
    return path != NULL && strstr(path, "usb") != NULL;
}

static bool
sink_matches_target(pa_sink *sink, pa_audio_manager_bridge_target target)
{
    pa_assert(sink);

    switch (target) {
    case PA_AUDIO_MANAGER_BRIDGE_LOCAL:
        return property_is(sink->proplist, PA_PROP_DEVICE_API, "audio-manager") &&
               !property_is(sink->proplist, PA_PROP_DEVICE_CLASS, "abstract");
    case PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH:
        return is_bluetooth_hfp_proplist(sink->proplist);
    case PA_AUDIO_MANAGER_BRIDGE_USB:
        return is_usb_proplist(sink->proplist);
    case PA_AUDIO_MANAGER_BRIDGE_NONE:
    default:
        return false;
    }
}

static bool
source_matches_target(pa_source *source, pa_audio_manager_bridge_target target)
{
    pa_assert(source);

    if (is_monitor_source(source))
        return false;

    switch (target) {
    case PA_AUDIO_MANAGER_BRIDGE_LOCAL:
        return property_is(source->proplist, PA_PROP_DEVICE_API, "audio-manager") &&
               !property_is(source->proplist, PA_PROP_DEVICE_CLASS, "abstract");
    case PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH:
        return is_bluetooth_hfp_proplist(source->proplist);
    case PA_AUDIO_MANAGER_BRIDGE_USB:
        return is_usb_proplist(source->proplist);
    case PA_AUDIO_MANAGER_BRIDGE_NONE:
    default:
        return false;
    }
}

bool
pa_audio_manager_bridge_find_device_on_card(pa_core *core,
                                            pa_audio_manager_bridge_target target,
                                            const char *card_name,
                                            pa_audio_manager_bridge_device *device)
{
    pa_sink *sink;
    pa_source *source;
    uint32_t sink_index;
    uint32_t source_index;

    pa_assert(core);
    pa_assert(device);

    memset(device, 0, sizeof(*device));
    PA_IDXSET_FOREACH(sink, core->sinks, sink_index) {
        if (!sink_matches_target(sink, target))
            continue;
        if (card_name != NULL &&
            (sink->card == NULL || sink->card->name == NULL ||
             !pa_streq(sink->card->name, card_name)))
            continue;

        PA_IDXSET_FOREACH(source, core->sources, source_index) {
            if (source->card == NULL || sink->card == NULL ||
                source->card != sink->card ||
                !source_matches_target(source, target))
                continue;

            device->sink = sink;
            device->source = source;
            return true;
        }
    }

    return false;
}

bool
pa_audio_manager_bridge_target_available(pa_core *core,
                                         pa_audio_manager_bridge_target target)
{
    pa_audio_manager_bridge_device device;

    return pa_audio_manager_bridge_find_device_on_card(core, target, NULL, &device);
}

static bool
is_bluetooth_card(pa_card *card)
{
    pa_assert(card);

    return property_is(card->proplist, PA_PROP_DEVICE_API, "bluez") ||
           property_is(card->proplist, PA_PROP_DEVICE_BUS, "bluetooth") ||
           (card->name != NULL && strncmp(card->name, "bluez_card.", 11) == 0);
}

static bool
profile_is_hfp(const pa_card_profile *profile)
{
    pa_assert(profile);

    return profile->name != NULL &&
           (strstr(profile->name, "handsfree") != NULL ||
            strstr(profile->name, "headset") != NULL);
}

#define SUSPEND_ON_IDLE_TIMEOUT_PROPERTY "module-suspend-on-idle.timeout"

static pa_card_profile *
find_non_hfp_profile(pa_card *card)
{
    static const char *const preferred[] = {
        "off",
        "a2dp_sink",
        "a2dp-sink",
        NULL,
    };
    pa_card_profile *profile;
    void *state = NULL;
    unsigned i;

    pa_assert(card);

    for (i = 0; preferred[i] != NULL; i++) {
        profile = pa_hashmap_get(card->profiles, preferred[i]);
        if (profile != NULL && profile->available != PA_AVAILABLE_NO &&
            !profile_is_hfp(profile))
            return profile;
    }

    PA_HASHMAP_FOREACH(profile, card->profiles, state) {
        if (profile->available != PA_AVAILABLE_NO && !profile_is_hfp(profile))
            return profile;
    }

    return NULL;
}

static char *
set_suspend_on_idle_timeout(pa_card *card, const char *timeout)
{
    const char *current;
    char *saved = NULL;

    pa_assert(card);

    current = pa_proplist_gets(card->proplist, SUSPEND_ON_IDLE_TIMEOUT_PROPERTY);
    if (current != NULL)
        saved = pa_xstrdup(current);

    if (timeout != NULL)
        pa_proplist_sets(card->proplist, SUSPEND_ON_IDLE_TIMEOUT_PROPERTY, timeout);
    else
        pa_proplist_unset(card->proplist, SUSPEND_ON_IDLE_TIMEOUT_PROPERTY);

    return saved;
}

static void
restore_suspend_on_idle_timeout(pa_card *card, char *saved)
{
    pa_assert(card);

    if (saved != NULL)
        pa_proplist_sets(card->proplist, SUSPEND_ON_IDLE_TIMEOUT_PROPERTY, saved);
    else
        pa_proplist_unset(card->proplist, SUSPEND_ON_IDLE_TIMEOUT_PROPERTY);

    pa_xfree(saved);
}

static pa_card_profile *
find_hfp_profile(pa_card *card)
{
    static const char *const preferred[] = {
        "handsfree_head_unit",
        "headset_head_unit",
        "handsfree-head-unit",
        "headset-head-unit",
        NULL,
    };
    pa_card_profile *profile;
    void *state = NULL;
    unsigned i;

    pa_assert(card);

    for (i = 0; preferred[i] != NULL; i++) {
        profile = pa_hashmap_get(card->profiles, preferred[i]);
        if (profile != NULL && profile->available != PA_AVAILABLE_NO)
            return profile;
    }

    PA_HASHMAP_FOREACH(profile, card->profiles, state) {
        if (profile->available != PA_AVAILABLE_NO && profile_is_hfp(profile))
            return profile;
    }

    return NULL;
}

static pa_card *
find_bluetooth_card(pa_core *core, const char *name)
{
    pa_card *card;
    uint32_t idx;

    pa_assert(core);

    PA_IDXSET_FOREACH(card, core->cards, idx) {
        if (!is_bluetooth_card(card))
            continue;
        if (name == NULL || pa_streq(card->name, name))
            return card;
    }

    return NULL;
}

bool
pa_audio_manager_bridge_bluetooth_hfp_available(pa_core *core)
{
    pa_card *card;
    uint32_t idx;

    pa_assert(core);

    PA_IDXSET_FOREACH(card, core->cards, idx) {
        if (is_bluetooth_card(card) && find_hfp_profile(card) != NULL)
            return true;
    }

    return false;
}

bool
pa_audio_manager_bridge_prepare_bluetooth(pa_core *core,
                                          char **card_name,
                                          char **restore_profile)
{
    pa_card *card;
    pa_card_profile *fallback;
    pa_card_profile *original;
    pa_card_profile *target;
    char *saved_timeout;
    uint32_t idx;
    int ret;

    pa_assert(core);
    pa_assert(card_name);
    pa_assert(restore_profile);

    *card_name = NULL;
    *restore_profile = NULL;

    PA_IDXSET_FOREACH(card, core->cards, idx) {
        if (!is_bluetooth_card(card))
            continue;

        target = find_hfp_profile(card);
        if (target == NULL)
            continue;

        *card_name = pa_xstrdup(card->name);
        original = card->active_profile;
        if (original != NULL)
            *restore_profile = pa_xstrdup(original->name);

        /*
         * module-suspend-on-idle reads this property when a sink or source is
         * created. HFP has to stay open for a hostless call even though no
         * PulseAudio PCM stream exists.
         * create the call HFP endpoints with idle suspension disabled.
         */
        if (original == target) {
            fallback = find_non_hfp_profile(card);
            if (fallback == NULL) {
                pa_log_error("cannot recreate BlueZ HFP profile '%s' without an alternate profile",
                             target->name);
                goto fail;
            }

            ret = pa_card_set_profile(card, fallback, false);
            if (ret < 0) {
                pa_log_error("failed to leave BlueZ HFP profile '%s' before recreating it",
                             target->name);
                goto fail;
            }
        }

        saved_timeout = set_suspend_on_idle_timeout(card, "-1");
        ret = pa_card_set_profile(card, target, false);
        restore_suspend_on_idle_timeout(card, saved_timeout);
        if (ret < 0) {
            pa_log_error("failed to switch BlueZ card '%s' to HFP profile '%s'",
                         card->name, target->name);
            if (original != NULL && card->active_profile != original &&
                pa_card_set_profile(card, original, false) < 0)
                pa_log_warn("failed to restore BlueZ card '%s' after HFP setup failure",
                            card->name);
            goto fail;
        }

        pa_log_info("switched BlueZ card '%s' to HFP profile '%s'",
                    card->name, target->name);
        return true;
    }

    return false;

fail:
    pa_xfree(*card_name);
    pa_xfree(*restore_profile);
    *card_name = NULL;
    *restore_profile = NULL;
    return false;
}

void
pa_audio_manager_bridge_restore_bluetooth(pa_core *core,
                                          const char *card_name,
                                          const char *restore_profile)
{
    pa_card *card;
    pa_card_profile *fallback;
    pa_card_profile *profile;

    if (core == NULL || card_name == NULL || restore_profile == NULL)
        return;

    card = find_bluetooth_card(core, card_name);
    if (card == NULL)
        return;

    profile = pa_hashmap_get(card->profiles, restore_profile);
    if (profile == NULL || profile->available == PA_AVAILABLE_NO)
        return;

    if (card->active_profile == profile) {
        /* recreate HFP and restore suspend-on-idle policies */
        if (!profile_is_hfp(profile))
            return;

        fallback = find_non_hfp_profile(card);
        if (fallback == NULL) {
            pa_log_warn("cannot restore BlueZ HFP profile '%s'", profile->name);
            return;
        }

        if (pa_card_set_profile(card, fallback, false) < 0 ||
            pa_card_set_profile(card, profile, false) < 0) {
            pa_log_warn("failed to recreate BlueZ card '%s' HFP profile '%s' after call",
                        card_name, restore_profile);
            return;
        }

        pa_log_info("restored BlueZ card '%s' HFP profile '%s'",
                    card_name, restore_profile);
        return;
    }

    if (pa_card_set_profile(card, profile, false) < 0)
        pa_log_warn("failed to restore BlueZ card '%s' profile '%s'",
                    card_name, restore_profile);
    else
        pa_log_info("restored BlueZ card '%s' profile '%s'",
                    card_name, restore_profile);
}

bool
pa_audio_manager_bridge_sink_is_target(pa_sink *sink,
                                       pa_audio_manager_bridge_target target)
{
    return sink != NULL && sink_matches_target(sink, target);
}

bool
pa_audio_manager_bridge_source_is_target(pa_source *source,
                                         pa_audio_manager_bridge_target target)
{
    return source != NULL && source_matches_target(source, target);
}

static pa_module *
load_loopback(pa_core *core,
              const char *source,
              const char *sink,
              unsigned latency_msec,
              const char *direction)
{
    pa_module *module = NULL;
    char *args;
    int ret;

    pa_assert(core);
    pa_assert(source);
    pa_assert(sink);
    pa_assert(direction);

    args = pa_sprintf_malloc("source=%s sink=%s latency_msec=%u source_dont_move=true sink_dont_move=true "
                             "remix=true source_output_properties=audio_manager.call_bridge=%s "
                             "sink_input_properties=audio_manager.call_bridge=%s",
                             source,
                             sink,
                             latency_msec,
                             direction,
                             direction);

    ret = pa_module_load(&module, core, "module-loopback", args);
    pa_xfree(args);

    if (ret < 0 || module == NULL) {
        pa_log_error("failed to load module-loopback for call %s bridge", direction);
        return NULL;
    }

    return module;
}

pa_audio_manager_bridge *
pa_audio_manager_bridge_new_on_card(pa_core *core,
                                    pa_audio_manager_bridge_target target,
                                    const char *card_name,
                                    pa_source *call_downlink,
                                    pa_sink *call_uplink,
                                    unsigned latency_msec)
{
    pa_audio_manager_bridge_device device;
    pa_audio_manager_bridge *bridge;

    pa_assert(core);
    pa_assert(call_downlink);
    pa_assert(call_uplink);

    if (target == PA_AUDIO_MANAGER_BRIDGE_NONE)
        return NULL;

    if (!pa_audio_manager_bridge_find_device_on_card(core, target, card_name, &device)) {
        pa_log_error("no duplex %s PulseAudio device is available for call",
                     pa_audio_manager_bridge_target_to_string(target));
        return NULL;
    }

    bridge = pa_xnew0(pa_audio_manager_bridge, 1);
    bridge->core = core;
    bridge->target_sink = pa_sink_ref(device.sink);
    bridge->target_source = pa_source_ref(device.source);

    bridge->downlink_loopback = load_loopback(core,
                                               call_downlink->name,
                                               device.sink->name,
                                               latency_msec,
                                               "downlink");
    if (bridge->downlink_loopback == NULL)
        goto fail;

    bridge->uplink_loopback = load_loopback(core,
                                             device.source->name,
                                             call_uplink->name,
                                             latency_msec,
                                             "uplink");
    if (bridge->uplink_loopback == NULL)
        goto fail;

    pa_log_info("bridged call host PCM to %s device: sink='%s' source='%s'",
                pa_audio_manager_bridge_target_to_string(target),
                device.sink->name,
                device.source->name);
    return bridge;

fail:
    pa_audio_manager_bridge_free(bridge);
    return NULL;
}

pa_audio_manager_bridge *
pa_audio_manager_bridge_new(pa_core *core,
                            pa_audio_manager_bridge_target target,
                            pa_source *call_downlink,
                            pa_sink *call_uplink,
                            unsigned latency_msec)
{
    return pa_audio_manager_bridge_new_on_card(core, target, NULL,
                                               call_downlink, call_uplink,
                                               latency_msec);
}

void
pa_audio_manager_bridge_free(pa_audio_manager_bridge *bridge)
{
    if (bridge == NULL)
        return;

    if (bridge->uplink_loopback != NULL) {
        pa_module_unload(bridge->uplink_loopback, true);
        bridge->uplink_loopback = NULL;
    }
    if (bridge->downlink_loopback != NULL) {
        pa_module_unload(bridge->downlink_loopback, true);
        bridge->downlink_loopback = NULL;
    }

    if (bridge->target_source != NULL)
        pa_source_unref(bridge->target_source);
    if (bridge->target_sink != NULL)
        pa_sink_unref(bridge->target_sink);

    pa_xfree(bridge);
}

bool
pa_audio_manager_bridge_uses_sink(const pa_audio_manager_bridge *bridge,
                                  const pa_sink *sink)
{
    return bridge != NULL && sink != NULL && bridge->target_sink == sink;
}

bool
pa_audio_manager_bridge_uses_source(const pa_audio_manager_bridge *bridge,
                                    const pa_source *source)
{
    return bridge != NULL && source != NULL && bridge->target_source == source;
}

const char *
pa_audio_manager_bridge_target_to_string(pa_audio_manager_bridge_target target)
{
    switch (target) {
    case PA_AUDIO_MANAGER_BRIDGE_LOCAL:
        return "local";
    case PA_AUDIO_MANAGER_BRIDGE_BLUETOOTH:
        return "bluetooth";
    case PA_AUDIO_MANAGER_BRIDGE_USB:
        return "usb";
    case PA_AUDIO_MANAGER_BRIDGE_NONE:
    default:
        return "none";
    }
}
