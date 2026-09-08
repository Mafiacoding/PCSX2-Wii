#ifndef PCSX2WII_EE_JIT_H
#define PCSX2WII_EE_JIT_H

#include <stdint.h>
#include "core/ee/ee_core.h"

/*
 * ee_jit - Round 887 (task #866/#868 continuation): the first real
 * wiring of the ppc_dynarec.c PoC (source/core/recompiler/ppc_dynarec.c)
 * into actual EE execution (ee_core.c's ee_step()).
 *
 * DESIGN / SAFETY MODEL (read before touching the opcode set below):
 *
 * ee_step() does far more per instruction than decode-and-execute one
 * MIPS opcode - its ~60-line epilogue (ee_core.c, right after the
 * giant opcode switch) latches/checks timer and INTC/DMAC interrupts,
 * ticks VBLANK and the EE peripheral timers, and runs half a dozen
 * project-specific HLE heuristics (boot-unblock guards, RPC/CDVD
 * pending checks, browser-menu escalation, etc.) - all deliberately
 * calibrated, across hundreds of prior rounds, to fire at EVERY
 * genuine instruction boundary (see e.g. Round 598's regression, which
 * found that even moving ONE of these checks to a coarser granularity
 * broke real boot progress). A JIT that batched multiple instructions
 * per native call and only ran that epilogue once per BATCH - the
 * "obvious" JIT design - would silently change that cadence and risk
 * exactly the class of regression this project has spent enormous
 * effort finding and fixing before.
 *
 * So this JIT does NOT batch instructions and does NOT skip the
 * epilogue. It intercepts exactly ONE thing: for a small, fixed set of
 * pure-ALU MIPS opcodes (no memory access, no branching, no exception-
 * raising possibility - see ee_jit_try_execute_one()'s implementation
 * for the authoritative list, kept in sync with
 * ppc_dynarec_translate_one()'s own supported set), the REGISTER
 * COMPUTATION for that single instruction is done by calling real,
 * cached, natively-executing PPC750 machine code instead of running
 * the interpreter's C switch case - operating directly on
 * `&st->gpr[0]` (zero-copy, exactly the integration ppc_dynarec.h's
 * own header comment described as the eventual goal). Every other
 * part of ee_step() (delay-slot bookkeeping, the epilogue, exception
 * context, $zero handling, instructions_executed accounting) runs
 * completely unchanged, at the same per-instruction granularity as
 * before this file existed.
 *
 * Each of the 11 supported opcodes' JIT semantics were verified
 * bit-for-bit against ee_core.c's OWN interpreter case bodies (not
 * just an independently-derived MIPS ISA model) before this file was
 * wired in - see docs/STATUS.md Round 887 for the full comparison.
 *
 * Because none of the supported opcodes read memory or depend on PC,
 * a single compiled block is valid for every future occurrence of the
 * exact same 32-bit instruction WORD, anywhere in memory - so the
 * cache below is keyed by instruction encoding, not by address. This
 * sidesteps self-modifying-code invalidation entirely for this opcode
 * class (there is nothing address-dependent to invalidate).
 */

/* Attempts to execute `instr` (the raw MIPS word already fetched at
 * st->pc, exactly as ee_step() decodes it) via the PPC dynarec.
 * Returns 1 if it was executed this way (the caller must skip its own
 * interpreter case for this instruction and fall through to the
 * epilogue), 0 if this opcode isn't currently JIT-supported (caller
 * should run its normal interpreter switch, exactly as before). */
int ee_jit_try_execute_one(ee_state_t *st, uint32_t instr);

/* Diagnostics for verification/STATUS.md writeups and host-native
 * tests - not used by any control-flow decision. */
uint64_t ee_jit_get_executed_count(void);
uint32_t ee_jit_get_cache_size(void);
void     ee_jit_reset_stats_for_test(void); /* test-only: zero counters + cache, so successive host-native tests don't see stale state from an earlier test in the same process */

#endif
