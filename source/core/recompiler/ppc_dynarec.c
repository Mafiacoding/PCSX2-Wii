/*
 * ppc_dynarec.c - see include/core/recompiler/ppc_dynarec.h for the
 * (important) disclaimer about what this proof-of-concept is and is
 * not. Encodings below are standard PowerPC instruction forms.
 */

#include "core/recompiler/ppc_dynarec.h"
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <ogc/cache.h>

/* --- PPC instruction encoders (subset) --- */

static inline uint32_t enc_lwz(int rD, int rA, int16_t d)
{
    return (32u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | (uint16_t)d;
}

static inline uint32_t enc_stw(int rS, int rA, int16_t d)
{
    return (36u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | (uint16_t)d;
}

static inline uint32_t enc_addi(int rD, int rA, int16_t simm)
{
    /* rA == 0 gives "li rD, simm" per the PPC ISA definition */
    return (14u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | (uint16_t)simm;
}

static inline uint32_t enc_or(int rA, int rS, int rB)
{
    return (31u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (444u << 1);
}

static inline uint32_t enc_blr(void)
{
    return 0x4E800020u;
}

/* Scratch PPC GPRs used by generated code. r3 is the incoming context
 * pointer (uint32_t *gpr32) per the PowerPC EABI calling convention -
 * we never touch r1 (stack ptr) or r2/r13 (TOC/small-data). */
#define CTX_REG   3
#define SCRATCH_A 4
#define SCRATCH_B 5

int ppc_dynarec_init(ppc_codegen_ctx_t *ctx, size_t max_instructions)
{
    memset(ctx, 0, sizeof(*ctx));

    /* Worst case ~4 PPC instructions emitted per MIPS instruction,
     * plus one trailing blr. */
    size_t words = max_instructions * 4 + 1;
    ctx->code = memalign(32, words * sizeof(uint32_t));
    if (!ctx->code)
        return -1;

    ctx->capacity_words = words;
    ctx->used_words = 0;
    return 0;
}

void ppc_dynarec_free(ppc_codegen_ctx_t *ctx)
{
    if (ctx->code) {
        free(ctx->code);
        ctx->code = NULL;
    }
}

static void emit(ppc_codegen_ctx_t *ctx, uint32_t instr)
{
    ctx->code[ctx->used_words++] = instr;
}

int ppc_dynarec_translate_one(ppc_codegen_ctx_t *ctx, uint32_t mips_instr)
{
    if (ctx->used_words + 4 > ctx->capacity_words)
        return -1; /* out of buffer space */

    uint32_t op    = (mips_instr >> 26) & 0x3F;
    uint32_t rs    = (mips_instr >> 21) & 0x1F;
    uint32_t rt    = (mips_instr >> 16) & 0x1F;
    uint32_t rd    = (mips_instr >> 11) & 0x1F;
    int32_t  imm   = (int16_t)(mips_instr & 0xFFFF);
    uint32_t funct = mips_instr & 0x3F;

    if (op == 0x09) {
        /* MIPS: addiu rt, rs, imm  ->  gpr[rt] = gpr[rs] + imm */
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, (int16_t)(rs * 4)));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm));
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, (int16_t)(rt * 4)));
        return 0;
    }

    if (op == 0x00 && funct == 0x25) {
        /* MIPS: or rd, rs, rt -> gpr[rd] = gpr[rs] | gpr[rt] */
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, (int16_t)(rs * 4)));
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, (int16_t)(rt * 4)));
        /* PPC 'or' takes (rA=dest, rS=src1, rB=src2) in that field order */
        emit(ctx, enc_or(SCRATCH_A, SCRATCH_A, SCRATCH_B));
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, (int16_t)(rd * 4)));
        return 0;
    }

    /* Unsupported: NOP/SLL-by-zero, branches, loads/stores, MMI, COP1/2,
     * everything else. Real coverage would require this switch to be
     * the size of ee_core's interpreter (or larger, with scheduling). */
    return -1;
}

ppc_block_fn ppc_dynarec_finalize(ppc_codegen_ctx_t *ctx)
{
    if (ctx->used_words + 1 > ctx->capacity_words)
        return NULL;

    emit(ctx, enc_blr());

    size_t bytes = ctx->used_words * sizeof(uint32_t);
    DCFlushRange(ctx->code, bytes);
    ICInvalidateRange(ctx->code, bytes);

    return (ppc_block_fn)(void *)ctx->code;
}
