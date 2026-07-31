/*
 * iop_intc.c - IOP interrupt controller register model. See
 * iop_intc.h for the exact per-register semantics and PCSX2 source
 * cross-reference.
 */
#include "core/hw/iop_intc.h"
#include <string.h>

#define IOP_INTC_ISTAT 0x1F801070u
#define IOP_INTC_IMASK 0x1F801074u
#define IOP_INTC_ICTRL 0x1F801078u

static iop_intc_state_t g_intc;

void iop_intc_init(void)
{
    memset(&g_intc, 0, sizeof(g_intc));
}

iop_intc_state_t *iop_intc_get_state(void) { return &g_intc; }

void iop_intc_raise(int irq)
{
    if (irq < 0 || irq > 31)
        return;
    g_intc.istat |= (1u << irq);
}

/* Round 112: raise-side hook for the real 32-63 "soft" irq range -
 * see iop_intc.h's iop_intc_state_t.istat_hi comment for the full
 * citation. Deliberately separate from iop_intc_raise() above (not
 * folded into a single 0-63 function) because these really are two
 * architecturally distinct mechanisms on real hardware - one is a
 * memory-mapped register, the other is purely internal to INTRMAN -
 * and keeping them as separate functions/fields makes that real
 * distinction visible in the code instead of hiding it behind a
 * uniform-looking API that isn't uniform on the actual hardware. */
void iop_intc_raise_soft(int irq)
{
    if (irq < 32 || irq > 63)
        return;
    g_intc.istat_hi |= (1u << (irq - 32));
}

int iop_intc_mmio_read32(uint32_t addr, uint32_t *out)
{
    switch (addr) {
        case IOP_INTC_ISTAT:
            *out = g_intc.istat;
            return 1;
        case IOP_INTC_IMASK:
            *out = g_intc.imask;
            return 1;
        case IOP_INTC_ICTRL:
            /* Real PCSX2 (IopHwRead.cpp): reading I_CTRL returns the
             * current value, then the register is cleared to 0 as a
             * side effect of the read - this is a one-shot latch, not
             * a plain register. Easy to miss since almost nothing
             * else in this project's register models has a read-side
             * effect (GS_CSR/SIF SMFLAG have write-side effects, this
             * is the first read-side one). */
            *out = g_intc.ictrl;
            g_intc.ictrl = 0;
            return 1;
        default:
            return 0;
    }
}

int iop_intc_mmio_write32(uint32_t addr, uint32_t value)
{
    switch (addr) {
        case IOP_INTC_ISTAT:
            /* Real PCSX2 (IopHwWrite.cpp): psxHu(addr) &= val -
             * "write 0 to clear/acknowledge a pending bit, write 1 to
             * leave it alone" - the OPPOSITE polarity from the
             * write-1-to-clear pattern used by GS_CSR and SIF's
             * SMFLAG elsewhere in this project. Do not "fix" this to
             * match those - it's genuinely different hardware. */
            g_intc.istat &= value;
            return 1;
        case IOP_INTC_IMASK:
            /* Plain assignment. Real PCSX2 also calls iopTestIntc()
             * here to re-evaluate whether a CPU interrupt should be
             * raised - not modeled, since iop_core.c has no
             * interrupt/exception handling yet (see docs/ROADMAP.md). */
            g_intc.imask = value;
            return 1;
        case IOP_INTC_ICTRL:
            /* Plain assignment on write (the read side is where the
             * one-shot-latch behavior lives - see
             * iop_intc_mmio_read32 above). */
            g_intc.ictrl = value;
            return 1;
        default:
            return 0;
    }
}
