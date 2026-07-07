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
 *
 * ONE EXCEPTION to "every call gets a generic default": C(07h),
 * InstallExceptionHandlers - see the real-BIOS-testing investigation
 * in docs/STATUS.md. Real PS1/PS2 hardware/BIOS documents this
 * function's job precisely (community reference: psx-spx,
 * https://psx-spx.consoledev.net/kernelbios/, "BIOS Function
 * Summary" and "BIOS RAM Map" sections): it writes a small, fixed,
 * 4-instruction trampoline (LUI $k0,0 / ADDIU $k0,$k0,<addr> /
 * JR $k0 / NOP) to the CPU's hardware-mandated general exception
 * vector at RAM address 0x80 (mirrored to address 0, per that same
 * document's "Garbage Area" note). Confirmed via real-BIOS testing
 * that this project's generic default-return stub was exactly what
 * was leaving that vector empty, causing execution to eventually
 * wander into unrelated reserved memory and halt. Rather than
 * hardcoding the well-known template bytes from the public
 * documentation (which vary slightly by BIOS revision - the jump
 * target immediate differs), `iop_hle_bios_try_handle()` locates the
 * REAL 16-byte template already present in the loaded BIOS ROM image
 * itself (a distinctive, unambiguous byte signature - see
 * `find_exception_handler_template()`) and copies those exact,
 * version-correct bytes verbatim. If no such signature is found (a
 * different BIOS revision structures this differently, or no BIOS
 * ROM is loaded), this falls back to the same generic default as
 * every other function - no fabricated bytes are ever written.
 *
 * FURTHER EXCEPTIONS added this round (task: "IOP HLE stubs for
 * BIOS-boot-path modules"): the same psx-spx reference
 * (https://psx-spx.consoledev.net/kernelbios/, "A-Functions"/
 * "B-Functions" tables) also documents a set of A0-table calls that
 * are pure, fully-specified computation on IOP RAM/registers only -
 * no dependency on any real BIOS internal kernel structure (unlike
 * e.g. module loading, thread control blocks, or device drivers,
 * which this project still has no verified reference for - see
 * iop_hle_modules.h and docs/STATUS.md's "round 14" finding). These
 * are implemented for real below: ABS/LABS(0x0E/0x0F), STRCAT/
 * STRNCAT(0x15/0x16), STRCMP/STRNCMP(0x17/0x18), STRCPY/STRNCPY
 * (0x19/0x1A), STRLEN(0x1B), BCOPY/BZERO(0x27/0x28), MEMCPY/MEMSET/
 * MEMMOVE(0x2A/0x2B/0x2C), INITHEAP(0x39, bookkeeping only - no real
 * allocator, same "scaffold, not a port" caveat as
 * iop_hle_modules.c), FLUSHCACHE(0x44, a correct no-op - this project
 * has no cache model to flush), and EXIT/_EXIT(0x06/0x3A, which halts
 * the core with an honest, descriptive reason instead of silently
 * returning 0 and letting the caller proceed past a call that real
 * hardware never returns from).
 *
 * MEMMOVE is a deliberate exception to "implement it correctly":
 * psx-spx annotates A(2Ch) memmove as ";Bugged" on real hardware
 * (i.e. the real BIOS's memmove does NOT handle overlapping regions
 * safely, unlike a standard C memmove). This is implemented here as
 * a plain forward byte-copy (matching memcpy, NOT overlap-safe) to
 * match that documented real (buggy) behavior, rather than silently
 * "fixing" it to modern libc semantics no real IOP BIOS ever had.
 *
 * Still NOT implemented (same rationale as the rest of this file):
 * anything touching files/devices (open/read/write/close/ioctl),
 * heap allocation (malloc/free/calloc/realloc - InitHeap is recorded
 * but nothing actually allocates from it), threads/events (SetConf/
 * EnqueueCdIntr/etc.), or any CD-ROM/memory-card function - all of
 * these depend on internal BIOS kernel structures (TCBs, EvCBs, file
 * control blocks) this project has never modeled and has no verified
 * layout for.
 */
#ifndef PCSX2_WII_IOP_HLE_BIOS_H
#define PCSX2_WII_IOP_HLE_BIOS_H

#include <stdint.h>
#include "core/iop/iop_core.h"

#define IOP_HLE_TABLE_A0 0x000000A0u
#define IOP_HLE_TABLE_B0 0x000000B0u
#define IOP_HLE_TABLE_C0 0x000000C0u

/* A0-table function numbers this round implements for real - see the
 * header comment above and psx-spx's "A-Functions" table. */
#define IOP_HLE_A0_EXIT       0x06u
#define IOP_HLE_A0_ABS        0x0Eu
#define IOP_HLE_A0_LABS       0x0Fu
#define IOP_HLE_A0_STRCAT     0x15u
#define IOP_HLE_A0_STRNCAT    0x16u
#define IOP_HLE_A0_STRCMP     0x17u
#define IOP_HLE_A0_STRNCMP    0x18u
#define IOP_HLE_A0_STRCPY     0x19u
#define IOP_HLE_A0_STRNCPY    0x1Au
#define IOP_HLE_A0_STRLEN     0x1Bu
#define IOP_HLE_A0_BCOPY      0x27u
#define IOP_HLE_A0_BZERO      0x28u
#define IOP_HLE_A0_MEMCPY     0x2Au
#define IOP_HLE_A0_MEMSET     0x2Bu
#define IOP_HLE_A0_MEMMOVE    0x2Cu
#define IOP_HLE_A0_INITHEAP   0x39u
#define IOP_HLE_A0__EXIT      0x3Au
#define IOP_HLE_A0_FLUSHCACHE 0x44u

typedef struct {
    uint64_t calls_seen;
    uint32_t last_table;     /* one of the IOP_HLE_TABLE_* constants */
    uint32_t last_function;  /* value of $t1 at the time of the last call */
    char     last_call_desc[64];

    /* InstallExceptionHandlers (C0h/0x07) real-behavior tracking -
     * see the header comment above. */
    uint8_t  exception_handler_template_found; /* 1 if the 16-byte
                                                 * signature was
                                                 * located in the
                                                 * loaded BIOS ROM */
    uint8_t  exception_handler_installed;      /* 1 once the template
                                                 * has actually been
                                                 * written to RAM */

    /* Real-behavior A0-table calls added this round - see the header
     * comment above. */
    uint64_t known_calls_handled; /* count of calls that got real
                                    * behavior (not the generic
                                    * default) - includes C(07h) and
                                    * every IOP_HLE_A0_* function
                                    * below */
    uint32_t heap_addr, heap_size; /* InitHeap(0x39) bookkeeping only -
                                     * no real allocator, see header */
    uint8_t  heap_initialized;
    uint8_t  exited;               /* 1 once EXIT/_EXIT has halted the core */
    uint32_t exit_code;
} iop_hle_bios_state_t;

void iop_hle_bios_init(void);

/* Returns 1 if `pc` is one of the three trap addresses (in which case
 * the call has been fully "handled" - st->pc/st->next_pc have already
 * been redirected to the return address, and the caller should NOT
 * also fetch/decode a real instruction for this step), 0 otherwise. */
int iop_hle_bios_try_handle(iop_state_t *st, uint32_t pc);

iop_hle_bios_state_t *iop_hle_bios_get_state(void);

#endif
