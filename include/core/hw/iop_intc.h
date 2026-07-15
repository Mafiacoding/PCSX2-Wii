/*
 * iop_intc.h - IOP interrupt controller (I_STAT/I_MASK/I_CTRL)
 *
 * These are PS1-legacy registers the PS2 IOP retains (real PS2 IOP
 * hardware address range 0x1F801070-0x1F80107B): the pending-interrupt
 * status register, the per-source mask register, and a one-shot
 * "master interrupt enable" control register. Real IOP BIOS/module
 * code polls I_STAT (masked by I_MASK) to find and service pending
 * interrupts - this is foundational plumbing needed before any real
 * IOP BIOS code (as opposed to the hand-written test programs used
 * so far) can do anything meaningful.
 *
 * Semantics ported from real PCSX2 source
 * (pcsx2/ps2/Iop/IopHwWrite.cpp, IopHwRead.cpp - the HW_ISTAT/
 * HW_IMASK/HW_ICTRL cases), not reinvented:
 *
 *   0x1F801070 I_STAT - pending interrupt bits (one per IRQ source).
 *                        Write ANDs the written value into the
 *                        register (`psxHu(addr) &= val`) - i.e. to
 *                        clear/acknowledge bit N, write a value with
 *                        bit N = 0 (all other bits = 1). This is
 *                        "write 0 to clear", the opposite of the
 *                        write-1-to-clear pattern used by GS_CSR/
 *                        SIF SMFLAG elsewhere in this project - easy
 *                        to get backwards, so take care here.
 *                        Plain read.
 *   0x1F801074 I_MASK - per-source interrupt enable mask. Plain
 *                        read/write on real hardware/PCSX2 (the
 *                        write handler is a direct assignment, then
 *                        PCSX2 re-evaluates whether to raise a CPU
 *                        interrupt - that re-evaluation step,
 *                        `iopTestIntc()`, is NOT modeled here since
 *                        this project doesn't yet raise IOP CPU
 *                        interrupts/exceptions at all).
 *   0x1F801078 I_CTRL - a one-shot master-interrupt-enable latch.
 *                        Plain write. READ is the interesting part:
 *                        real hardware/PCSX2 returns the current
 *                        value AND THEN CLEARS the register to 0 as
 *                        a side effect of the read
 *                        (`ret = psxHu32(HW_ICTRL); psxHu32(HW_ICTRL)
 *                        = 0;`) - reading this register consumes it.
 *
 * UPDATE (task #115): raising a real IOP CPU interrupt/exception when
 * I_STAT & I_MASK becomes nonzero IS now modeled - see iop_core.c's
 * iop_check_hw_interrupt(). UPDATE (tasks #215/#216): two real IRQ
 * source numbers are now wired up and actually raised by hardware
 * models - the IOP counter/timer overflow/target IRQs (bits 4-6,
 * 14-16, see iop_timers.c's iop_timers_tick()) and the real IOP
 * VBLANK_IN/VBLANK_OUT lines (bits 0 and 11, see iop_core.c's
 * iop_check_vblank(), cited from PCSX2's IopCounters.cpp
 * iopIntcIrq(0)/iopIntcIrq(11) calls). Real PS2 IOP has ~20 total IRQ
 * sources; the remainder (DMA completion, CDVD, SIO, SPU, PIO, etc.)
 * are NOT modeled yet. iop_intc_raise() below lets other hardware
 * models set a specific I_STAT bit, for when those are eventually
 * wired up.
 *
 * UPDATE (Round 112, task #172/#267/#268, following the 152nd
 * finding's honestly-documented gap): real PS2 IOP hardware ALSO has
 * a second, separate irq range (0x20-0x3F / 32-63, per ps2sdk's cited
 * `enum iop_irq_list` in intrman.h) that is NOT part of the 32-bit
 * I_STAT/I_MASK MMIO registers modeled above at all - it is dispatched
 * entirely by INTRMAN's own internal software bookkeeping (real
 * per-DMA-channel "soft" IRQs like IOP_IRQ_DMA_SIF0=0x2A/
 * IOP_IRQ_DMA_SIF1=0x2B, plus two pure software interrupts
 * IOP_IRQ_SW1=0x3E/IOP_IRQ_SW2=0x3F). `istat_hi`/`imask_hi` below
 * model exactly that second, software-only range (bit N here means
 * irq 32+N) - deliberately NOT exposed via iop_intc_mmio_read32/
 * write32, because real hardware doesn't expose it as memory-mapped
 * I/O either. `iop_intc_raise_soft()` is the raise-side hook for this
 * range, mirroring `iop_intc_raise()`'s own precedent: exposed now,
 * before any real hardware model (DMA completion) calls it, exactly
 * per this project's established "build the mechanism, gate
 * activation on a real condition" convention (no real DMA-completion
 * hardware model exists in this project yet, so nothing raises into
 * this range today - this round only fixes the previously-honest
 * gap that `iop_check_hw_interrupt()` could never even CONSIDER
 * dispatching to irq >= 32, regardless of whether anything eventually
 * raises one).
 */
#ifndef PCSX2_WII_IOP_INTC_H
#define PCSX2_WII_IOP_INTC_H

#include <stdint.h>

typedef struct {
    uint32_t istat;
    uint32_t imask;
    uint32_t ictrl;

    /* Round 112: real irq 32-63 "soft" range - see the header comment
     * above. Bit N here = real irq (32+N). NOT memory-mapped; real
     * hardware doesn't expose this range via I_STAT/I_MASK MMIO
     * either - it's purely internal to INTRMAN's own software
     * dispatch, which is exactly what this mirrors. */
    uint32_t istat_hi;
    uint32_t imask_hi;
} iop_intc_state_t;

void iop_intc_init(void);

/* Returns 1 and fills *out if addr is a modeled INTC register, 0
 * otherwise - same convention as sif_iop_mmio_read32/write32. */
int iop_intc_mmio_read32(uint32_t addr, uint32_t *out);
int iop_intc_mmio_write32(uint32_t addr, uint32_t value);

/* Sets bit `irq` (0-31) in I_STAT, as real IOP peripherals would when
 * they have a pending interrupt to report. Not yet called by
 * anything in this project (no peripheral models raise real
 * interrupts yet) - exposed now so future hardware models have a
 * ready hook. */
void iop_intc_raise(int irq);

/* Round 112: sets bit (irq-32) in the real 32-63 "soft" irq range's
 * istat_hi (irq must be 32-63) - see the header comment and
 * iop_intc_state_t's istat_hi field comment above for the full real-
 * hardware citation. Not yet called by anything in this project (no
 * DMA-completion hardware model raises real per-channel soft irqs
 * yet, e.g. IOP_IRQ_DMA_SIF0/SIF1) - exposed now, mirroring
 * iop_intc_raise()'s own precedent, so a future DMA-completion model
 * has a ready hook, and so iop_check_hw_interrupt()'s dispatch-side
 * selection logic (fixed this round) has a real range to scan. */
void iop_intc_raise_soft(int irq);

iop_intc_state_t *iop_intc_get_state(void);

#endif
