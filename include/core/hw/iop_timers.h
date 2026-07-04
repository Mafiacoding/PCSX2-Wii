/*
 * iop_timers.h - IOP counter/timer register block (T0-T5)
 *
 * Real PS2 IOP address ranges (pcsx2/IopHw.h): six hardware counters,
 * each with a COUNT, MODE, and TARGET register:
 *
 *   T0: COUNT 0x1F801100  MODE 0x1F801104  TARGET 0x1F801108
 *   T1: COUNT 0x1F801110  MODE 0x1F801114  TARGET 0x1F801118
 *   T2: COUNT 0x1F801120  MODE 0x1F801124  TARGET 0x1F801128
 *   T3: COUNT 0x1F801480  MODE 0x1F801484  TARGET 0x1F801488
 *   T4: COUNT 0x1F801490  MODE 0x1F801494  TARGET 0x1F801498
 *   T5: COUNT 0x1F8014A0  MODE 0x1F8014A4  TARGET 0x1F8014A8
 *
 * SCOPE - deliberately a PLAIN register stub, more limited than the
 * DMA/INTC/SIF register models elsewhere in this project: COUNT,
 * MODE, and TARGET are just latched storage with no special-case
 * write behavior at all. Real hardware/PCSX2 (pcsx2/IopCounters.cpp,
 * `psxRcntWmode16/32`) does something considerably more involved on
 * a MODE write - it preserves certain status flag bits across the
 * write (via an IOPCNT_MODE_WRITE_MSK / IOPCNT_MODE_FLAG_MSK split),
 * recomputes IRQ-mode state, and interacts with a live, ticking
 * counter service (`psxRcntSync`, `_rcntSet`) that advances COUNT
 * based on elapsed CPU cycles, gate mode, and clock source, firing an
 * interrupt when COUNT reaches TARGET or overflows.
 *
 * None of that ticking/gating/IRQ behavior is modeled here - these
 * registers do NOT advance on their own, and MODE's flag-preservation
 * quirk is not replicated (a write here plainly overwrites the whole
 * register). This is intentional: modeling real counter behavior
 * needs to tick forward in sync with instruction execution (like a
 * scheduler, not a register latch), which is a meaningfully bigger
 * effort than the register-stub pattern used for iop_dma.c/
 * iop_intc.c - see docs/ROADMAP.md. This stub exists so that BIOS/IOP
 * code reading/writing these addresses at least gets consistent,
 * addressable storage back instead of silently falling through to
 * IOP RAM (these addresses are far outside the 2MB RAM window, so
 * without this they'd already correctly no-op today - but explicit
 * storage is more honest than an accidental no-op).
 */
#ifndef PCSX2_WII_IOP_TIMERS_H
#define PCSX2_WII_IOP_TIMERS_H

#include <stdint.h>

#define IOP_TIMERS_COUNT 6

typedef struct {
    uint32_t count;
    uint32_t mode;
    uint32_t target;
} iop_timer_t;

typedef struct {
    iop_timer_t t[IOP_TIMERS_COUNT];
} iop_timers_state_t;

void iop_timers_init(void);

int iop_timers_mmio_read32(uint32_t addr, uint32_t *out);
int iop_timers_mmio_write32(uint32_t addr, uint32_t value);

iop_timers_state_t *iop_timers_get_state(void);

#endif
