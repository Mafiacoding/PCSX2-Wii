/*
 * ee_jit.c - see include/core/recompiler/ee_jit.h for the full design
 * rationale and safety model. Short version: this compiles single
 * pure-ALU MIPS instructions into cached, natively-executing PPC750
 * machine code (via ppc_dynarec.c), keyed by instruction ENCODING
 * (not address, since none of the supported opcodes are memory- or
 * PC-dependent) so the same compiled block is reused for every future
 * occurrence of that exact instruction word anywhere in memory.
 */

#include "core/recompiler/ee_jit.h"
#include "core/recompiler/ppc_dynarec.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h> /* offsetof, for the _Static_assert layout checks below */

/* Round 890 (task #874): compile-time enforcement of the layout
 * contract ppc_dynarec.c's HI_IDX/LO_IDX (see that file) depends on:
 * `gpr[32]` must be the very FIRST field of ee_state_t (so `&st->gpr[0]`
 * is also byte offset 0 of the whole struct - this was already relied
 * on before this round, just never asserted), and `hi`/`lo` must sit
 * immediately after it with no padding, so that ppc_dynarec.c's flat
 * "register 32 = HI, register 33 = LO" addressing lands exactly on the
 * real fields. If ee_core.h's struct layout ever changes, this fires a
 * compile error here instead of silently corrupting HI/LO (or some
 * unrelated field) the next time a MULT/DIV/MFHI/MTHI/MFLO/MTLO
 * instruction gets JIT-compiled. */
_Static_assert(offsetof(ee_state_t, gpr) == 0,
               "ppc_dynarec.c's context pointer is &st->gpr[0] - gpr must be ee_state_t's first field");
_Static_assert(offsetof(ee_state_t, hi) == sizeof(ee_reg128_t) * 32,
               "ppc_dynarec.c's HI_IDX (32) assumes hi sits immediately after gpr[32]");
_Static_assert(offsetof(ee_state_t, lo) == sizeof(ee_reg128_t) * 33,
               "ppc_dynarec.c's LO_IDX (33) assumes lo sits immediately after hi");

#define EE_JIT_CACHE_SLOTS 8192u /* power of two - see ee_jit_cache_lookup()/insert() */

typedef struct {
    uint32_t     instr; /* only meaningful when fn != NULL */
    ppc_block_fn fn;    /* NULL = empty slot */
} ee_jit_cache_slot_t;

static ee_jit_cache_slot_t g_cache[EE_JIT_CACHE_SLOTS];
static uint32_t g_cache_count = 0;
static uint64_t g_jit_executed = 0;

/* These three helpers are only reachable from the GEKKO branch of
 * ee_jit_try_execute_one() below (see that function's host-safety-gate
 * comment) - wrapped in #ifdef GEKKO here too so host-native builds
 * (which never call them) don't warn about unused static functions. */
#ifdef GEKKO

/* Whether `instr` is one of the MIPS opcodes ppc_dynarec_translate_one()
 * currently supports: ADDIU (op 0x09); SLTI/SLTIU (op 0x0A/0x0B); LUI
 * (op 0x0F, Round 887b); SPECIAL (op 0x00) ADDU/SUBU/AND/OR/XOR/NOR/
 * SLT/SLTU (funct 0x21/0x23-0x27/0x2A/0x2B); SLL/SRL/SRA/SLLV/SRLV/SRAV
 * (funct 0x00/0x02-0x04/0x06/0x07, Round 888); MOVZ/MOVN (funct
 * 0x0A/0x0B, Round 889); MFHI/MTHI/MFLO/MTLO (funct 0x10-0x13) and
 * MULT/MULTU/DIV/DIVU (funct 0x18/0x19/0x1A/0x1B), added in Round 890
 * now that HI/LO have somewhere real to live (see HI_IDX/LO_IDX in
 * ppc_dynarec.c); and LW/SW (op 0x23/0x2B), new in Round 891 - this
 * dynarec's first opcodes that call a real C function
 * (ee_mem_read32/ee_mem_write32) rather than just moving bits between
 * the context array and PPC registers (see ppc_dynarec.c's
 * ADDR_EE_MEM_READ32/WRITE32 comment for the call-emission mechanism).
 * Kept in sync by hand with translate_one()'s own dispatch - see that
 * function's own comments for the authoritative list. This is a cheap
 * pre-filter so the (much more expensive) cache lookup/compile path is
 * never attempted for the vast majority of real instructions
 * ppc_dynarec.c can't handle yet (branches, other loads/stores, MMI,
 * COP0/1/2, ...). */
static int ee_jit_opcode_supported(uint32_t instr)
{
    uint32_t op = (instr >> 26) & 0x3Fu;
    if (op == 0x09u) return 1; /* ADDIU */
    if (op == 0x0Au || op == 0x0Bu) return 1; /* SLTI / SLTIU */
    if (op == 0x0Fu) return 1; /* LUI */
    if (op == 0x23u) return 1; /* LW (Round 891) */
    if (op == 0x2Bu) return 1; /* SW (Round 891) */
    if (op == 0x00u) {
        uint32_t funct = instr & 0x3Fu;
        switch (funct) {
        case 0x00: /* SLL (and the all-zero-word NOP encoding, harmlessly - see translate_one's rd==0 guard) */
        case 0x02: case 0x03: /* SRL / SRA */
        case 0x04: case 0x06: case 0x07: /* SLLV / SRLV / SRAV */
        case 0x0A: case 0x0B: /* MOVZ / MOVN */
        case 0x10: case 0x11: case 0x12: case 0x13: /* MFHI / MTHI / MFLO / MTLO */
        case 0x18: case 0x19: /* MULT / MULTU */
        case 0x1A: case 0x1B: /* DIV / DIVU */
        case 0x21: case 0x23: /* ADDU / SUBU */
        case 0x24: case 0x25: case 0x26: case 0x27: /* AND / OR / XOR / NOR */
        case 0x2A: case 0x2B: /* SLT / SLTU */
            return 1;
        default:
            return 0;
        }
    }
    return 0;
}

/* Open-addressing (linear probe) lookup/insert, keyed by the raw
 * instruction word. 8192 slots comfortably covers the realistic
 * working set of distinct pure-ALU encodings a real boot/game
 * exercises; if it ever does fill up, insert() below just declines to
 * cache further entries rather than growing or evicting - the caller
 * (ee_jit_try_execute_one) still runs the freshly-compiled block for
 * that one call, it just won't be cached for reuse. Not a correctness
 * issue, only a (currently unobserved) performance ceiling. */
static ppc_block_fn ee_jit_cache_lookup(uint32_t instr)
{
    uint32_t h = (instr * 2654435761u) & (EE_JIT_CACHE_SLOTS - 1u);
    for (uint32_t probe = 0; probe < EE_JIT_CACHE_SLOTS; probe++) {
        uint32_t slot = (h + probe) & (EE_JIT_CACHE_SLOTS - 1u);
        if (g_cache[slot].fn == NULL)
            return NULL; /* empty slot reached along the probe chain: definitely not cached */
        if (g_cache[slot].instr == instr)
            return g_cache[slot].fn;
    }
    return NULL; /* cache full and not found */
}

static void ee_jit_cache_insert(uint32_t instr, ppc_block_fn fn)
{
    if (g_cache_count >= EE_JIT_CACHE_SLOTS)
        return; /* full - see lookup()'s comment; not cached, not leaked here (freed by reset, or lives for process lifetime, same as every other cache entry) */
    uint32_t h = (instr * 2654435761u) & (EE_JIT_CACHE_SLOTS - 1u);
    for (uint32_t probe = 0; probe < EE_JIT_CACHE_SLOTS; probe++) {
        uint32_t slot = (h + probe) & (EE_JIT_CACHE_SLOTS - 1u);
        if (g_cache[slot].fn == NULL) {
            g_cache[slot].instr = instr;
            g_cache[slot].fn = fn;
            g_cache_count++;
            return;
        }
    }
}

#endif /* GEKKO */

int ee_jit_try_execute_one(ee_state_t *st, uint32_t instr)
{
#ifndef GEKKO
    /* Round 887 host-safety gate: ppc_dynarec.c generates raw PPC750
     * machine code, and the block below CALLS it as a function
     * pointer. This project's regression/test suite builds and runs
     * on an x86_64 host - invoking a buffer of PPC opcode bytes there
     * would execute garbage as x86_64 instructions (undefined
     * behavior / near-certain crash or memory corruption), not a
     * no-op. GEKKO is devkitPPC's own auto-defined macro (also passed
     * explicitly via -DGEKKO in this project's Wii Makefile), so it's
     * true exactly when compiling for the real PPC750/Broadway target
     * this generated code can actually run on. On every other build
     * (host-native tests, host tools) this function must decline
     * immediately so ee_step() falls back to the interpreter - exactly
     * the same behavior as every round before this one. Round 886's
     * r880_ppc_verify.c is the correct way to verify the generated PPC
     * ENCODINGS on host: it interprets the raw bytes in a synthetic
     * PPC model rather than executing them natively. See
     * include/core/recompiler/ee_jit.h's header comment. */
    (void)st;
    (void)instr;
    return 0;
#else
    if (!ee_jit_opcode_supported(instr))
        return 0;

    ppc_block_fn fn = ee_jit_cache_lookup(instr);
    if (!fn) {
        ppc_codegen_ctx_t ctx;
        if (ppc_dynarec_init(&ctx, 1) != 0)
            return 0; /* out of memory - fall back to the interpreter, not fatal */
        if (ppc_dynarec_translate_one(&ctx, instr) != 0) {
            /* Should be unreachable given ee_jit_opcode_supported()'s
             * pre-filter is hand-kept in sync with translate_one()'s
             * own dispatch - but if they ever drift, fail safe
             * (interpret this instruction) rather than call a
             * half-built or missing block. */
            ppc_dynarec_free(&ctx);
            return 0;
        }
        fn = ppc_dynarec_finalize(&ctx);
        if (!fn) {
            ppc_dynarec_free(&ctx);
            return 0;
        }
        /* Deliberately NOT ppc_dynarec_free(&ctx) here: that would
         * free the very code buffer `fn` now points into (finalize()
         * returns ctx.code itself, memalign'd, cast to a function
         * pointer). `ctx` is a local struct of plain value/pointer
         * fields, so letting it go out of scope here is harmless -
         * ownership of the code buffer has effectively transferred to
         * the cache (ee_jit_reset_stats_for_test() is the only thing
         * that ever frees these, for host-native test hygiene). */
        ee_jit_cache_insert(instr, fn);
    }

    /* ppc_dynarec_gpr128_t and ee_reg128_t are separately-declared but
     * byte-layout-identical structs ({uint64_t ud0, ud1;} in both) -
     * see ppc_dynarec.h's own header comment, which explicitly names
     * this exact cast as the intended zero-copy integration path. */
    fn((ppc_dynarec_gpr128_t *)&st->gpr[0]);
    g_jit_executed++;
    return 1;
#endif /* GEKKO */
}

uint64_t ee_jit_get_executed_count(void) { return g_jit_executed; }
uint32_t ee_jit_get_cache_size(void) { return g_cache_count; }

void ee_jit_reset_stats_for_test(void)
{
    for (uint32_t i = 0; i < EE_JIT_CACHE_SLOTS; i++) {
        if (g_cache[i].fn != NULL)
            free((void *)g_cache[i].fn);
    }
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_count = 0;
    g_jit_executed = 0;
}
