#ifndef PCSX2WII_PPC_DYNAREC_H
#define PCSX2WII_PPC_DYNAREC_H

#include <stdint.h>
#include <stddef.h>

/*
 * ppc_dynarec - EXPERIMENTAL proof-of-concept recompiler.
 *
 * THIS IS NOT PCSX2'S RECOMPILER. PCSX2's real EE/VU JITs emit x86-64
 * (and, in modern builds, AArch64) machine code and are tens of
 * thousands of lines of architecture-specific codegen, register
 * allocation, and pipeline scheduling. There is no PowerPC backend for
 * them, and writing one is a multi-year undertaking, not something
 * produced in a single project pass.
 *
 * What this file actually does: translates a short, straight-line
 * (no internal branches) run of MIPS instructions into native PPC750
 * machine code operating on an in-memory register file, using the
 * classic "load operands from context, compute, store back" template
 * pattern. It supports exactly two MIPS opcodes (ADDIU, and
 * SPECIAL/OR) as a demonstration that dynamic codegen + icache
 * invalidation works end-to-end on Wii hardware via libogc. Any
 * instruction it doesn't recognize aborts translation of that block;
 * the caller (ee_core) should fall back to the interpreter for it.
 *
 * Round 880 (task #864) fix: the register context used to be a flat
 * `uint32_t *gpr32` (one 32-bit word per register), which silently
 * dropped the EE R5900's real 64-bit register semantics - MIPS64
 * ADDIU/ADDU etc. compute a 32-bit result and sign-extend it into the
 * full 64-bit register, and MIPS OR/AND/XOR/NOR operate on the full
 * 64-bit value with no truncation at all. Getting these two rules
 * mixed up (or ignoring them) silently corrupts the upper 32 bits of
 * every register a translated block touches. The context is now
 * `ppc_dynarec_gpr128_t gpr[32]`, laid out identically to this
 * project's own `ee_reg128_t gpr[32]` (see include/core/ee/ee_core.h)
 * so a real integration can eventually pass `&ee->gpr[0]` straight in
 * with no copying - each slot is 16 bytes: `ud0` (the low 64 bits,
 * what these Phase-1 opcodes read/write) followed by `ud1` (the high
 * 64 bits, only touched by MMI ops - left completely alone here).
 *
 * IMPORTANT (PPC750/Broadway is BIG-ENDIAN): within each 8-byte `ud0`
 * field, the 32-bit word at the LOWER address is the HIGH half of the
 * 64-bit value, and the word at address+4 is the LOW half - the
 * opposite of what a little-endian (x86) host would use. Every load/
 * store offset in this file accounts for that; see REG_HI()/REG_LO().
 *
 * There is still no register allocation, no branch handling inside
 * blocks, no linking between compiled blocks, and no invalidation-on-
 * write strategy for self-modifying code. All of that would be
 * required before this could be called a real recompiler. This PoC is
 * also still NOT called from anywhere in ee_core.c/system.c/main.c -
 * wiring it into real execution is explicitly out of scope for this
 * round (see docs/STATUS.md task #864).
 *
 * Round 890 (task #874) update: this PoC IS now wired into real EE
 * execution (see ee_jit.c/ee_jit.h, done back in Round 887) and its
 * context contract just grew. MULT/MULTU/DIV/DIVU/MFHI/MTHI/MFLO/MTLO
 * need somewhere real to read/write the R5900's dedicated HI/LO
 * registers, which this PoC's `gpr[32]` context array alone can't
 * represent. Rather than widening the function-pointer signature (and
 * touching every existing call site/opcode block), HI/LO are modeled
 * as two more MIPS-register-shaped slots immediately following gpr[31]
 * in the SAME contiguous array - i.e. the context this PoC's generated
 * code addresses is really `ppc_dynarec_gpr128_t ctx[34]`, where
 * ctx[0..31] are the real MIPS GPRs, ctx[32] is HI, and ctx[33] is LO
 * (see ppc_dynarec.c's HI_IDX/LO_IDX). This only works because
 * `ee_state_t` (include/core/ee/ee_core.h) was deliberately reordered
 * in Round 890 to put `hi, lo` immediately after `gpr[32]` - see that
 * struct's own comment for why that reordering is safe. ee_jit.c's
 * `_Static_assert`s next to the `fn(...)` call site are what actually
 * enforce this contract at compile time; this header and
 * ppc_dynarec.c itself intentionally still don't #include ee_core.h,
 * to keep this file's only real dependency on the wider codebase
 * exactly what it always was: "a flat array of 16-byte register
 * slots", now just two slots longer than it looks from `gpr[32]` alone.
 */

typedef struct {
    uint64_t ud0; /* low 64 bits - what ADDIU/OR (Phase 1) operate on */
    uint64_t ud1; /* high 64 bits - MMI-only, untouched by this PoC */
} ppc_dynarec_gpr128_t;

typedef void (*ppc_block_fn)(ppc_dynarec_gpr128_t *gpr);

typedef struct {
    uint32_t *code;         /* executable buffer (memalign'd, 32 bytes) */
    size_t    capacity_words;
    size_t    used_words;
} ppc_codegen_ctx_t;

int  ppc_dynarec_init(ppc_codegen_ctx_t *ctx, size_t max_instructions);
void ppc_dynarec_free(ppc_codegen_ctx_t *ctx);

/* Attempts to translate a single MIPS instruction word into the
 * codegen buffer. Returns 0 on success, -1 if the opcode isn't
 * supported by this PoC (translation of the block should stop here). */
int ppc_dynarec_translate_one(ppc_codegen_ctx_t *ctx, uint32_t mips_instr);

/* Emits the trailing 'blr' and flushes d-cache / invalidates i-cache
 * over the generated range so the PPC core can safely execute it.
 * Returns a callable function pointer, or NULL on failure. */
ppc_block_fn ppc_dynarec_finalize(ppc_codegen_ctx_t *ctx);

#endif
