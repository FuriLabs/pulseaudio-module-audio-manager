/*
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#define PA_MODULE_NAME module_audio_manager_source

#include "audio-manager-source.h"

#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/module.h>

PA_MODULE_AUTHOR("Bardia Moshiri");
PA_MODULE_DESCRIPTION("Capture source");
PA_MODULE_VERSION(PULSE_MODULE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE("");

int
pa__get_n_used(pa_module *module)
{
    pa_source *source;

    pa_assert(module);

    source = module->userdata;
    return source != NULL ? pa_source_linked_by(source) : 0;
}

void
pa__done(pa_module *module)
{
    pa_source *source;

    pa_assert(module);

    if ((source = module->userdata) != NULL) {
        module->userdata = NULL;
        pa_audio_manager_source_free(source);
    }
}

int
pa__init(pa_module *module)
{
    pa_assert(module);

    if (module->argument != NULL && module->argument[0] != '\0') {
        pa_log_error("module-audio-manager-source does not accept module arguments");
        return -1;
    }

    module->userdata = pa_audio_manager_source_new(module, __FILE__);

    if (module->userdata == NULL) {
        pa__done(module);
        return -1;
    }

    return 0;
}
