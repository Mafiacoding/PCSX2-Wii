#ifndef PCSX2WII_EE_INTC_H
#define PCSX2WII_EE_INTC_H

#include <stdint.h>

/*
 * ee_intc.h - EE interrupt controller (INTC_STAT/INTC_MASK)
 *
 * Real EE hardware memory map (PCSX2's pcsx2/Hw.h EERegisterAddresses):
 *   INTC_STAT = 0x1000F000
 *   INTC_MASK = 0x1000F010
 *
 * Task #176 (splash-screen blocker investigation): this project had
 * NO external-interrupt-source model for the EE at all before this -
 * only Cause.IP7 (the internal COP0 Timer/Compare interrupt, see
 * ee_check_timer_interrupt() in ee_core.c) was ever raised. Real
 * hardware routes ten external sources (GS, SBUS, VBLANK start/end,
 * VIF0/1, VU0/1, IPU, Timers 0-3, SFIFO, VU0 watchdog) through this
 * single INTC_STAT/MASK pair into Cause.IP2 - this file only models
 * the register pair itself; ee_core.c's ee_check_intc_interrupt()
 * does the Cause.IP2 raising.
 *
 * Semantics ported from real PCSX2 source (pcsx2/HwWrite.cpp's
 * mcase(INTC_STAT)/mcase(INTC_MASK) in _hwWrite32(), pcsx2/Hw.cpp's
 * intcInterrupt()), not reinvented:
 *
 *   INTC_STAT: read is a plain value. WRITE clears the bits that are
 *              SET in the written value - `psHu32(INTC_STAT) &= ~value`
 *              (write-1-to-clear/acknowledge, matching the polarity
 *              already used by GS_CSR/SIF SMFLAG elsewhere in this
 *              project - NOT the IOP's I_STAT "write-0-to-clear"
 *              polarity, which is the opposite and easy to confuse).
 *   INTC_MASK: read is a plain value. WRITE TOGGLES (XORs) the bits
 *              that are set in the (16-bit-truncated) written value -
 *              `psHu32(INTC_MASK) ^= (u16)value` - this is a real
 *              hardware quirk (also used by DMAC_STAT's upper/enable
 *              half, see dma.h), not "plain assignment" like most
 *              other mask registers in this project.
 *
 * An interrupt is pending (should raise Cause.IP2) whenever
 * (INTC_STAT & INTC_MASK) != 0 - see intcInterrupt() in Hw.cpp.
 */

typedef struct {
    uint32_t stat;
    uint32_t mask;
} ee_intc_state_t;

void ee_intc_init(void);
ee_intc_state_t *ee_intc_get_state(void);

/* Returns 1 and fills *out if addr is INTC_STAT/INTC_MASK, 0
 * otherwise - same convention as dma_mmio_read32/sif_mmio_read32. */
int ee_intc_mmio_read32(uint32_t addr, uint32_t *out);
int ee_intc_mmio_write32(uint32_t addr, uint32_t value);

/* Sets bit `irq` (0-31) in INTC_STAT, as a real EE peripheral would
 * when it has a pending interrupt to report. Not yet called by
 * anything (no peripheral in this project raises a real IP2 source
 * yet) - exposed for future hardware models (GS vblank, VIF, etc). */
void ee_intc_raise(int irq);

/* Returns 1 if (stat & mask) != 0 - i.e. a real IP2 interrupt is
 * currently pending and unmasked. ee_core.c's ee_check_intc_interrupt()
 * calls this every step, mirroring ee_check_timer_interrupt()'s
 * Cause.IP7 pattern for this new external line. */
int ee_intc_pending(void);

/* Round 716 (task #696-699): real per-cause IRQ-raise hit counter,
 * mirroring ee_timers_get_irq_count() (Round 715). Purely diagnostic -
 * does not affect emulated behavior. */
uint32_t ee_intc_get_raise_count(int irq);

#endif
