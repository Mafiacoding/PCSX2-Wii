/*
 * ee_timers.c - EE peripheral counter/timer register block + real
 * tick/IRQ model. See ee_timers.h for full scope, citation trail, and
 * the exact real behaviors intentionally NOT modeled (CLKS alternate
 * clock sources, gate modes, HOLD latching).
 */
#include "core/hw/ee_timers.h"
#include "core/hw/ee_intc.h"
#include <string.h>

typedef struct {
    uint32_t base;   /* COUNT register address; MODE=base+0x10, COMP=base+0x20, HOLD=base+0x30 */
    int has_hold;
} ee_timer_range_t;

/* Real PS2 EE counter base addresses (pcsx2/Hw.h EERegisterAddresses:
 * T0=0x10000000, T1=0x10000800, T2=0x10001000, T3=0x10001800). Only
 * T0/T1 implement a real HOLD register. */
static const ee_timer_range_t s_ranges[EE_TIMERS_COUNT] = {
    { 0x10000000u, 1 }, /* T0 */
    { 0x10000800u, 1 }, /* T1 */
    { 0x10001000u, 0 }, /* T2 */
    { 0x10001800u, 0 }, /* T3 */
};

/* Real EE_INTC source bit positions for TIMER0-3 (ee_intc.h's own
 * citation: pcsx2/Hw.h's EE_INTC enum has GS=0, SBUS=1, VBLANK_S=2,
 * VBLANK_E=3, VIF0=4, VIF1=5, VU0=6, VU1=7, IPU=8, TIMER0=9,
 * TIMER1=10, TIMER2=11, TIMER3=12, SFIFO=13, VU0WATCHDOG=14). */
static const int s_irq_bit[EE_TIMERS_COUNT] = { 9, 10, 11, 12 };

static ee_timers_state_t g_timers;

/* Round 127 (task #172/#247, 167th finding): real EE clock is
 * 294,912,000 Hz; real NTSC horizontal-sync (HSYNC) line rate is
 * 15,734.264 Hz - both well-established, publicly documented
 * hardware/analog-video constants, independent of any BIOS-specific
 * data. Their ratio gives a real HBLNK period of ~18,743 EE bus
 * cycles, i.e. how many of this project's "1 instruction = 1 cycle"
 * ticks (see file-level comment) correspond to one real HSYNC pulse.
 * g_bus_tick_counter is a free-running count of ee_timers_tick()
 * calls, used to gate CLKS=1/2/3 timers to their real, slower-than-
 * BUSCLK increment rates (previously all four CLKS values were
 * silently treated as BUSCLK/1:1 - see header comment for how this
 * was found and why it mattered for task #247). */
static const uint64_t EE_HBLNK_PERIOD_CYCLES = 18743ull;
static uint64_t g_bus_tick_counter = 0;

void ee_timers_init(void)
{
    memset(&g_timers, 0, sizeof(g_timers));
    g_bus_tick_counter = 0;
}

ee_timers_state_t *ee_timers_get_state(void) { return &g_timers; }

/* sub_off: 0x00=COUNT, 0x10=MODE, 0x20=COMP, 0x30=HOLD (real hardware
 * layout - each register is a full 0x10-aligned slot, unlike the IOP's
 * packed +0/+4/+8 layout, per pcsx2/Hw.h's real address deltas). */
static ee_timer_t *find_timer(uint32_t addr, uint32_t *sub_off_out, int *has_hold_out)
{
    for (int i = 0; i < EE_TIMERS_COUNT; i++) {
        uint32_t base = s_ranges[i].base;
        if (addr >= base && addr < base + 0x40u) {
            uint32_t off = addr - base;
            if (off == 0x00u || off == 0x10u || off == 0x20u || off == 0x30u) {
                *sub_off_out = off;
                *has_hold_out = s_ranges[i].has_hold;
                return &g_timers.t[i];
            }
            return NULL; /* inside the window but not one of the 4 known registers */
        }
    }
    return NULL;
}

int ee_timers_mmio_read32(uint32_t addr, uint32_t *out)
{
    uint32_t sub_off;
    int has_hold;
    ee_timer_t *t = find_timer(addr, &sub_off, &has_hold);
    if (!t)
        return 0;

    switch (sub_off) {
        case 0x00: *out = t->count; return 1;
        case 0x10: *out = t->mode;  return 1;
        case 0x20: *out = t->comp;  return 1;
        case 0x30: *out = has_hold ? t->hold : 0; return 1;
        default:   return 0; /* unreachable given find_timer's checks */
    }
}

int ee_timers_mmio_write32(uint32_t addr, uint32_t value)
{
    uint32_t sub_off;
    int has_hold;
    ee_timer_t *t = find_timer(addr, &sub_off, &has_hold);
    if (!t)
        return 0;

    switch (sub_off) {
        case 0x00:
            t->count = value;
            return 1;
        case 0x10:
            /* Real rcntWmode(): software writes MODE to configure
             * clock source/gate/CUE/CMPE/OVFE/repeat/toggle, and to
             * ACK the EQUF/OVFF flags by writing 1 to them (real
             * hardware: writing 1 to bit 11/12 clears that bit,
             * mirroring INTC_STAT's own write-1-to-clear polarity
             * already documented in ee_intc.h). This project keeps
             * every configuration bit as written (no CLKS/GATE
             * remapping, matching the "not modeled" scope above), and
             * only special-cases the two write-1-to-clear flag bits. */
            t->mode = (value & ~(uint32_t)(EE_CNT_MODE_EQUF | EE_CNT_MODE_OVFF))
                      | (t->mode & (EE_CNT_MODE_EQUF | EE_CNT_MODE_OVFF)
                         & ~(value & (EE_CNT_MODE_EQUF | EE_CNT_MODE_OVFF)));
            return 1;
        case 0x20:
            t->comp = value;
            return 1;
        case 0x30:
            if (has_hold)
                t->hold = value;
            return 1;
        default:
            return 0; /* unreachable given find_timer's checks */
    }
}

/* See ee_timers.h's own extensive comment above ee_timers_tick()'s
 * declaration for the full real-hardware citation trail and honest
 * scope. Must be called unconditionally once per EE instruction step,
 * regardless of what the CPU itself is doing - same rationale as
 * iop_timers_tick()/ee_check_vblank(): real counters run off the
 * system clock, not off conditional CPU state. */
void ee_timers_tick(void)
{
    /* Round 87 (127th finding continuation, live host-native
     * evidence): a diagnostic run showed real BIOS code configuring
     * ALL FOUR timers' COMP register to exactly 0xFFFF - the max
     * value of a 16-bit counter, not a coincidental 32-bit value.
     * This confirms real PS2 EE T0-T3 are 16-bit counters (matching
     * the IOP's own T0-T2, which iop_timers.c already models as
     * max_count=0xFFFF) - NOT full 32-bit as this function's first
     * draft incorrectly assumed. Fixed here: overflow now wraps at
     * 0xFFFF (real rcntUpdate()'s COUNTER_OVERFLOW_VAL), matching the
     * exact real value real code is arming COMP against. */
    static const uint32_t EE_TIMER_MAX_COUNT = 0xFFFFu;

    g_bus_tick_counter++;

    for (int i = 0; i < EE_TIMERS_COUNT; i++) {
        ee_timer_t *t = &g_timers.t[i];

        if (!(t->mode & EE_CNT_MODE_CUE))
            continue; /* real: CUE=0 means the counter is stopped */

        /* Round 127 (167th finding): CLKS clock-source divider gate.
         * Real hardware's four dividers all run off a continuously
         * advancing bus clock, not a per-timer phase reset at
         * CUE-set time, hence gating on the shared free-running
         * g_bus_tick_counter rather than restarting a counter local
         * to this timer. */
        switch (t->mode & EE_CNT_MODE_CLKS) {
            case 0: break; /* BUSCLK: every tick, unchanged prior behavior */
            case 1: if (g_bus_tick_counter % 16ull) continue; break;   /* BUSCLK/16 */
            case 2: if (g_bus_tick_counter % 256ull) continue; break;  /* BUSCLK/256 */
            default: if (g_bus_tick_counter % EE_HBLNK_PERIOD_CYCLES) continue; break; /* HBLNK (CLKS=3) */
        }

        t->count++;

        /* Compare match - real rcntUpdate()'s target-check. */
        if (t->count == t->comp) {
            t->mode |= EE_CNT_MODE_EQUF;
            if (t->mode & EE_CNT_MODE_CMP_ENABLE) {
                ee_intc_raise(s_irq_bit[i]);
                if (!(t->mode & EE_CNT_MODE_REPEAT_IRQ))
                    t->mode &= ~EE_CNT_MODE_CMP_ENABLE; /* real: one-shot disables further compare IRQs until MODE is rewritten */
            }
            if (t->mode & EE_CNT_MODE_ZERO_RETURN)
                t->count = 0; /* real: zero-return wraps immediately on match, ready for the next period */
        }

        /* Overflow - real rcntUpdate()'s 16-bit wrap check (see the
         * function-level comment above for the live evidence this is
         * 16-bit, not 32-bit). Skipped for zero-return timers whose
         * count never reaches past COMP in the first place (they
         * wrap via the match branch above instead). */
        if (t->count > EE_TIMER_MAX_COUNT) {
            t->mode |= EE_CNT_MODE_OVFF;
            if (t->mode & EE_CNT_MODE_OVF_ENABLE) {
                ee_intc_raise(s_irq_bit[i]);
                if (!(t->mode & EE_CNT_MODE_REPEAT_IRQ))
                    t->mode &= ~EE_CNT_MODE_OVF_ENABLE;
            }
            t->count -= (EE_TIMER_MAX_COUNT + 1u);
        }
    }
}
