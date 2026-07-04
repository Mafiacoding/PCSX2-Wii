/*
 * iop_hle_modules.h - IOP module registry ("module loading logic")
 *
 * SCOPE, read before extending or trusting this: this is a project-
 * owned, deliberately simplified scaffold, NOT a port of anything in
 * PCSX2. Real PS2 IOP module loading involves parsing an IRX file (an
 * ELF-like binary format with its own import/export table
 * conventions), and PCSX2's actual mechanism for intercepting it
 * (pcsx2/IopBios.cpp's `irxImportHLE`/`irxImportExec`, plus the SIF
 * RPC protocol real games use to ask the IOP to load a module,
 * `sceSifLoadModule` and friends - see `Sifcmd.h`) depends on a real,
 * running IOP BIOS having already built its own internal loadcore
 * data structures. None of that exists in this project.
 *
 * What this file provides instead: a plain in-memory registry that
 * records "a module with this name/version is considered loaded",
 * with no relationship to any real binary format, real memory
 * layout, or real BIOS structure. It exists as scaffolding for
 * whatever comes next (e.g. once/if a verified real function-number
 * reference for the IOP's LoadModule-style BIOS syscall is found,
 * core/hw/iop_hle_bios.c's trap dispatcher could call
 * iop_hle_module_register() from there - not done yet, see its
 * header comment for why). Until then, this is only reachable via
 * its own direct C API, not from any IOP-executed code path.
 */
#ifndef PCSX2_WII_IOP_HLE_MODULES_H
#define PCSX2_WII_IOP_HLE_MODULES_H

#include <stdint.h>

#define IOP_HLE_MAX_MODULES 32
#define IOP_HLE_MODULE_NAME_MAX 32

typedef struct {
    char     name[IOP_HLE_MODULE_NAME_MAX];
    uint32_t version;
} iop_hle_module_t;

typedef struct {
    iop_hle_module_t modules[IOP_HLE_MAX_MODULES];
    int count;
} iop_hle_module_registry_t;

void iop_hle_modules_init(void);

/* Records `name` (truncated to IOP_HLE_MODULE_NAME_MAX-1 chars if
 * longer) as loaded with the given version. If a module with the
 * same name is already registered, its version is updated in place
 * rather than adding a duplicate entry. Returns 1 on success, 0 if
 * name is NULL/empty or the registry is full. */
int iop_hle_module_register(const char *name, uint32_t version);

/* Returns 1 if a module with this exact name is registered, 0
 * otherwise. If found and version_out is non-NULL, fills it with the
 * registered version. */
int iop_hle_module_is_registered(const char *name, uint32_t *version_out);

iop_hle_module_registry_t *iop_hle_modules_get_state(void);

#endif
