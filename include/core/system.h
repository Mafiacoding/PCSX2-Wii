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
 * This is a round-robin scheduler: EE_IOP_STEP_RATIO (8, matching
 * real hardware's ~294MHz EE vs ~33-36MHz IOP clock ratio - see
 * system.c) EE instructions, then one IOP instruction, repeat. This is
 * NOT cycle-accurate (different MIPS instructions take different real
 * cycle counts on both cores, none of which is modeled) - it's a
 * ratio-aware approximation, an improvement over this project's
 * original 1:1 stepping but still an honest simplification, not a
 * claim of real timing fidelity. What this does provide, for the
 * first time in this project, is genuine cross-CPU visibility: a
 * write the EE makes to a SIF register is visible to the IOP on its
 * very next step (rather than after a whole separate run-to-
 * completion pass), and vice versa, which is enough to prove out a
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

/* Steps the EE and IOP alternately (EE_IOP_STEP_RATIO, currently 8,
 * EE instructions per 1 IOP instruction per slice - see system.c)
 * until both cores have halted, or until max_slices slices have run -
 * whichever comes first. Pass max_slices == 0 for no limit (real
 * hardware has none; used by main.c). Host-native tests should pass a
 * finite cap so a scheduling/protocol bug produces a clean test
 * failure instead of a hang.
 *
 * Returns 1 if both cores halted on their own, 0 if the slice limit
 * was hit first (only possible when max_slices != 0). */
int system_run_interleaved(uint64_t max_slices);

#endif
