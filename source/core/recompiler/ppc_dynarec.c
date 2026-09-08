/*
 * ppc_dynarec.c - see include/core/recompiler/ppc_dynarec.h for the
 * (important) disclaimer about what this proof-of-concept is and is
 * not. Encodings below are standard PowerPC instruction forms.
 */

#include "core/recompiler/ppc_dynarec.h"
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#ifdef GEKKO
#include <ogc/cache.h> /* Round 887b host-safety: only needed by finalize()'s
                         * cache-maintenance calls below, which are themselves
                         * only meaningful (and only compiled) on the real
                         * Wii/devkitPPC target - see ee_jit.c's matching gate
                         * for the full rationale. This keeps every other
                         * function in this file (the encoders, init/free,
                         * translate_one) host-portable, so host-native
                         * verification harnesses can call the REAL
                         * translate_one() directly instead of reimplementing
                         * its logic. */
#endif

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

/* Round 887b (task #866/#868/#869 continuation): addis - same D-form
 * layout as enc_addi above (opcode 15 instead of 14); rA == 0 gives
 * "lis rD, simm" per the PPC ISA definition, which places the raw
 * 16-bit immediate field directly into the upper halfword of rD with
 * the lower halfword zeroed (concatenation, not a shift-of-a-sign-
 * extended-value - the encoding doesn't care whether `simm` is "meant"
 * as signed or unsigned, only its 16 raw bits matter here, which is
 * exactly MIPS LUI's own semantics: gpr[rt] = sext32(uimm << 16)).
 * Verified bit-for-bit against real devkitPPC: "lis r4,0x1234" ->
 * 0x3C801234; "lis r5,-1" -> 0x3CA0FFFF; "lis r6,0x7fff" -> 0x3CC07FFF. */
static inline uint32_t enc_addis(int rD, int rA, int16_t simm)
{
    return (15u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | (uint16_t)simm;
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

/* Round 886 (task #866) additions: subfc/subfe/xoris/andi. - the
 * building blocks for a genuine 64-bit signed/unsigned less-than
 * comparison (SLT/SLTU/SLTI/SLTIU), synthesized from 32-bit PPC750
 * primitives via the standard multi-word subtract-with-borrow idiom
 * (PPC750 has no native 64-bit compare - it's a 32-bit implementation).
 * All four encodings verified bit-for-bit against real devkitPPC
 * (powerpc-eabi-as/-objdump): "subfc r4,r5,r6" -> 0x7C853010,
 * "subfe r4,r5,r6" -> 0x7C853110, "subfe r4,r4,r4" -> 0x7C842110,
 * "xoris r4,r5,0x8000" -> 0x6CA48000, "andi. r4,r5,1" -> 0x70A40001 -
 * all reproduced exactly by the formulas below. */

/* subfc rD, rA, rB -> rD = rB - rA, sets XER.CA = 1 iff NO borrow
 * (i.e. rB >= rA unsigned). Same rD/rA/rB field layout as enc_add. */
static inline uint32_t enc_subfc(int rD, int rA, int rB)
{
    return (31u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (8u << 1);
}

/* subfe rD, rA, rB -> rD = rB - rA + (CA_in - 1) (i.e. subtract with
 * incoming borrow), and updates CA with the new borrow/carry. Used
 * both to propagate the borrow into the high word of a 64-bit
 * subtract (subfe hi, bHi, aHi) and, in the "rD==rA==rB" self-referencing
 * form, as a carry-to-mask trick: subfe rT,rX,rX = ~rX + rX + CA =
 * -1 + CA, which is 0 when CA=1 (no overall borrow) or 0xFFFFFFFF
 * when CA=0 (a borrow occurred) - independent of rX's actual value. */
static inline uint32_t enc_subfe(int rD, int rA, int rB)
{
    return (31u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (136u << 1);
}

/* xoris rA(dest), rS, UIMM -> rA = rS XOR (UIMM << 16). Used with
 * UIMM=0x8000 to flip just bit 31 of a 32-bit high-word, which maps
 * signed comparison range onto unsigned ordering (the standard
 * "add/xor the sign bit" trick for building a signed compare out of
 * an unsigned one). Same "dest is really rA field" layout as enc_or. */
static inline uint32_t enc_xoris(int rA, int rS, uint16_t uimm)
{
    return (27u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | uimm;
}

/* andi. rA(dest), rS, UIMM -> rA = rS AND UIMM (always records CR0,
 * per the real PPC ISA - there is no non-dot "andi"). Used here only
 * to mask a 0/0xFFFFFFFF carry-derived value down to 0/1; CR0 is not
 * read by any code this PoC generates. */
static inline uint32_t enc_andi_dot(int rA, int rS, uint16_t uimm)
{
    return (28u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | uimm;
}

/* Round 888 (task #866/#868/#869 continuation): slw/srw/sraw - X-form
 * variable-shift instructions (same "dest is really rA field, rS at
 * bits6-10 is source, rB at bits16-20 is the shift-amount register"
 * layout as enc_and/enc_or/enc_xor/enc_nor above). Used for BOTH the
 * immediate-shift MIPS ops (SLL/SRL/SRA - the shift amount is
 * materialized into a scratch register with a plain `li` first) and
 * the variable-shift ones (SLLV/SRLV/SRAV), so translate_one only
 * needs one shift-emission path instead of two. PPC's shift
 * instructions only look at the low 5 (slw/srw) or 6 (on 64-bit PPC;
 * PPC750 is 32-bit so effectively 5) bits of rB's shift count and
 * behave exactly like MIPS's 0-31 range here - no >=32 edge case is
 * reachable from any of these six MIPS opcodes (sa is a literal 5-bit
 * field; rs32 & 0x1F is explicitly masked by the interpreter, and this
 * dynarec masks it the same way below), so there's no divergence to
 * worry about between PPC's and MIPS's shift-amount overflow rules.
 * All three encodings verified bit-for-bit against real devkitPPC
 * (powerpc-eabi-as/-objdump): "slw r4,r5,r6" -> 0x7CA43030,
 * "srw r4,r5,r6" -> 0x7CA43430, "sraw r4,r5,r6" -> 0x7CA43630 - all
 * reproduced exactly by the formulas below (note sraw's XO=792 sits
 * right next to srawi's already-verified XO=824 in the standard PPC
 * extended-opcode table, which is a useful cross-check). */
static inline uint32_t enc_slw(int rA, int rS, int rB)
{
    return (31u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (24u << 1);
}

static inline uint32_t enc_srw(int rA, int rS, int rB)
{
    return (31u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (536u << 1);
}

static inline uint32_t enc_sraw(int rA, int rS, int rB)
{
    return (31u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (792u << 1);
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

/* Shared core for SLT/SLTU/SLTI/SLTIU: given the LEFT operand's hi/lo
 * pre-loaded into SCRATCH_C/SCRATCH_A and the RIGHT operand's (a real
 * register for SLT/SLTU, or a synthetic sign-extended-immediate
 * operand for SLTI/SLTIU) hi/lo pre-loaded into SCRATCH_D/SCRATCH_B,
 * emits the multi-word subtract-with-borrow chain (optionally sign-
 * flipping just the two high words first, for a SIGNED comparison)
 * and leaves a clean 0/1 result ("(left < right) ? 1 : 0") in
 * SCRATCH_A. Caller stores SCRATCH_A to REG_LO(dest) and a zero word
 * to REG_HI(dest) - the result is always exactly 0 or 1, so zero-
 * extension and sign-extension of the 64-bit result are identical
 * (see the file note above enc_subfc). 4 PPC instructions for an
 * unsigned compare, 6 for a signed one. */
static void emit(ppc_codegen_ctx_t *ctx, uint32_t instr); /* forward decl - defined below, used here */

static void emit_slt_core(ppc_codegen_ctx_t *ctx, int is_signed)
{
    if (is_signed) {
        /* Flip just the sign bit (bit 31) of both high words: the
         * standard trick that maps two's-complement 64-bit ordering
         * onto unsigned 64-bit ordering, so the SAME unsigned borrow
         * chain below works for a signed comparison too. */
        emit(ctx, enc_xoris(SCRATCH_C, SCRATCH_C, 0x8000));
        emit(ctx, enc_xoris(SCRATCH_D, SCRATCH_D, 0x8000));
    }
    /* left.lo - right.lo (subfc rD,rA,rB computes rD=rB-rA, so the
     * value being subtracted goes in the rA slot: rA=right, rB=left);
     * CA=1 iff left.lo >= right.lo unsigned (no borrow). */
    emit(ctx, enc_subfc(SCRATCH_A, SCRATCH_B, SCRATCH_A));
    /* Propagate the borrow into the high word: CA ends up 1 iff the
     * full 64-bit left >= right unsigned (i.e. NOT(left < right)). */
    emit(ctx, enc_subfe(SCRATCH_C, SCRATCH_D, SCRATCH_C));
    /* Self-subtract trick: ~SCRATCH_A + SCRATCH_A + CA = -1 + CA.
     * CA=1 (left>=right) -> 0x00000000. CA=0 (left<right) -> 0xFFFFFFFF. */
    emit(ctx, enc_subfe(SCRATCH_A, SCRATCH_A, SCRATCH_A));
    /* Mask down to a clean 0/1. */
    emit(ctx, enc_andi_dot(SCRATCH_A, SCRATCH_A, 1));
}

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

    /* Worst case is SLT/SLTI's 13 PPC instructions per MIPS instruction
     * (4 setup loads/li's + 6 for the signed emit_slt_core chain + 3
     * store/li/store - see ppc_dynarec_translate_one; SLTU/SLTIU are
     * cheaper at 11, the 64-bit logical ops OR/AND/XOR/NOR at 8,
     * ADDIU/ADDU/SUBU cheaper still at 5-6), plus one trailing blr. */
    size_t words = max_instructions * 13 + 1;
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
    if (ctx->used_words + 13 > ctx->capacity_words)
        return -1; /* out of buffer space */

    uint32_t op    = (mips_instr >> 26) & 0x3F;
    uint32_t rs    = (mips_instr >> 21) & 0x1F;
    uint32_t rt    = (mips_instr >> 16) & 0x1F;
    uint32_t rd    = (mips_instr >> 11) & 0x1F;
    uint32_t sa    = (mips_instr >> 6) & 0x1F; /* Round 888: shift-amount field, used by SLL/SRL/SRA */
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

    if (op == 0x0F) {
        /* MIPS: lui rt, uimm -> gpr[rt] = sign_extend_64((int32_t)(uimm
         * << 16)). Same 32-bit-compute-then-sign-extend shape as
         * ADDIU above, but the 32-bit result is produced directly by a
         * single "lis" (addis with rA=0) instead of a load+add - see
         * enc_addis's own comment for why the raw-immediate-field
         * semantics line up exactly with MIPS's uimm<<16. Note MIPS
         * treats the immediate as UNSIGNED here (`uimm`, not `imm`),
         * unlike ADDIU's signed imm - but since enc_addis just places
         * the raw 16 bits verbatim (no sign-extension happens in the
         * encoding itself), casting to int16_t for the encoder call is
         * purely a bit-pattern reinterpretation and produces the
         * identical instruction either way. */
        if (rt == 0)
            return 0;
        uint32_t uimm = mips_instr & 0xFFFFu;
        emit(ctx, enc_addis(SCRATCH_A, 0, (int16_t)uimm));
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rt)));
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

    if (op == 0x00 && (funct == 0x2A || funct == 0x2B)) {
        /* MIPS: slt/sltu rd, rs, rt -> rd = (rs < rt) ? 1 : 0, using the
         * FULL 64-bit value of both operands (signed for SLT, unsigned
         * for SLTU) - see emit_slt_core above for the comparison
         * method. The written-back result is always exactly 0 or 1, so
         * unlike ADDIU/ADDU/SUBU there's no sign-vs-zero-extension
         * distinction to make for the result: the high word is simply
         * zero either way. Discard writes to $zero (rd==0). */
        if (rd == 0)
            return 0;
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(rt)));
        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_HI(rs)));
        emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, REG_HI(rt)));
        emit_slt_core(ctx, funct == 0x2A /* SLT is signed, SLTU is not */);
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rd)));
        emit(ctx, enc_addi(SCRATCH_B, 0, 0)); /* li SCRATCH_B, 0 */
        emit(ctx, enc_stw(SCRATCH_B, CTX_REG, REG_HI(rd)));
        return 0;
    }

    if (op == 0x0A || op == 0x0B) {
        /* MIPS: slti/sltiu rt, rs, imm -> rt = (rs < imm) ? 1 : 0.
         * `imm` is ALWAYS sign-extended to a full 64-bit value first -
         * yes, even for the "unsigned" SLTIU: per the real MIPS64 ISA,
         * only the COMPARISON itself is unsigned for SLTIU, the 16-bit
         * immediate's sign-extension happens unconditionally. We build
         * a synthetic "rt operand" (hi/lo pair) from imm the same way
         * ADDIU's sign-extension works (li + srawi-by-31 fill word),
         * then feed it through the same emit_slt_core as SLT/SLTU. */
        if (rt == 0)
            return 0;
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_addi(SCRATCH_B, 0, (int16_t)imm)); /* li SCRATCH_B, imm (synthetic right.lo) */
        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_HI(rs)));
        emit(ctx, enc_srawi(SCRATCH_D, SCRATCH_B, 31)); /* synthetic right.hi = sign fill of imm */
        emit_slt_core(ctx, op == 0x0A /* SLTI is signed, SLTIU is not */);
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rt)));
        emit(ctx, enc_addi(SCRATCH_B, 0, 0)); /* li SCRATCH_B, 0 */
        emit(ctx, enc_stw(SCRATCH_B, CTX_REG, REG_HI(rt)));
        return 0;
    }

    if (op == 0x00 && (funct == 0x00 || funct == 0x02 || funct == 0x03)) {
        /* MIPS: sll/srl/sra rd, rt, sa -> gpr[rd] = sign_extend_64(
         * (int32_t)(shift_op(gpr[rt].lo32, sa))), where sa is the
         * literal 5-bit shift-amount field (bits 6-10 of the
         * instruction, already decoded into the local `sa` variable
         * above). Same 32-bit-compute-then-sign-extend shape as
         * ADDU/SUBU. The shift amount is materialized into a scratch
         * register with a plain `li` so the SAME slw/srw/sraw emission
         * below serves both this immediate form and the *V variable
         * form right after it. rd==0 (which includes the literal
         * all-zero-word NOP encoding, sa==rd==rt==rs==0) is a true
         * no-op on real hardware - decline without emitting anything,
         * consistent with every other opcode above. */
        if (rd == 0)
            return 0;
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rt)));
        emit(ctx, enc_addi(SCRATCH_B, 0, (int16_t)sa)); /* li SCRATCH_B, sa (0-31, fits) */
        if (funct == 0x00) { /* SLL */
            emit(ctx, enc_slw(SCRATCH_A, SCRATCH_A, SCRATCH_B));
        } else if (funct == 0x02) { /* SRL */
            emit(ctx, enc_srw(SCRATCH_A, SCRATCH_A, SCRATCH_B));
        } else { /* funct == 0x03: SRA */
            emit(ctx, enc_sraw(SCRATCH_A, SCRATCH_A, SCRATCH_B));
        }
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rd)));
        emit(ctx, enc_srawi(SCRATCH_C, SCRATCH_A, 31));
        emit(ctx, enc_stw(SCRATCH_C, CTX_REG, REG_HI(rd)));
        return 0;
    }

    if (op == 0x00 && (funct == 0x04 || funct == 0x06 || funct == 0x07)) {
        /* MIPS: sllv/srlv/srav rd, rt, rs -> gpr[rd] = sign_extend_64(
         * (int32_t)(shift_op(gpr[rt].lo32, gpr[rs].lo32 & 0x1F))). Same
         * shape as the immediate sll/srl/sra above, but the shift
         * amount comes from a register (masked to its low 5 bits, per
         * the real MIPS ISA and ee_core.c's own `rs32 & 0x1F`) instead
         * of the sa field. andi. is reused from the SLT/SLTI machinery
         * above purely for its masking effect; CR0 (which andi. also
         * sets, being the only non-dot "andi" PPC has) is never read by
         * any code this dynarec generates. */
        if (rd == 0)
            return 0;
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rt)));
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_andi_dot(SCRATCH_B, SCRATCH_B, 0x1F));
        if (funct == 0x04) { /* SLLV */
            emit(ctx, enc_slw(SCRATCH_A, SCRATCH_A, SCRATCH_B));
        } else if (funct == 0x06) { /* SRLV */
            emit(ctx, enc_srw(SCRATCH_A, SCRATCH_A, SCRATCH_B));
        } else { /* funct == 0x07: SRAV */
            emit(ctx, enc_sraw(SCRATCH_A, SCRATCH_A, SCRATCH_B));
        }
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rd)));
        emit(ctx, enc_srawi(SCRATCH_C, SCRATCH_A, 31));
        emit(ctx, enc_stw(SCRATCH_C, CTX_REG, REG_HI(rd)));
        return 0;
    }

    /* Unsupported: branches, loads/stores, MMI, COP1/2, everything
     * else. Real coverage would require this switch to be the size of
     * ee_core's interpreter (or larger, with scheduling). */
    return -1;
}

ppc_block_fn ppc_dynarec_finalize(ppc_codegen_ctx_t *ctx)
{
    if (ctx->used_words + 1 > ctx->capacity_words)
        return NULL;

    emit(ctx, enc_blr());

    size_t bytes = ctx->used_words * sizeof(uint32_t);
#ifdef GEKKO
    DCFlushRange(ctx->code, bytes);
    ICInvalidateRange(ctx->code, bytes);
#else
    /* Host-native builds: no real PPC750 icache to maintain, and (per
     * ee_jit.c's host-safety gate) the returned function pointer will
     * never actually be called here - only host-native verification
     * harnesses that interpret ctx->code's bytes (never execute them)
     * should ever reach this branch. */
    (void)bytes;
#endif

    return (ppc_block_fn)(void *)ctx->code;
}
