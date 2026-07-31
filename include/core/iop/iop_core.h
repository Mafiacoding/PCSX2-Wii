#ifndef PCSX2WII_IOP_CORE_H
#define PCSX2WII_IOP_CORE_H

#include <stdint.h>
#include "core/bios_loader.h"

/*
 * IOP (I/O Processor) CPU state - a separate R3000A (MIPS I) core from
 * the EE. Much simpler than the EE: 32-bit registers only, no MMI, no
 * 128-bit anything, no vector units. Real hardware: 2MB RAM, boots
 * from the same physical BIOS ROM as the EE (a real PS2 board has one
 * BIOS chip both CPUs can see), running IOP-side init/module-loading
 * code before the EE hands off control via SIF.
 *
 * Semantics ported from PCSX2's pcsx2/R3000AOpcodeTables.cpp
 * (GPL-3.0) - see /COPYING.GPLv3.
 *
 * NOTE: this is a standalone skeleton at this point - not yet wired
 * into main.c/ee_core.c, no SIF, no IOP hardware register model. See
 * docs/ROADMAP.md section 2.
 */
typedef struct {
    uint32_t gpr[32];
    uint32_t pc;
    uint32_t next_pc;
    uint32_t hi, lo;
    uint32_t cop0[32];

    uint8_t *ram;       /* 2MB IOP RAM */
    uint32_t ram_size;

    const bios_image_t *bios; /* shared with the EE - same physical ROM */

    uint64_t instructions_executed;
    uint8_t  halted;
    char     halt_reason[128];

    /* Task #179 continued: real IOP hardware never halts - after its
     * boot-time module list finishes running, the real IOP kernel's
     * thread scheduler always has at least an idle thread to fall
     * back to, and stays interrupt-responsive forever (any driver
     * that installed a real interrupt handler during its earlier
     * init keeps reacting to hardware events indefinitely, even
     * though this project doesn't model persistent threads). Set
     * instead of `halted` by iop_module_loader.c's final "all
     * modules run to completion" site (previously the last remaining
     * unconditional halt() in that file). While idle, iop_core_step()
     * does NOT fetch/decode/execute anything at the (synthetic,
     * likely-zeroed) trampoline address - inventing specific "idle
     * loop" instruction bytes real hardware might contain would be
     * fabrication this project's discipline forbids - it only keeps
     * re-checking iop_check_hw_interrupt() every call, exactly like
     * the real per-step check every other instruction already gets.
     * If a hardware interrupt becomes pending (any of the sources
     * this project already models - timers, DMA, etc.), that check
     * vectors pc/next_pc into the real exception vector as normal,
     * this flag is cleared, and the interpreter resumes real
     * fetch/decode/execute from there next call - running whatever
     * real, RAM-resident handler code the modules installed before
     * their entry points returned. See docs/STATUS.md's 54th
     * finding. */
    uint8_t  idle;

    /* Round 29 continued (task #156): set to 1 by every real
     * exception-entry site (hardware interrupt, SYSCALL, TGE - see
     * iop_core.c's own cop0[14]/EPC-writing sites), cleared to 0 by
     * RFE. Distinguishes "Cause.ExcCode still reads 8 because this
     * exception hasn't been handled yet" from "Cause.ExcCode still
     * reads 8 merely because RFE (which only restores Status, not
     * Cause - see RFE's own header comment) already handled and
     * returned from an EARLIER syscall, and nothing has overwritten
     * the now-stale Cause register since". Needed because BREAK's
     * own "unimplemented syscall fallback" heuristic (task #149, the
     * 29th change) checks Cause.ExcCode==8 alone, which fires on
     * stale Cause values left over from an already-RFE-handled
     * exception - a real, reproducible hang found while regression-
     * testing task #155 (see docs/STATUS.md). */
    uint8_t  exception_pending;

    /* Round 77 (117th finding, task #221/#245): scratch state for the
     * device-table embedded-ELF intercept (see iop_core.c's JAL/JALR
     * case comments). 0 = no pending image. Only ever set/used when
     * cop0[15]==0x1f (dead field at the default PRId=0). */
    uint32_t devtable_pending_image;
} iop_state_t;

int  iop_core_init(const bios_image_t *bios);
void iop_core_run(void);

/* Single-step entry point, for the interleaved EE/IOP scheduler
 * in core/system.h. See its definition in iop_core.c for details. */
int iop_core_step(void);
void iop_core_shutdown(void);
iop_state_t *iop_core_get_state(void);

uint8_t  iop_mem_read8(iop_state_t *st, uint32_t addr);
uint16_t iop_mem_read16(iop_state_t *st, uint32_t addr);
uint32_t iop_mem_read32(iop_state_t *st, uint32_t addr);
void     iop_mem_write8(iop_state_t *st, uint32_t addr, uint8_t val);
void     iop_mem_write16(iop_state_t *st, uint32_t addr, uint16_t val);
void     iop_mem_write32(iop_state_t *st, uint32_t addr, uint32_t val);

#endif
