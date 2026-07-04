/*
 * iop_hle_bios.h - IOP BIOS syscall trap (the "HLE BIOS replacement")
 *
 * IMPORTANT SCOPE NOTE, read this before extending or trusting this
 * file: this is NOT a port of PCSX2's actual IOP HLE. PCSX2's real
 * approach (pcsx2/IopBios.cpp, ~1500 lines) is far more involved than
 * what's implemented here - it lets the REAL IOP BIOS ROM execute far
 * enough to build its own internal data structures (loadcore's module
 * list, thread manager state, etc. - see IopBios.cpp's `loadcore::
 * GetModList`/`GetThreadList`, which literally parse the running
 * BIOS's in-memory structures), and only intercepts specific IRX
 * library import calls once those structures exist
 * (`irxImportTableAddr`/`irxImportHLE`/`irxImportExec`). That
 * approach fundamentally depends on having a real, working PS2 IOP
 * BIOS ROM to boot first - this project doesn't ship one (PS2 BIOS
 * ROMs are copyrighted Sony firmware, see docs/STATUS.md), and even
 * with one, faithfully replicating version-specific internal BIOS
 * structure layouts is out of reach without a verified reference this
 * project doesn't have.
 *
 * What IS implemented here instead is the older, much simpler, and
 * extremely well-established PS1-style BIOS call convention that the
 * PS2 IOP retains for backward compatibility: three fixed "trap"
 * addresses right at the bottom of the address space -
 *
 *   0x000000A0 - the "A0 table" (typically low-level kernel/memory
 *                functions on real PS1/PS2 hardware)
 *   0x000000B0 - the "B0 table" (typically higher-level kernel calls)
 *   0x000000C0 - the "C0 table" (typically exception/interrupt setup)
 *
 * Calling convention: the caller loads a function number into
 * register $t1 (r9) and jumps to one of these three addresses with
 * the return address in $ra (r31) - i.e. effectively `JAL 0xA0` (or
 * 0xB0/0xC0). Real hardware/BIOS ROM code AT those addresses reads
 * $t1 and dispatches through an internal jump table, eventually
 * returning via `JR $ra`. This three-vector mechanism is foundational
 * PS1/PS2 architecture (used by essentially every open-source PS1/PS2
 * emulator for BIOS HLE), not something invented for this project -
 * but the SPECIFIC function-number-to-behavior mapping for each
 * table (e.g. "function 0x3C on the A0 table does X") is NOT
 * something this project has a verified, citable reference for, and
 * is deliberately NOT guessed at here.
 *
 * What this file actually does: when the IOP core's PC reaches one of
 * the three trap addresses, iop_step() (in iop_core.c) calls
 * iop_hle_bios_try_handle() INSTEAD OF fetching/decoding a real
 * instruction there. The call is logged (table + function number),
 * $v0 is set to a generic default (0), and execution is redirected
 * straight to the return address in $ra - simulating "the call
 * happened and returned" without executing real BIOS-ROM bytes that
 * likely don't exist/aren't meaningful in this project's incomplete
 * BIOS images anyway. This turns what would otherwise be either an
 * infinite NOP loop (if the region reads as zero bytes, which decode
 * as valid MIPS NOPs) or a halt-on-garbage-opcode into forward
 * progress for whatever boot code issued the call - a real,
 * meaningful improvement even without knowing what any specific
 * function number "really" does, matching how PCSX2 itself falls
 * back to "log it, return a default" for any IRX import it doesn't
 * have a specific C++ implementation for (see `irxImportExec`'s
 * fallback path).
 */
#ifndef PCSX2_WII_IOP_HLE_BIOS_H
#define PCSX2_WII_IOP_HLE_BIOS_H

#include <stdint.h>
#include "core/iop/iop_core.h"

#define IOP_HLE_TABLE_A0 0x000000A0u
#define IOP_HLE_TABLE_B0 0x000000B0u
#define IOP_HLE_TABLE_C0 0x000000C0u

typedef struct {
    uint64_t calls_seen;
    uint32_t last_table;     /* one of the IOP_HLE_TABLE_* constants */
    uint32_t last_function;  /* value of $t1 at the time of the last call */
    char     last_call_desc[64];
} iop_hle_bios_state_t;

void iop_hle_bios_init(void);

/* Returns 1 if `pc` is one of the three trap addresses (in which case
 * the call has been fully "handled" - st->pc/st->next_pc have already
 * been redirected to the return address, and the caller should NOT
 * also fetch/decode a real instruction for this step), 0 otherwise. */
int iop_hle_bios_try_handle(iop_state_t *st, uint32_t pc);

iop_hle_bios_state_t *iop_hle_bios_get_state(void);

#endif
