#ifndef PCSX2WII_EE_CORE_H
#define PCSX2WII_EE_CORE_H

#include <stdint.h>
#include "core/bios_loader.h"

/*
 * Emotion Engine (R5900) CPU state.
 *
 * GPRs and HI/LO are modeled as full 128 bits (two 64-bit halves,
 * "ud0"/"ud1" matching PCSX2's own UD[0]/UD[1] naming) because the MMI
 * (multimedia/SIMD) instruction set operates on all 128 bits as packed
 * lanes, and PCSX2 itself models HI/LO as 128-bit registers too (used
 * by the "pipeline 1" MMI multiply/divide variants: MULT1/DIV1/etc).
 *
 * Opcode semantics in ee_core.c (sign-extension rules, lane packing
 * for MMI ops, HI/LO handling, etc.) are ported from PCSX2's own
 * interpreter reference (pcsx2/R5900OpcodeImpl.cpp and pcsx2/MMI.cpp,
 * GPL-3.0) rather than reinvented, so this project is GPL-3.0
 * licensed as a whole - see /COPYING.GPLv3.
 */
typedef struct {
    uint64_t ud0; /* low 64 bits  (what plain 64-bit MIPS ops read/write) */
    uint64_t ud1; /* high 64 bits (only touched by MMI ops) */
} ee_reg128_t;

typedef struct {
    ee_reg128_t gpr[32];
    uint32_t pc;
    uint32_t next_pc;       /* branch delay slot handling */
    ee_reg128_t hi, lo;
    uint32_t cop0[32];      /* status/cause/EPC/config subset only */
    uint8_t  branch_pending;

    /* Real R5900 TLB (48 entries), ported from PCSX2's COP0.cpp
     * "tlbs" struct. Written/read by TLBWI/TLBWR/TLBR/TLBP (see
     * ee_core.c). NOTE: this is storage + lookup logic only - address
     * translation itself is NOT wired into ee_mem_ptr() yet, so this
     * project still treats kuseg/kseg0/kseg1 as flat, TLB-free
     * mappings for actual memory accesses (see ee_mem_ptr()'s
     * "phys = addr & 0x1FFFFFFF" comment). Real boot-time TLBWI calls
     * now execute correctly (matching real hardware's register
     * semantics) instead of halting; whether this project ever needs
     * real translation depends on what further real-BIOS boot
     * progress reveals. */
    struct {
        uint32_t page_mask;
        uint32_t entry_hi;
        uint32_t entry_lo0;
        uint32_t entry_lo1;
    } tlb[48];

    /* COP1 (FPU) - single-precision only. Raw IEEE-754 bit patterns,
     * not C floats, so bit-level ops (ABS_S/MOV_S/NEG_S, and the
     * denormal/infinity handling PS2's FPU does that plain IEEE float
     * math doesn't) match real hardware exactly. Semantics ported
     * from PCSX2's pcsx2/FPU.cpp. */
    uint32_t fpr[32];
    uint32_t fcr31;
    uint32_t acc;   /* FPU accumulator - MADDA.S/MSUBA.S/MULA.S/ADDA.S/
                     * SUBA.S write it, MADD.S/MSUB.S read it. Same raw
                     * IEEE-754 bit-pattern representation as fpr[]. */

    uint8_t *ram;           /* 32MB emulated EE RAM */
    uint32_t ram_size;

    const bios_image_t *bios;

    uint64_t instructions_executed;
    uint8_t  halted;
    char     halt_reason[128];
} ee_state_t;

int  ee_core_init(const bios_image_t *bios);
void ee_core_run(const bios_image_t *bios);

/* Single-step entry point, for the interleaved EE/IOP scheduler
 * in core/system.h. See its definition in ee_core.c for details. */
int ee_core_step(void);
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
