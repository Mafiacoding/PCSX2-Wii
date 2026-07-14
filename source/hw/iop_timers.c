/*
 * iop_timers.c - IOP counter/timer register block + real tick/IRQ
 * model. See iop_timers.h for full scope, citation trail, and the
 * exact real behaviors intentionally NOT modeled (gate modes,
 * prescale dividers, toggle-mode IRQ polarity inversion).
 */
#include "core/hw/iop_timers.h"
#include "core/hw/iop_intc.h"
#include <string.h>

typedef struct {
    uint32_t base;   /* COUNT register address; MODE = base+4, TARGET = base+8 */
} iop_timer_range_t;

/* Real PS2 IOP counter base addresses (pcsx2/IopHw.h). T0-T2 and
 * T3-T5 live in two separate address windows on real hardware, but
 * are otherwise laid out identically (+0/+4/+8 for COUNT/MODE/TARGET). */
static const iop_timer_range_t s_ranges[IOP_TIMERS_COUNT] = {
    { 0x1F801100u }, /* T0 */
    { 0x1F801110u }, /* T1 */
    { 0x1F801120u }, /* T2 */
    { 0x1F801480u }, /* T3 */
    { 0x1F801490u }, /* T4 */
    { 0x1F8014A0u }, /* T5 */
};

/* Real psxCounters[i].interrupt bit positions (pcsx2/IopCounters.cpp
 * psxRcntInit(): T0=0x10(bit4), T1=0x20(bit5), T2=0x40(bit6),
 * T3=0x4000(bit14), T4=0x8000(bit15), T5=0x10000(bit16)), expressed
 * as raw bit indices for iop_intc_raise(int irq). */
static const int s_irq_bit[IOP_TIMERS_COUNT] = { 4, 5, 6, 14, 15, 16 };

static iop_timers_state_t g_timers;

void iop_timers_init(void)
{
    memset(&g_timers, 0, sizeof(g_timers));
}

iop_timers_state_t *iop_timers_get_state(void) { return &g_timers; }

static iop_timer_t *find_timer(uint32_t addr, uint32_t *sub_off_out)
{
    /* Task #214/#215: mask off the KUSEG/KSEG0/KSEG1 segment-select
     * bits before matching, same convention as sif_iop_mmio_read32/
     * write32 and iop_mem_ptr() already use elsewhere in this
     * project - real IOP code can reach any hardware register
     * through any of the three segment aliases. */
    uint32_t phys = addr & 0x1FFFFFFFu;
    for (int i = 0; i < IOP_TIMERS_COUNT; i++) {
        uint32_t base = s_ranges[i].base;
        if (phys >= base && phys < base + 0x0Cu) {
            /* Each timer's 3 registers span base+0x00..base+0x08;
             * base..base+0x0B (12 bytes / 3 words) is the exact
             * window - anything inside it that isn't one of the 3
             * known offsets falls through to "not a known register"
             * below rather than being silently treated as one. */
            uint32_t off = phys - base;
            if (off == 0x00u || off == 0x04u || off == 0x08u) {
                *sub_off_out = off;
                return &g_timers.t[i];
            }
            return NULL; /* inside the window but not one of the 3 known registers */
        }
    }
    return NULL;
}

int iop_timers_mmio_read32(uint32_t addr, uint32_t *out)
{
    uint32_t sub_off;
    iop_timer_t *t = find_timer(addr, &sub_off);
    if (!t)
        return 0;

    switch (sub_off) {
        case 0x00: *out = t->count;  return 1;
        case 0x04: *out = t->mode;   return 1;
        case 0x08: *out = t->target; return 1;
        default:   return 0; /* unreachable given find_timer's checks */
    }
}

int iop_timers_mmio_write32(uint32_t addr, uint32_t value)
{
    uint32_t sub_off;
    iop_timer_t *t = find_timer(addr, &sub_off);
    if (!t)
        return 0;

    switch (sub_off) {
        case 0x00:
            t->count = value;
            return 1;
        case 0x04:
            /* Real PCSX2 (psxRcntWmode16/32): new_mode = (value &
             * IOPCNT_MODE_WRITE_MSK=0x63FF) | (old_mode &
             * IOPCNT_MODE_FLAG_MSK=0x1C00), immediately followed by
             * psxRcntSetNewIntrMode() forcing intrEnable=1 (bit10),
             * targetFlag=0 (bit11), overflowFlag=0 (bit12) - since
             * those three bits are exactly IOPCNT_MODE_FLAG_MSK, the
             * net simplified result is: keep only the real
             * WRITE_MSK-permitted bits from the new value, then
             * unconditionally force bits 10-12 to 1/0/0. Bit 15
             * (stopped) is outside both masks on real hardware -
             * real code only sets it as a gate-mode side effect this
             * project doesn't model, so it is always cleared here. */
            t->mode = (value & 0x63FFu) | IOP_CNT_MODE_INTR_ENABLE;
            /* Real: "Current counter *always* resets on mode write." */
            t->count = 0;
            return 1;
        case 0x08:
            t->target = value;
            return 1;
        default:
            return 0; /* unreachable given find_timer's checks */
    }
}

/* See iop_timers.h's own extensive comment above this function's
 * declaration for the full real-hardware citation trail and honest
 * scope (no gate modes, no prescale dividers, no toggle-mode IRQ
 * polarity inversion - only plain free-running counting plus the two
 * most common real IRQ behaviors: one-shot, and zero-return/repeat). */
void iop_timers_tick(void)
{
    for (int i = 0; i < IOP_TIMERS_COUNT; i++) {
        iop_timer_t *t = &g_timers.t[i];

        if (t->mode & IOP_CNT_MODE_STOPPED)
            continue;

        t->count++;

        uint32_t max_count = (i < 3) ? 0xFFFFu : 0xFFFFFFFFu;

        /* Target match - real _rcntTestTarget(). */
        if (!(t->mode & IOP_CNT_MODE_TARGET_FLAG) && t->count >= t->target) {
            if ((t->mode & IOP_CNT_MODE_TARGET_INTR) && (t->mode & IOP_CNT_MODE_INTR_ENABLE)) {
                iop_intc_raise(s_irq_bit[i]);
                if (!(t->mode & IOP_CNT_MODE_REPEAT_INTR))
                    t->mode &= ~IOP_CNT_MODE_INTR_ENABLE; /* real: one-shot disables further IRQs until MODE is rewritten */
            }
            if (t->mode & IOP_CNT_MODE_ZERO_RETURN) {
                /* Real: "psxCounters[i].count -= psxCounters[i].target;"
                 * - immediate wrap, ready to re-arm for the next
                 * period (the common real periodic-tick-timer use
                 * case: zeroReturn + repeatIntr + targetIntr). Do NOT
                 * leave TARGET_FLAG set here, unlike the else branch
                 * below, so it can fire again next period. */
                if (t->target != 0)
                    t->count -= t->target;
                else
                    t->count = 0;
            } else {
                /* Real: target |= IOPCNT_FUTURE_TARGET - suppressed
                 * until the next overflow "un-sets" it. This
                 * project's equivalent sentinel is TARGET_FLAG
                 * itself, cleared only on overflow below. */
                t->mode |= IOP_CNT_MODE_TARGET_FLAG;
            }
        }

        /* Overflow - real _rcntTestOverflow(). */
        if (t->count > max_count) {
            if ((t->mode & IOP_CNT_MODE_OVERFL_INTR) && (t->mode & IOP_CNT_MODE_INTR_ENABLE)) {
                iop_intc_raise(s_irq_bit[i]);
                if (!(t->mode & IOP_CNT_MODE_REPEAT_INTR))
                    t->mode &= ~IOP_CNT_MODE_INTR_ENABLE;
            }
            t->mode |= IOP_CNT_MODE_OVERFLOW_FLAG;
            t->count -= (max_count + 1u);
            t->mode &= ~IOP_CNT_MODE_TARGET_FLAG; /* real: target's IOPCNT_FUTURE_TARGET bit is cleared on overflow */
        }
    }
}
