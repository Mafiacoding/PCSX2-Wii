#ifndef PCSX2WII_EE_CORE_H
#define PCSX2WII_EE_CORE_H

#include <stdint.h>
#include "core/bios_loader.h"

/*
 * Emotion Engine (R5900) CPU state.
 *
 * Real hardware note: the EE registers are 128-bit (64-bit GPRs with an
 * extra "upper" 64 bits used by MMI/multimedia instructions). This
 * skeleton only models the low 64 bits - the MMI instruction set
 * (parallel SIMD-ish ops PS2 games/BIOS routines rely on constantly)
 * is NOT implemented. This alone means real BIOS boot code will hit
 * unimplemented-opcode halts almost immediately. See docs/STATUS.md.
 *
 * Opcode semantics in ee_core.c (sign-extension rules, HI/LO handling,
 * etc.) are ported from PCSX2's own interpreter reference
 * (pcsx2/R5900OpcodeImpl.cpp, GPL-3.0) rather than reinvented, so this
 * project is GPL-3.0 licensed as a whole - see COPYING.GPLv3.
 */
typedef struct {
    uint64_t gpr[32];
    uint32_t pc;
    uint32_t next_pc;       /* branch delay slot handling */
    uint64_t hi, lo;
    uint32_t cop0[32];      /* status/cause/EPC subset only */
    uint8_t  branch_pending;

    uint8_t *ram;           /* 32MB emulated EE RAM */
    uint32_t ram_size;

    const bios_image_t *bios;

    uint64_t instructions_executed;
    uint8_t  halted;
    char     halt_reason[128];
} ee_state_t;

int  ee_core_init(const bios_image_t *bios);
void ee_core_run(const bios_image_t *bios);
void ee_core_shutdown(void);

/* Exposed for the recompiler PoC (source/core/recompiler) to share
 * register state layout / memory access helpers. */
ee_state_t *ee_core_get_state(void);

uint8_t  ee_mem_read8(ee_state_t *st, uint32_t addr);
uint16_t ee_mem_read16(ee_state_t *st, uint32_t addr);
uint32_t ee_mem_read32(ee_state_t *st, uint32_t addr);
uint64_t ee_mem_read64(ee_state_t *st, uint32_t addr);
void     ee_mem_write8(ee_state_t *st, uint32_t addr, uint8_t val);
void     ee_mem_write16(ee_state_t *st, uint32_t addr, uint16_t val);
void     ee_mem_write32(ee_state_t *st, uint32_t addr, uint32_t val);
void     ee_mem_write64(ee_state_t *st, uint32_t addr, uint64_t val);

#endif
