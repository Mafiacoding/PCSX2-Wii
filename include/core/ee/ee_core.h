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

    /* Set to 1 by whichever instruction executes right before a
     * branch-delay-slot instruction (any taken-or-not regular branch/
     * jump, or a taken Branch Likely - see the BRANCH_TO() macro and
     * the manual sets on BEQ/BNE/BLEZ/BGTZ/BLTZ/BGEZ/BLTZAL/BGEZAL/
     * BC1F/BC1T in ee_core.c), consumed and cleared at the top of the
     * NEXT ee_step() call. That next instruction's "am I in a delay
     * slot" status is what determines the Cause.BD bit and EPC value
     * (EPC = branch pc, not delay-slot pc) if IT raises an exception -
     * see ee_raise_exception() in ee_core.c, ported from PCSX2's own
     * cpuException() in R5900.cpp. */
    uint8_t  branch_pending;

    /* Real R5900 TLB (48 entries), ported from PCSX2's COP0.cpp
     * "tlbs" struct. Written/read by TLBWI/TLBWR/TLBR/TLBP (see
     * ee_core.c). Address translation IS wired into ee_mem_ptr() (via
     * ee_tlb_translate()) for any KUSEG address (<0x80000000) - see
     * docs/STATUS.md's "round 6". A KUSEG TLB miss now raises a real
     * TLB Refill exception (see ee_raise_exception()/
     * ee_raise_tlb_exception()) instead of silently reading as zero,
     * ported from PCSX2's cpuException()/cpuTlbMiss() in R5900.cpp -
     * see docs/STATUS.md's exception-delivery section for the full
     * story. */
    struct {
        uint32_t page_mask;
        uint32_t entry_hi;
        uint32_t entry_lo0;
        uint32_t entry_lo1;
    } tlb[48];

    /* Transient, per-instruction scratch state used only by the
     * exception-raising path (ee_raise_exception() and callers in
     * ee_core.c). Set once at the top of each ee_step() call (before
     * the instruction fetch, which can itself fault) so that any
     * memory-access helper deep in that instruction's execution
     * (ee_mem_read32(), ee_mem_write8(), etc.) can raise a correctly
     * addressed exception (right EPC / Cause.BD) without every one of
     * those functions needing extra parameters threaded through. Not
     * meaningful outside of an in-progress ee_step() call. */
    uint32_t exc_this_pc;         /* address of the instruction currently executing */
    uint8_t  exc_in_delay_slot;   /* was exc_this_pc itself a branch-delay-slot instruction? */
    uint8_t  exc_raised_this_step;/* guards against double-faulting once per instruction -
                                    * e.g. SWL/SWR do a read then a write of the SAME address,
                                    * both of which would otherwise independently detect the
                                    * same TLB miss and each try to vector away, corrupting the
                                    * second call's Cause/EPC bookkeeping (see ee_raise_tlb_exception()) */
    uint8_t  mem_tlb_miss;        /* set by ee_mem_ptr() itself: 1 if its most recent NULL
                                    * return was specifically a KUSEG TLB miss (exception-
                                    * worthy), 0 for every other outcome (success, or a
                                    * kseg0/kseg1 address with no backing ROM/RAM, which is
                                    * architecturally NOT a TLB fault and still just reads-as-
                                    * zero/no-ops as before - see ee_mem_ptr()'s own comments) */

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

    /* COP2 (VU0 in "macro mode") - control-register file only. Added
     * round 12 to get past a real BIOS init sequence
     * (cfc2/ori/ctc2 read-modify-write on FBRST, control register 28)
     * that halted with "unimplemented primary opcode 0x12" once round
     * 11's fixes let boot progress far enough to reach it. This
     * project does NOT model VU0's actual vector datapath (VF/VI
     * register files, the vector arithmetic opcode family - ADD/SUB/
     * MUL/MAC pipelines etc.) - only the 32-bit control-register
     * transfer instructions (MFC2/CFC2/MTC2/CTC2) real BIOS/kernel
     * init code uses to read/write control state like FBRST. Real
     * FBRST semantics (bit 0x2 = VU0 reset, bit 0x200 = VU1 reset,
     * bits 0x1/0x100 = force-break - ported from PCSX2's own
     * VU0.cpp CTC2() handler) aren't modeled beyond plain storage,
     * since no VU0/VU1 execution state exists yet to actually reset -
     * an honest simplification, not a fabricated one (matches this
     * project's existing SIF CTRL register precedent: real documented
     * side effects noted, not modeled, when the dependent subsystem
     * doesn't exist yet). See docs/STATUS.md's "round 12" section. */
    uint32_t cop2_ctrl[32];

    uint8_t *ram;           /* 32MB emulated EE RAM */
    uint32_t ram_size;

    /* R5900 Scratchpad RAM (SPR): a real, dedicated 16KB on-chip
     * memory hardwired to the fixed virtual range 0x70000000-
     * 0x70003FFF (KUSEG). This is NOT ordinary TLB-mapped memory -
     * real hardware bypasses the TLB entirely for this fixed address
     * window (confirmed both by a live PCSX2 trace and PCSX2's own
     * source: pcsx2/Memory.cpp literally comments "0x70000000-
     * 0x70003fff scratch pad", pcsx2/MemoryTypes.h's
     * Ps2MemSize::Scratch = 16KB, and pcsx2/COP0.cpp's MapTLB()
     * special-cases isSPR() entries to route straight to a dedicated
     * Scratch buffer instead of normal PFN-based physical translation).
     * Round 7's real TLB implementation initially tried to translate
     * this range through the normal KUSEG TLB path like any other
     * address, which - since the BIOS's stack pointer lands in the
     * upper 8KB half of this window, past the one narrow TLB entry
     * that happened to be installed - produced a genuine, unresolvable
     * TLB Refill exception loop (see docs/STATUS.md's "round 8").
     * ee_mem_ptr() now intercepts this fixed range unconditionally,
     * before any TLB lookup, exactly matching real hardware. */
    uint8_t scratch[16 * 1024];

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
