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

/* Round 716 (task #696-699, per the user's "make the code work that
 * would wake it up" directive): per-cause real IRQ-raise hit counter,
 * mirroring ee_timers_get_irq_count()'s (Round 715) diagnostic
 * convention. Lets a boot survey empirically confirm how many times
 * each of the 15 real EE_INTC causes (GS=0 .. VU0WATCHDOG=14) is
 * actually raised - in particular VBLANK_START=2/VBLANK_END=3 vs. the
 * real BIOS-resident per-cause handler-table dispatch's own null-jalr
 * recovery-guard hit count (source/core/ee/ee_core.c) - to determine
 * whether VBLANK interrupts are being raised but failing to dispatch
 * (many guard hits) or dispatching successfully to a real, registered
 * handler every time (raise count high, guard hits low/attributable
 * elsewhere). Purely additive, does not affect emulated behavior. */
static uint32_t g_raise_count[32];

uint32_t ee_intc_get_raise_count(int irq)
{
    if (irq < 0 || irq > 31)
        return 0;
    return g_raise_count[irq];
}

void ee_intc_init(void)
{
    memset(&g_intc, 0, sizeof(g_intc));
    memset(&g_raise_count, 0, sizeof(g_raise_count));
}

ee_intc_state_t *ee_intc_get_state(void) { return &g_intc; }

void ee_intc_raise(int irq)
{
    if (irq < 0 || irq > 31)
        return;
    g_intc.stat |= (1u << irq);
    g_raise_count[irq]++;
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
