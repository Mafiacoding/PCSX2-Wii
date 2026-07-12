/*
 * ee_intc.c - EE interrupt controller register model. See
 * ee_intc.h for exact per-register semantics and PCSX2 source
 * cross-reference (Hw.cpp/HwWrite.cpp).
 */
#include "core/hw/ee_intc.h"
#include <string.h>

#define EE_INTC_STAT 0x1000F000u
#define EE_INTC_MASK 0x1000F010u

static ee_intc_state_t g_intc;

void ee_intc_init(void)
{
    memset(&g_intc, 0, sizeof(g_intc));
}

ee_intc_state_t *ee_intc_get_state(void) { return &g_intc; }

void ee_intc_raise(int irq)
{
    if (irq < 0 || irq > 31)
        return;
    g_intc.stat |= (1u << irq);
}

int ee_intc_pending(void)
{
    return (g_intc.stat & g_intc.mask) != 0u;
}

int ee_intc_mmio_read32(uint32_t addr, uint32_t *out)
{
    switch (addr) {
        case EE_INTC_STAT:
            *out = g_intc.stat;
            return 1;
        case EE_INTC_MASK:
            *out = g_intc.mask;
            return 1;
        default:
            return 0;
    }
}

int ee_intc_mmio_write32(uint32_t addr, uint32_t value)
{
    switch (addr) {
        case EE_INTC_STAT:
            /* Real PCSX2 (HwWrite.cpp mcase(INTC_STAT)):
             * psHu32(INTC_STAT) &= ~value - write 1 to a bit to clear
             * (acknowledge) it. */
            g_intc.stat &= ~value;
            return 1;
        case EE_INTC_MASK:
            /* Real PCSX2 (HwWrite.cpp mcase(INTC_MASK)):
             * psHu32(INTC_MASK) ^= (u16)value - write 1 to a bit to
             * TOGGLE it (not a plain assignment). */
            g_intc.mask ^= (value & 0xFFFFu);
            return 1;
        default:
            return 0;
    }
}
