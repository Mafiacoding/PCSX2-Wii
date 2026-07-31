/*
 * iop_hle_modules.c - IOP module registry. See iop_hle_modules.h for
 * scope (a project-owned bookkeeping scaffold, not a port of real
 * IRX module parsing or PCSX2's SIF-based module loading protocol).
 */
#include "core/hw/iop_hle_modules.h"
#include <string.h>

static iop_hle_module_registry_t g_registry;

void iop_hle_modules_init(void)
{
    memset(&g_registry, 0, sizeof(g_registry));
}

iop_hle_module_registry_t *iop_hle_modules_get_state(void) { return &g_registry; }

static iop_hle_module_t *find_module(const char *name)
{
    for (int i = 0; i < g_registry.count; i++) {
        if (strncmp(g_registry.modules[i].name, name, IOP_HLE_MODULE_NAME_MAX) == 0)
            return &g_registry.modules[i];
    }
    return NULL;
}

int iop_hle_module_register(const char *name, uint32_t version)
{
    if (!name || !name[0])
        return 0;

    iop_hle_module_t *existing = find_module(name);
    if (existing) {
        existing->version = version;
        return 1;
    }

    if (g_registry.count >= IOP_HLE_MAX_MODULES)
        return 0;

    iop_hle_module_t *slot = &g_registry.modules[g_registry.count];
    strncpy(slot->name, name, IOP_HLE_MODULE_NAME_MAX - 1);
    slot->name[IOP_HLE_MODULE_NAME_MAX - 1] = '\0';
    slot->version = version;
    g_registry.count++;
    return 1;
}

int iop_hle_module_is_registered(const char *name, uint32_t *version_out)
{
    if (!name)
        return 0;

    iop_hle_module_t *m = find_module(name);
    if (!m)
        return 0;

    if (version_out)
        *version_out = m->version;
    return 1;
}
