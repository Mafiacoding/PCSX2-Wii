/*
 * system.h - interleaved EE/IOP scheduler
 *
 * Real PS2 hardware runs the EE and IOP as two genuinely independent
 * CPUs executing in parallel. ee_core.c and iop_core.c model each CPU
 * in isolation and, until now, neither ever actually ran alongside
 * the other - iop_core_run() and ee_core_run() each loop internally
 * until their own core halts, with no interleaving at all. That was
 * fine for testing each core standalone, but a real EE<->IOP
 * handshake over the SIF mailbox registers (core/hw/sif.h) requires
 * both sides to make progress in the same wall-clock timeframe - the
 * EE typically has to poll a flag and wait for the IOP to set it (or
 * vice versa), which can't happen if one core runs to completion
 * before the other starts at all.
 *
 * This is a round-robin scheduler: per slice, up to EE_IOP_CLOCK_RATIO
 * (8) EE instructions, then one IOP instruction, repeat. The 8:1 ratio
 * matches real hardware's EE (294.912 MHz) vs IOP (36.864 MHz) clock
 * rates (294.912 / 36.864 == 8 exactly). This used to be strict 1:1
 * instruction stepping - a documented simplification (see
 * docs/ROADMAP.md's "clock-rate-aware EE:IOP scheduler" entry). Note
 * this is still an INSTRUCTION-count ratio, not a real cycle-accurate
 * model: real MIPS instructions don't all take one cycle each (loads,
 * branches, multiply/divide differ), so 8 EE instructions per IOP
 * instruction approximates the clock ratio without being truly
 * cycle-accurate - a genuine cycle-cost model is a further-out
 * refinement, not attempted here. What this provides, for the first
 * time in this project, is genuine cross-CPU visibility: a write the
 * EE makes to a SIF register is visible to the IOP within the same
 * slice or the next, and vice versa, which is enough to prove out a
 * real handshake protocol (see tests/test_system_handshake.c).
 */
#ifndef PCSX2_WII_SYSTEM_H
#define PCSX2_WII_SYSTEM_H

#include <stdint.h>
#include "core/bios_loader.h"

/* Initializes both cores (which transitively initializes DMA/GS/GIF/
 * SIF via ee_core_init()). ee_bios and iop_bios may point to the same
 * bios_image_t - real hardware shares one physical ROM between both
 * CPUs - or different ones. Returns 0 on success. */
int system_init(const bios_image_t *ee_bios, const bios_image_t *iop_bios);

/* Steps the EE and IOP alternately - up to EE_IOP_CLOCK_RATIO EE
 * instructions, then one IOP instruction, per slice (see system.h's
 * header comment for why 8:1) - until both cores have halted, or
 * until max_slices slices have run, whichever comes first. Pass
 * max_slices == 0 for no limit (real hardware has none; used by
 * main.c). Host-native tests should pass a finite cap so a
 * scheduling/protocol bug produces a clean test failure instead of a
 * hang.
 *
 * Returns 1 if both cores halted on their own, 0 if the slice limit
 * was hit first (only possible when max_slices != 0). */
int system_run_interleaved(uint64_t max_slices);

/* Real hardware's EE (294.912 MHz) : IOP (36.864 MHz) clock ratio -
 * exactly 8. Exposed so tests can assert on it instead of hardcoding
 * the number a second time. */
#define EE_IOP_CLOCK_RATIO 8

#endif
