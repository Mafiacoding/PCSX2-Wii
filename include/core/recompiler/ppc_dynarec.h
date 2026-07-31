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
 * machine code operating on an in-memory 32-bit register file, using
 * the classic "load operands from context, compute, store back"
 * template pattern. It supports exactly two MIPS opcodes (ADDIU, and
 * SPECIAL/OR) as a demonstration that dynamic codegen + icache
 * invalidation works end-to-end on Wii hardware via libogc. Any
 * instruction it doesn't recognize aborts translation of that block;
 * the caller (ee_core) should fall back to the interpreter for it.
 *
 * There is no register allocation, no branch handling inside blocks,
 * no linking between compiled blocks, and no invalidation-on-write
 * strategy for self-modifying code. All of that would be required
 * before this could be called a real recompiler.
 */

typedef void (*ppc_block_fn)(uint32_t *gpr32);

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
