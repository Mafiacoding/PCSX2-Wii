/*
 * iop_timers.c - IOP counter/timer register block. See
 * iop_timers.h for scope (deliberately a plain, non-ticking
 * register stub - no real counting behavior) and address reference.
 */
#include "core/hw/iop_timers.h"
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

static iop_timers_state_t g_timers;

void iop_timers_init(void)
{
    memset(&g_timers, 0, sizeof(g_timers));
}

iop_timers_state_t *iop_timers_get_state(void) { return &g_timers; }

static iop_timer_t *find_timer(uint32_t addr, uint32_t *sub_off_out)
{
    for (int i = 0; i < IOP_TIMERS_COUNT; i++) {
        uint32_t base = s_ranges[i].base;
        if (addr >= base && addr < base + 0x0Cu) {
            /* Each timer's 3 registers span base+0x00..base+0x08;
             * base..base+0x0B (12 bytes / 3 words) is the exact
             * window - anything inside it that isn't one of the 3
             * known offsets falls through to "not a known register"
             * below rather than being silently treated as one. */
            uint32_t off = addr - base;
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
        case 0x00: t->count  = value; return 1;
        case 0x04: t->mode   = value; return 1; /* plain - no flag-preservation quirk, see header */
        case 0x08: t->target = value; return 1;
        default:   return 0; /* unreachable given find_timer's checks */
    }
}
