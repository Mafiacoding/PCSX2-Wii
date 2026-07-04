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
