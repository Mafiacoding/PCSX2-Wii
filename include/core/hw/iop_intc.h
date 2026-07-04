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
 * NOT modeled: raising an actual IOP CPU interrupt/exception when
 * I_STAT & I_MASK becomes nonzero (iop_core.c has no interrupt/
 * exception handling of any kind yet - see docs/ROADMAP.md), and the
 * individual IRQ source numbers/assignments (VBLANK, DMA completion,
 * etc - real PS2 IOP has ~20 of these). iop_intc_raise() below lets
 * other hardware models set a specific I_STAT bit, for when those are
 * eventually wired up.
 */
#ifndef PCSX2_WII_IOP_INTC_H
#define PCSX2_WII_IOP_INTC_H

#include <stdint.h>

typedef struct {
    uint32_t istat;
    uint32_t imask;
    uint32_t ictrl;
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

iop_intc_state_t *iop_intc_get_state(void);

#endif
