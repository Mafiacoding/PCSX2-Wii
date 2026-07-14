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
 * SCOPE (updated, task #214/#215, 85th/86th findings): this WAS a
 * deliberate plain register stub with no ticking/IRQ behavior at all
 * (see git history / docs/ROADMAP.md for that earlier round). That
 * gap is now closed with a real, but intentionally scoped-down,
 * subset of PCSX2's pcsx2/IopCounters.cpp model - see the detailed
 * comment above iop_timers_tick()'s own declaration below for the
 * full citation trail (MODE write masking, IRQ bit positions, and
 * exactly which real behaviors are NOT modeled: gate modes, prescale
 * dividers, toggle-mode IRQ polarity inversion).
 */
#ifndef PCSX2_WII_IOP_TIMERS_H
#define PCSX2_WII_IOP_TIMERS_H

#include <stdint.h>

#define IOP_TIMERS_COUNT 6

/* Task #214/#215 continuation (85th/86th findings): the header
 * comment above USED to say this was a deliberate no-ticking
 * register stub (see docs/ROADMAP.md history) - that gap is now
 * closed. Root-causing the EE poll loop at pc=0x8000CFD0-0x8000CFD4
 * (waits on INTC_SBUS, see iop_icfg.h) found the IOP itself sits
 * permanently `idle` (per iop_core.h's `idle` field) because nothing
 * ever raises a real IOP hardware interrupt to wake it - and real
 * IOP counters/timers are exactly the mechanism real hardware uses
 * for this (a real IOP kernel's thread scheduler is driven by
 * periodic timer-tick interrupts, independent of what the CPU
 * itself is doing). This file now implements a real, but
 * intentionally scoped-down, subset of PCSX2's pcsx2/IopCounters.cpp
 * counting/IRQ model:
 *
 *   - `iop_timers_tick()` increments every counter's COUNT by 1 per
 *     call. Real hardware ticks each counter by its own configured
 *     clock source/divisor (PSXCLK, HBLNK, or a prescaled version -
 *     see IOPCNT_MODE fields below) - this project instead uses the
 *     SAME "1 call = 1 tick" simplification already established and
 *     documented for the EE's own COP0 Count register and VBLANK
 *     timing in ee_core.c (`ee_check_vblank()`'s own comment: "no
 *     cycle-accurate timing model... without precise bus-clock-rate
 *     fidelity, which isn't verifiable without a real timing model").
 *     Honest gap: gate modes (H/V-blank gating), prescale dividers
 *     (t2Prescale/t4_5Prescale), and the toggle-mode IRQ-polarity
 *     inversion are NOT modeled - only plain free-running counting
 *     plus the two most commonly-used real IRQ behaviors (one-shot
 *     and zero-return/repeat) are.
 *   - MODE register write semantics ported from real PCSX2
 *     (`psxRcntWmode16/32`'s IOPCNT_MODE_WRITE_MSK=0x63FF /
 *     IOPCNT_MODE_FLAG_MSK=0x1C00 split, and `psxRcntSetNewIntrMode`'s
 *     always-re-enable-and-clear-flags behavior): new_mode = (value &
 *     0x63FF) | 0x0400 (forces intrEnable=1, targetFlag=0,
 *     overflowFlag=0; bit 15 "stopped" is unconditionally cleared
 *     since real hardware only sets it via a gate-mode side effect
 *     this project doesn't model). COUNT always resets to 0 on a
 *     MODE write, matching real hardware's "Current counter *always*
 *     resets on mode write."
 *   - Target-match/overflow-triggered IRQ delivery, real bit
 *     positions from `psxRcntInit()`'s `psxCounters[i].interrupt`
 *     assignments (T0=bit4/0x10, T1=bit5/0x20, T2=bit6/0x40,
 *     T3=bit14/0x4000, T4=bit15/0x8000, T5=bit16/0x10000), raised via
 *     the existing `iop_intc_raise()` hook (previously unused by
 *     anything in this project).
 */
typedef struct {
    uint32_t count;
    uint32_t mode;
    uint32_t target;
} iop_timer_t;

typedef struct {
    iop_timer_t t[IOP_TIMERS_COUNT];
} iop_timers_state_t;

/* Real psxCounterMode bit layout (pcsx2/IopCounters.h union
 * psxCounterMode) - see the file-level comment above for which of
 * these this project's simplified tick model actually honors. */
#define IOP_CNT_MODE_GATE_ENABLE    0x0001u
#define IOP_CNT_MODE_GATE_MODE      0x0006u /* bits 1-2, not modeled */
#define IOP_CNT_MODE_ZERO_RETURN    0x0008u
#define IOP_CNT_MODE_TARGET_INTR    0x0010u
#define IOP_CNT_MODE_OVERFL_INTR    0x0020u
#define IOP_CNT_MODE_REPEAT_INTR    0x0040u
#define IOP_CNT_MODE_TOGGLE_INTR    0x0080u /* not modeled */
#define IOP_CNT_MODE_EXT_SIGNAL     0x0100u /* not modeled */
#define IOP_CNT_MODE_T2_PRESCALE    0x0200u /* not modeled */
#define IOP_CNT_MODE_INTR_ENABLE    0x0400u
#define IOP_CNT_MODE_TARGET_FLAG    0x0800u
#define IOP_CNT_MODE_OVERFLOW_FLAG  0x1000u
#define IOP_CNT_MODE_T45_PRESCALE   0x6000u /* bits 13-14, not modeled */
#define IOP_CNT_MODE_STOPPED        0x8000u

void iop_timers_init(void);

int iop_timers_mmio_read32(uint32_t addr, uint32_t *out);
int iop_timers_mmio_write32(uint32_t addr, uint32_t value);

/* Advances every counter by one tick and delivers any real target-
 * match/overflow interrupt this causes - see the file-level comment
 * above. Must be called unconditionally once per iop_core_step()
 * call, REGARDLESS of whether the IOP CPU itself is idle (real
 * counters run off the system clock, not off CPU instruction
 * execution - this is precisely the real mechanism that wakes a real
 * idle IOP thread scheduler back up). */
void iop_timers_tick(void);

iop_timers_state_t *iop_timers_get_state(void);

#endif
