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

/* Round 881 (task #866) additions: add/subf/and/xor/nor. All five
 * encodings verified bit-for-bit against real devkitPPC
 * (powerpc-eabi-as/-objdump) before use here, same as enc_srawi was -
 * e.g. "add r4,r5,r6" -> 0x7C853214, "subf r4,r5,r6" -> 0x7C853050,
 * "and r4,r5,r6" -> 0x7CA43038, "xor r4,r5,r6" -> 0x7CA43278,
 * "nor r4,r5,r6" -> 0x7CA430F8 - all reproduced exactly by the
 * formulas below. */

/* add rD, rA, rB -> rD = rA + rB. Field layout: rD at bits6-10,
 * rA at bits11-15, rB at bits16-20 (like enc_addi's rD/rA, not the
 * "dest last" pattern enc_or/and/xor/nor use). */
static inline uint32_t enc_add(int rD, int rA, int rB)
{
    return (31u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (266u << 1);
}

/* subf rT, rA, rB -> rT = rB - rA (PPC reverses the intuitive operand
 * order: the SECOND operand is subtracted FROM the third). */
static inline uint32_t enc_subf(int rT, int rA, int rB)
{
    return (31u << 26) | ((uint32_t)rT << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (40u << 1);
}

/* and/xor/nor rA(dest), rS, rB - same "dest first, but destination
 * field is really rA at bits11-15" layout as enc_or. */
static inline uint32_t enc_and(int rA, int rS, int rB)
{
    return (31u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (28u << 1);
}

static inline uint32_t enc_xor(int rA, int rS, int rB)
{
    return (31u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (316u << 1);
}

static inline uint32_t enc_nor(int rA, int rS, int rB)
{
    return (31u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (124u << 1);
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

    /* Worst case is the 64-bit logical ops' (OR/AND/XOR/NOR) 8 PPC
     * instructions per MIPS instruction (see ppc_dynarec_translate_one
     * - ADDIU/ADDU/SUBU are cheaper at 5-6), plus one trailing blr. */
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

    if (op == 0x00 && (funct == 0x25 || funct == 0x24 || funct == 0x26 || funct == 0x27)) {
        /* MIPS: or/and/xor/nor rd, rs, rt -> genuine full 64-bit
         * bitwise ops - unlike ADDU/SUBU below, MIPS logical ops
         * never truncate or sign-extend; both halves must be combined
         * independently. Discard writes to $zero (rd==0). */
        if (rd == 0)
            return 0;
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(rt)));
        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_HI(rs)));
        emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, REG_HI(rt)));
        if (funct == 0x25) { /* OR */
            emit(ctx, enc_or(SCRATCH_A, SCRATCH_A, SCRATCH_B));
            emit(ctx, enc_or(SCRATCH_C, SCRATCH_C, SCRATCH_D));
        } else if (funct == 0x24) { /* AND */
            emit(ctx, enc_and(SCRATCH_A, SCRATCH_A, SCRATCH_B));
            emit(ctx, enc_and(SCRATCH_C, SCRATCH_C, SCRATCH_D));
        } else if (funct == 0x26) { /* XOR */
            emit(ctx, enc_xor(SCRATCH_A, SCRATCH_A, SCRATCH_B));
            emit(ctx, enc_xor(SCRATCH_C, SCRATCH_C, SCRATCH_D));
        } else { /* funct == 0x27: NOR */
            emit(ctx, enc_nor(SCRATCH_A, SCRATCH_A, SCRATCH_B));
            emit(ctx, enc_nor(SCRATCH_C, SCRATCH_C, SCRATCH_D));
        }
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rd)));
        emit(ctx, enc_stw(SCRATCH_C, CTX_REG, REG_HI(rd)));
        return 0;
    }

    if (op == 0x00 && (funct == 0x21 || funct == 0x23)) {
        /* MIPS: addu/subu rd, rs, rt -> gpr[rd] = sign_extend_64(
         * (int32_t)(gpr[rs].lo32 +/- gpr[rt].lo32)). Same 32-bit-
         * compute-then-sign-extend rule as ADDIU above, just with both
         * operands coming from registers instead of an immediate. */
        if (rd == 0)
            return 0;
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(rt)));
        if (funct == 0x21) { /* ADDU */
            emit(ctx, enc_add(SCRATCH_A, SCRATCH_A, SCRATCH_B));
        } else { /* funct == 0x23: SUBU. subf RT,RA,RB computes RB-RA;
                  * we want rs-rt, so RA=SCRATCH_B(rt), RB=SCRATCH_A(rs). */
            emit(ctx, enc_subf(SCRATCH_A, SCRATCH_B, SCRATCH_A));
        }
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rd)));
        emit(ctx, enc_srawi(SCRATCH_C, SCRATCH_A, 31));
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
