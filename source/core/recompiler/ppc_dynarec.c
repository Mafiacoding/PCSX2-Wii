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

/* srawi rA, rS, SH - arithmetic shift right by SH, used here purely to
 * produce a 64-bit sign-extension fill word: srawi rA, rS, 31 leaves
 * rA = 0x00000000 if rS's sign bit is clear, or 0xFFFFFFFF if it's
 * set - exactly the MIPS64 sign-extend-32-to-64 rule applied to the
 * high word. Encoding verified bit-for-bit against real devkitPPC
 * (powerpc-eabi-as/-objdump): "srawi r5,r4,31" assembles to
 * 0x7C85FE70, which this formula reproduces exactly. */
static inline uint32_t enc_srawi(int rA, int rS, int sh)
{
    return (31u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | ((uint32_t)sh << 11) | (824u << 1);
}

static inline uint32_t enc_blr(void)
{
    return 0x4E800020u;
}

/* Scratch PPC GPRs used by generated code. r3 is the incoming context
 * pointer (ppc_dynarec_gpr128_t *gpr) per the PowerPC EABI calling
 * convention - we never touch r1 (stack ptr) or r2/r13 (TOC/small-
 * data). r6/r7 are a second scratch pair, needed now that OR has to
 * move both halves of a 64-bit value. */
#define CTX_REG   3
#define SCRATCH_A 4
#define SCRATCH_B 5
#define SCRATCH_C 6
#define SCRATCH_D 7

/* Byte offset of MIPS register `r`'s ppc_dynarec_gpr128_t slot within
 * the context array (16 bytes/slot: 8-byte ud0 + 8-byte ud1 - see the
 * header's endianness note before touching these). REG_HI/REG_LO give
 * the offsets of ud0's two 32-bit halves; REG_HI is the semantically
 * "high 32 bits" word regardless of host byte order because PPC750/
 * Broadway is big-endian, so it sits at the LOWER address. */
#define REG_SLOT(r)  ((int16_t)((r) * 16))
#define REG_HI(r)    ((int16_t)(REG_SLOT(r) + 0))
#define REG_LO(r)    ((int16_t)(REG_SLOT(r) + 4))

int ppc_dynarec_init(ppc_codegen_ctx_t *ctx, size_t max_instructions)
{
    memset(ctx, 0, sizeof(*ctx));

    /* Worst case is OR's 8 PPC instructions per MIPS instruction (see
     * ppc_dynarec_translate_one), plus one trailing blr. */
    size_t words = max_instructions * 8 + 1;
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
    if (ctx->used_words + 8 > ctx->capacity_words)
        return -1; /* out of buffer space */

    uint32_t op    = (mips_instr >> 26) & 0x3F;
    uint32_t rs    = (mips_instr >> 21) & 0x1F;
    uint32_t rt    = (mips_instr >> 16) & 0x1F;
    uint32_t rd    = (mips_instr >> 11) & 0x1F;
    int32_t  imm   = (int16_t)(mips_instr & 0xFFFF);
    uint32_t funct = mips_instr & 0x3F;

    if (op == 0x09) {
        /* MIPS: addiu rt, rs, imm -> gpr[rt] = sign_extend_64(
         * (int32_t)(gpr[rs].lo32 + imm)). ADDIU is a 32-bit-result op:
         * unlike OR below, the result is computed in 32 bits and then
         * sign-extended into the full 64-bit register - it does NOT
         * just OR/copy the upper half through unchanged. Real
         * hardware always discards writes to $zero (rt==0), so skip
         * emitting anything for that case rather than mutate gpr[0]. */
        if (rt == 0)
            return 0;
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm));
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rt)));
        /* Sign-extend: srawi by 31 turns SCRATCH_A's sign bit into an
         * all-0s or all-1s fill word, which IS the correct high half
         * of a 64-bit sign-extension of a 32-bit result. */
        emit(ctx, enc_srawi(SCRATCH_B, SCRATCH_A, 31));
        emit(ctx, enc_stw(SCRATCH_B, CTX_REG, REG_HI(rt)));
        return 0;
    }

    if (op == 0x00 && funct == 0x25) {
        /* MIPS: or rd, rs, rt -> gpr[rd] = gpr[rs] | gpr[rt], as a
         * genuine full 64-bit bitwise OR - unlike ADDIU above, MIPS
         * logical ops (OR/AND/XOR/NOR) never truncate or sign-extend;
         * both halves must be ORed independently. Discard writes to
         * $zero (rd==0) the same way ADDIU does. */
        if (rd == 0)
            return 0;
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(rt)));
        /* PPC 'or' takes (rA=dest, rS=src1, rB=src2) in that field order */
        emit(ctx, enc_or(SCRATCH_A, SCRATCH_A, SCRATCH_B));
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rd)));
        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_HI(rs)));
        emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, REG_HI(rt)));
        emit(ctx, enc_or(SCRATCH_C, SCRATCH_C, SCRATCH_D));
        emit(ctx, enc_stw(SCRATCH_C, CTX_REG, REG_HI(rd)));
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
