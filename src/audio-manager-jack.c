/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include "audio-manager-jack.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/input.h>
#include <linux/netlink.h>

#include <pulse/mainloop-api.h>
#include <pulse/xmalloc.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#define BITS_PER_LONG (sizeof(unsigned long) * 8U)
#define NBITS(x) (((x) + BITS_PER_LONG - 1U) / BITS_PER_LONG)
#define TEST_BIT(bit, array) (((array)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

struct pa_audio_manager_jack {
    pa_core *core;
    pa_mainloop_api *mainloop;
    pa_io_event *io_event;
    int fd;

    char *backend;
    char *path;

    bool known;
    bool headphone;
    bool microphone;

    bool evdev_has_headphone;
    bool evdev_has_microphone;
    bool evdev_has_lineout;
    bool evdev_has_physical;

    pa_audio_manager_jack_changed_cb changed_cb;
    void *userdata;
};

static void
jack_emit(pa_audio_manager_jack *jack,
          bool known,
          bool headphone,
          bool microphone)
{
    pa_assert(jack);

    if (jack->known == known &&
        jack->headphone == headphone &&
        jack->microphone == microphone)
        return;

    jack->known = known;
    jack->headphone = headphone;
    jack->microphone = microphone;

    if (jack->changed_cb != NULL)
        jack->changed_cb(jack->userdata, known, headphone, microphone);
}

static bool
read_text_file(const char *path, char *buffer, size_t size)
{
    int fd;
    ssize_t n;

    pa_assert(path);
    pa_assert(buffer);
    pa_assert(size > 1);

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    n = read(fd, buffer, size - 1);
    close(fd);
    if (n < 0)
        return false;

    buffer[n] = '\0';
    return true;
}

static bool
text_has_assignment(const char *text, const char *key, bool *value)
{
    char needle[96];
    const char *p;

    pa_assert(text);
    pa_assert(key);
    pa_assert(value);

    pa_snprintf(needle, sizeof(needle), "%s=", key);
    p = strstr(text, needle);
    if (p == NULL)
        return false;

    p += strlen(needle);
    if (*p == '1')
        *value = true;
    else if (*p == '0')
        *value = false;
    else
        return false;

    return true;
}

static bool
parse_extcon_state(const char *text, bool *headphone, bool *microphone)
{
    bool known = false;
    bool value;

    pa_assert(text);
    pa_assert(headphone);
    pa_assert(microphone);

    *headphone = false;
    *microphone = false;

    if (text_has_assignment(text, "HEADPHONE", &value)) {
        known = true;
        *headphone |= value;
    }
    if (text_has_assignment(text, "MICROPHONE", &value)) {
        known = true;
        *microphone |= value;
    }
    if (text_has_assignment(text, "LINE-OUT", &value)) {
        known = true;
        *headphone |= value;
    }
    if (text_has_assignment(text, "LINEOUT", &value)) {
        known = true;
        *headphone |= value;
    }
    if (text_has_assignment(text, "EAR_JACK", &value)) {
        known = true;
        *headphone |= value;
    }
    if (text_has_assignment(text, "HEADSET", &value)) {
        known = true;
        *headphone |= value;
        *microphone |= value;
    }

    if (*microphone)
        *headphone = true;

    return known;
}

static bool
parse_legacy_switch_state(const char *text, bool *headphone, bool *microphone)
{
    char *end = NULL;
    long state;

    pa_assert(text);
    pa_assert(headphone);
    pa_assert(microphone);

    errno = 0;
    state = strtol(text, &end, 0);
    if (errno != 0 || end == text)
        return false;

    switch (state) {
    case 0:
        *headphone = false;
        *microphone = false;
        return true;
    case 1:
        *headphone = true;
        *microphone = true;
        return true;
    case 2:
        *headphone = true;
        *microphone = false;
        return true;
    default:
        return false;
    }
}

static bool
sysfs_read_state(pa_audio_manager_jack *jack,
                 bool *headphone,
                 bool *microphone)
{
    char buffer[4096];

    pa_assert(jack);
    pa_assert(jack->path);

    if (!read_text_file(jack->path, buffer, sizeof(buffer)))
        return false;

    if (strstr(jack->path, "/class/switch/") != NULL)
        return parse_legacy_switch_state(buffer, headphone, microphone);

    return parse_extcon_state(buffer, headphone, microphone);
}

static void
sysfs_rescan(pa_audio_manager_jack *jack)
{
    bool headphone;
    bool microphone;

    if (!sysfs_read_state(jack, &headphone, &microphone)) {
        pa_log_warn("failed to read wired jack state from %s", jack->path);
        jack_emit(jack, false, false, false);
        return;
    }

    jack_emit(jack, true, headphone, microphone);
}

static bool
uevent_is_extcon_or_switch(const char *buffer, size_t size)
{
    size_t offset = 0;

    while (offset < size) {
        const char *entry = buffer + offset;
        size_t len = strnlen(entry, size - offset);

        if (len == 0)
            break;

        if (pa_streq(entry, "SUBSYSTEM=extcon") ||
            pa_streq(entry, "SUBSYSTEM=switch"))
            return true;

        offset += len + 1;
    }

    return false;
}

static void
sysfs_uevent_cb(pa_mainloop_api *api,
                pa_io_event *event,
                int fd,
                pa_io_event_flags_t events,
                void *userdata)
{
    pa_audio_manager_jack *jack = userdata;
    char buffer[8192];
    ssize_t n;

    pa_assert(api);
    pa_assert(event);
    pa_assert(jack);

    if (events & (PA_IO_EVENT_ERROR | PA_IO_EVENT_HANGUP)) {
        pa_log_warn("wired jack uevent monitor failed");
        api->io_enable(event, PA_IO_EVENT_NULL);
        jack_emit(jack, false, false, false);
        return;
    }

    for (;;) {
        n = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            pa_log_warn("failed to read wired jack uevent: %s", strerror(errno));
            api->io_enable(event, PA_IO_EVENT_NULL);
            jack_emit(jack, false, false, false);
            return;
        }
        if (n == 0)
            break;

        if (uevent_is_extcon_or_switch(buffer, (size_t)n))
            sysfs_rescan(jack);
    }
}

static int
open_uevent_socket(void)
{
    struct sockaddr_nl address;
    int fd;
    int receive_buffer = 64 * 1024;

    fd = socket(AF_NETLINK,
                SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                NETLINK_KOBJECT_UEVENT);
    if (fd < 0)
        return -1;

    memset(&address, 0, sizeof(address));
    address.nl_family = AF_NETLINK;
    address.nl_groups = 1;

    if (setsockopt(fd,
                   SOL_SOCKET,
                   SO_RCVBUF,
                   &receive_buffer,
                   sizeof(receive_buffer)) < 0)
        pa_log_debug("failed to enlarge wired jack uevent receive buffer: %s",
                     strerror(errno));

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static char *
normalize_sysfs_state_path(const char *path)
{
    struct stat st;

    pa_assert(path);

    if (stat(path, &st) < 0)
        return NULL;

    if (S_ISDIR(st.st_mode))
        return pa_sprintf_malloc("%s/state", path);

    if (S_ISREG(st.st_mode))
        return pa_xstrdup(path);

    return NULL;
}

static bool
sysfs_candidate_is_audio(const char *path)
{
    char buffer[4096];
    bool headphone;
    bool microphone;

    if (!read_text_file(path, buffer, sizeof(buffer)))
        return false;

    if (strstr(path, "/class/switch/") != NULL)
        return parse_legacy_switch_state(buffer, &headphone, &microphone);

    return parse_extcon_state(buffer, &headphone, &microphone);
}

static char *
find_sysfs_state_path(const char *requested)
{
    glob_t matches;
    size_t i;
    char *path;

    if (requested != NULL && !pa_streq(requested, "auto"))
        return normalize_sysfs_state_path(requested);

    memset(&matches, 0, sizeof(matches));
    if (glob("/sys/class/extcon/extcon*/state", 0, NULL, &matches) == 0) {
        for (i = 0; i < matches.gl_pathc; i++) {
            if (!sysfs_candidate_is_audio(matches.gl_pathv[i]))
                continue;
            path = pa_xstrdup(matches.gl_pathv[i]);
            globfree(&matches);
            return path;
        }
    }
    globfree(&matches);

    if (access("/sys/class/switch/h2w/state", R_OK) == 0)
        return pa_xstrdup("/sys/class/switch/h2w/state");

    return NULL;
}

static bool
evdev_query_state(pa_audio_manager_jack *jack,
                  bool *headphone,
                  bool *microphone)
{
    unsigned long state[NBITS(SW_MAX + 1)] = { 0 };

    pa_assert(jack);
    pa_assert(headphone);
    pa_assert(microphone);

    if (ioctl(jack->fd, EVIOCGSW(sizeof(state)), state) < 0)
        return false;

    *headphone = false;
    *microphone = false;

    if (jack->evdev_has_headphone && TEST_BIT(SW_HEADPHONE_INSERT, state))
        *headphone = true;
    if (jack->evdev_has_microphone && TEST_BIT(SW_MICROPHONE_INSERT, state))
        *microphone = true;
    if (jack->evdev_has_lineout && TEST_BIT(SW_LINEOUT_INSERT, state))
        *headphone = true;
    if (jack->evdev_has_physical &&
        !jack->evdev_has_headphone &&
        !jack->evdev_has_lineout &&
        TEST_BIT(SW_JACK_PHYSICAL_INSERT, state))
        *headphone = true;
    if (*microphone)
        *headphone = true;

    return true;
}

static bool
evdev_probe_fd(pa_audio_manager_jack *jack, int fd)
{
    unsigned long switches[NBITS(SW_MAX + 1)] = { 0 };
    bool useful = false;

    if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(switches)), switches) < 0)
        return false;

    jack->evdev_has_headphone = TEST_BIT(SW_HEADPHONE_INSERT, switches);
    useful |= jack->evdev_has_headphone;
    jack->evdev_has_microphone = TEST_BIT(SW_MICROPHONE_INSERT, switches);
    useful |= jack->evdev_has_microphone;
    jack->evdev_has_lineout = TEST_BIT(SW_LINEOUT_INSERT, switches);
    useful |= jack->evdev_has_lineout;
    jack->evdev_has_physical = TEST_BIT(SW_JACK_PHYSICAL_INSERT, switches);
    useful |= jack->evdev_has_physical;

    return useful;
}

static int
open_evdev_candidate(pa_audio_manager_jack *jack, const char *path)
{
    int fd;

    jack->evdev_has_headphone = false;
    jack->evdev_has_microphone = false;
    jack->evdev_has_lineout = false;
    jack->evdev_has_physical = false;

    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;

    if (!evdev_probe_fd(jack, fd)) {
        close(fd);
        return -1;
    }

    return fd;
}

static int
find_evdev_fd(pa_audio_manager_jack *jack,
              const char *requested,
              char **selected_path)
{
    glob_t matches;
    size_t i;
    int fd;

    pa_assert(jack);
    pa_assert(selected_path);

    if (requested != NULL && !pa_streq(requested, "auto")) {
        fd = open_evdev_candidate(jack, requested);
        if (fd >= 0)
            *selected_path = pa_xstrdup(requested);
        return fd;
    }

    memset(&matches, 0, sizeof(matches));
    if (glob("/dev/input/event*", 0, NULL, &matches) != 0) {
        globfree(&matches);
        return -1;
    }

    for (i = 0; i < matches.gl_pathc; i++) {
        fd = open_evdev_candidate(jack, matches.gl_pathv[i]);
        if (fd < 0)
            continue;

        *selected_path = pa_xstrdup(matches.gl_pathv[i]);
        globfree(&matches);
        return fd;
    }

    globfree(&matches);
    return -1;
}

static void
evdev_io_cb(pa_mainloop_api *api,
            pa_io_event *event,
            int fd,
            pa_io_event_flags_t events,
            void *userdata)
{
    pa_audio_manager_jack *jack = userdata;
    struct input_event input_events[32];
    ssize_t n;
    size_t count;
    size_t i;
    bool changed = false;
    bool headphone;
    bool microphone;

    pa_assert(api);
    pa_assert(event);
    pa_assert(jack);

    if (events & (PA_IO_EVENT_ERROR | PA_IO_EVENT_HANGUP)) {
        pa_log_warn("wired jack evdev monitor failed for %s", jack->path);
        api->io_enable(event, PA_IO_EVENT_NULL);
        jack_emit(jack, false, false, false);
        return;
    }

    for (;;) {
        n = read(fd, input_events, sizeof(input_events));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            pa_log_warn("failed to read wired jack evdev state from %s: %s",
                        jack->path,
                        strerror(errno));
            api->io_enable(event, PA_IO_EVENT_NULL);
            jack_emit(jack, false, false, false);
            return;
        }
        if (n == 0)
            break;

        count = (size_t)n / sizeof(struct input_event);
        for (i = 0; i < count; i++) {
            if (input_events[i].type == EV_SW)
                changed = true;
        }
    }

    if (!changed)
        return;

    if (!evdev_query_state(jack, &headphone, &microphone)) {
        pa_log_warn("failed to query wired jack switch state from %s: %s",
                    jack->path,
                    strerror(errno));
        api->io_enable(event, PA_IO_EVENT_NULL);
        jack_emit(jack, false, false, false);
        return;
    }

    jack_emit(jack, true, headphone, microphone);
}

static bool
start_evdev(pa_audio_manager_jack *jack, const char *requested)
{
    char *selected_path = NULL;
    bool headphone;
    bool microphone;

    jack->fd = find_evdev_fd(jack, requested, &selected_path);
    if (jack->fd < 0)
        return false;

    jack->backend = pa_xstrdup("evdev");
    jack->path = selected_path;

    if (!evdev_query_state(jack, &headphone, &microphone)) {
        pa_log_warn("failed to query initial wired jack state from %s: %s",
                    jack->path,
                    strerror(errno));
        return false;
    }

    jack->io_event = jack->mainloop->io_new(jack->mainloop,
                                            jack->fd,
                                            PA_IO_EVENT_INPUT,
                                            evdev_io_cb,
                                            jack);
    if (jack->io_event == NULL)
        return false;

    jack_emit(jack, true, headphone, microphone);
    return true;
}

static bool
start_sysfs(pa_audio_manager_jack *jack, const char *requested)
{
    bool headphone;
    bool microphone;

    jack->path = find_sysfs_state_path(requested);
    if (jack->path == NULL)
        return false;

    if (!sysfs_read_state(jack, &headphone, &microphone))
        return false;

    jack->fd = open_uevent_socket();
    if (jack->fd < 0) {
        pa_log_warn("failed to open wired jack uevent monitor: %s", strerror(errno));
        return false;
    }

    jack->backend = pa_xstrdup(strstr(jack->path, "/class/switch/") != NULL ?
                               "switch" : "extcon");
    jack->io_event = jack->mainloop->io_new(jack->mainloop,
                                            jack->fd,
                                            PA_IO_EVENT_INPUT,
                                            sysfs_uevent_cb,
                                            jack);
    if (jack->io_event == NULL)
        return false;

    jack_emit(jack, true, headphone, microphone);
    return true;
}

static void
reset_backend(pa_audio_manager_jack *jack)
{
    if (jack->io_event != NULL) {
        jack->mainloop->io_free(jack->io_event);
        jack->io_event = NULL;
    }

    if (jack->fd >= 0) {
        close(jack->fd);
        jack->fd = -1;
    }

    pa_xfree(jack->backend);
    jack->backend = NULL;
    pa_xfree(jack->path);
    jack->path = NULL;

    jack->known = false;
    jack->headphone = false;
    jack->microphone = false;
}

pa_audio_manager_jack *
pa_audio_manager_jack_new(pa_core *core,
                          const char *detection,
                          const char *evdev_path,
                          const char *extcon_path,
                          pa_audio_manager_jack_changed_cb changed_cb,
                          void *userdata)
{
    pa_audio_manager_jack *jack;

    pa_assert(core);

    if (detection == NULL)
        detection = "auto";
    if (pa_streq(detection, "none"))
        return NULL;

    if (!pa_streq(detection, "auto") &&
        !pa_streq(detection, "evdev") &&
        !pa_streq(detection, "extcon")) {
        pa_log_error("unknown jack_detection '%s'", detection);
        return NULL;
    }

    jack = pa_xnew0(pa_audio_manager_jack, 1);
    jack->core = core;
    jack->mainloop = core->mainloop;
    jack->fd = -1;
    jack->changed_cb = changed_cb;
    jack->userdata = userdata;

    if ((pa_streq(detection, "auto") || pa_streq(detection, "evdev")) &&
        start_evdev(jack, evdev_path != NULL ? evdev_path : "auto")) {
        pa_log_info("wired jack detection using evdev %s", jack->path);
        return jack;
    }

    reset_backend(jack);

    if ((pa_streq(detection, "auto") || pa_streq(detection, "extcon")) &&
        start_sysfs(jack, extcon_path != NULL ? extcon_path : "auto")) {
        pa_log_info("wired jack detection using %s %s", jack->backend, jack->path);
        return jack;
    }

    reset_backend(jack);
    pa_xfree(jack);

    pa_log_warn("no wired jack detector found");
    return NULL;
}

void
pa_audio_manager_jack_free(pa_audio_manager_jack *jack)
{
    if (jack == NULL)
        return;

    reset_backend(jack);
    pa_xfree(jack);
}
