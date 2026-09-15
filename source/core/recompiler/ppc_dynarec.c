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

/* Round 897 (task #881): xori rA(dest), rS, UIMM -> rA = rS XOR UIMM
 * (zero-extended, no CR0 side effect - unlike andi., there IS a plain
 * non-dot "xori"). Same D-form layout as ori/xoris above, opcode 26
 * (one less than xoris's 27). Verified bit-for-bit against real
 * devkitPPC: "xori r4,r5,0x1234" -> 0x68A41234, matching this formula
 * exactly. Needed for MIPS's XORI, which - like ORI below - XORs a
 * zero-extended 16-bit immediate into the LOW word only, leaving the
 * high word untouched (XOR with a zero-extended immediate's implicit
 * all-0 upper bits is a no-op on the high word). */
static inline uint32_t enc_xori(int rA, int rS, uint16_t uimm)
{
    return (26u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | uimm;
}

/* Round 898 (task #882) additions: addc/adde - the add-side counterparts
 * of subfc/subfe above, needed to synthesize a genuine 64-bit DADD/DADDU
 * out of two 32-bit halves (PPC750 has no native 64-bit add, same
 * constraint that made subfc/subfe necessary for SLT/SLTU/DSUB). addc
 * rD,rA,rB -> rD = rA + rB, sets XER.CA = 1 iff the unsigned 32-bit add
 * overflowed (a carry OUT of bit 0). adde rD,rA,rB -> rD = rA + rB +
 * CA_in, and updates CA with the new carry - chaining addc(lo halves)
 * into adde(hi halves) is the standard multi-word add idiom, the direct
 * mirror of subfc/subfe's multi-word subtract idiom already in use here.
 * Both encodings verified bit-for-bit against real devkitPPC
 * (powerpc-eabi-as/-objdump): "addc r4,r5,r6" -> 0x7C853014, "adde
 * r4,r5,r6" -> 0x7C853114 - reproduced exactly by the formulas below
 * (XO=10 for addc, XO=138 for adde - the well-known PPC ISA constants,
 * each exactly 2 more than their subf-family counterpart: subfc=8/
 * addc=10, subfe=136/adde=138, matching the pattern subfc/subfe already
 * established in this file). */
static inline uint32_t enc_addc(int rD, int rA, int rB)
{
    return (31u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (10u << 1);
}

static inline uint32_t enc_adde(int rD, int rA, int rB)
{
    return (31u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (138u << 1);
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

/* Round 890 (task #874) additions: mullw/mulhw/mulhwu/divw/divwu - the
 * 32-bit multiply/divide primitives needed for MULT/MULTU/DIV/DIVU.
 * All five share the same "dest first" rD/rA/rB field layout as
 * enc_add/enc_subf (NOT the "dest is really rA" layout enc_and/or/xor/
 * nor/slw/srw/sraw use). Verified bit-for-bit against real devkitPPC
 * (powerpc-eabi-as/-objdump): "mullw r4,r5,r6" -> 0x7C8531D6,
 * "mulhw r4,r5,r6" -> 0x7C853096, "mulhwu r4,r5,r6" -> 0x7C853016,
 * "divw r4,r5,r6" -> 0x7C8533D6, "divwu r4,r5,r6" -> 0x7C853396 - all
 * reproduced exactly by the formulas below.
 *
 * mullw gives the LOW 32 bits of the 32x32 product - identical
 * regardless of operand signedness (the low half of a two's-complement
 * product never depends on how the operands' sign bits are
 * interpreted), so MULT and MULTU share the same mullw call and only
 * differ in which high-word instruction they use.
 * mulhw/mulhwu give the HIGH 32 bits of the signed/unsigned 32x32
 * product respectively - together with mullw this reproduces a full
 * 64-bit widening multiply from two PPC750 32-bit-only instructions,
 * matching MIPS MULT/MULTU's 32x32->64 semantics exactly.
 * divw/divwu give ONLY the 32-bit quotient (PPC has no combined
 * quotient+remainder instruction); the remainder is computed
 * separately as `rs32 - quotient*rt32` via mullw+subf (both already
 * available above) - see the DIV/DIVU dispatch block below. Per the
 * PPC ISA, a zero divisor does not trap by default (OE=0 here) and
 * simply leaves the destination register "undefined" (implementation-
 * defined bit pattern, but never a fault) - safe to execute
 * unconditionally and discard the result via the same branch-free mask
 * trick MOVZ/MOVN (Round 889) established, since MIPS DIV/DIVU's own
 * real semantics leave HI/LO completely UNCHANGED when rt32==0 (see
 * ee_core.c's `if (rt32 != 0) { ... }` guard on both cases). */
static inline uint32_t enc_mullw(int rD, int rA, int rB)
{
    return (31u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (235u << 1);
}

static inline uint32_t enc_mulhw(int rD, int rA, int rB)
{
    return (31u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (75u << 1);
}

static inline uint32_t enc_mulhwu(int rD, int rA, int rB)
{
    return (31u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (11u << 1);
}

static inline uint32_t enc_divw(int rD, int rA, int rB)
{
    return (31u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (491u << 1);
}

static inline uint32_t enc_divwu(int rD, int rA, int rB)
{
    return (31u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | ((uint32_t)rB << 11) | (459u << 1);
}

/* Round 891 (task #875): call-emission primitives for LW/SW - the first
 * opcodes this dynarec has ever needed to invoke a real C function
 * (ee_mem_read32/ee_mem_write32) rather than just moving bits between
 * the context array and PPC registers. All five encodings verified
 * bit-for-bit against real devkitPPC (powerpc-eabi-as/-objdump):
 * "ori r4,r5,0x1234" -> 0x60A51234, "mflr r14" -> 0x7DC802A6,
 * "mtlr r14" -> 0x7DC803A6, "mtctr r12" -> 0x7D8903A6,
 * "bctrl" -> 0x4E800421 - all reproduced exactly by the formulas below. */

/* ori rA(dest), rS, UIMM -> rA = rS | UIMM (no CR0 update, unlike
 * andi. - there's a plain non-dot "ori", the immediate-logical odd one
 * out from andi./xoris above). Used with a preceding `lis` (enc_addis
 * with rA=0) to materialize an arbitrary 32-bit constant: lis places
 * the upper 16 bits with no sign-extension surprises (see enc_addis's
 * own comment), and ori ALSO doesn't sign-extend its immediate, so
 * `lis rD,hi16; ori rD,rD,lo16` reconstructs any 32-bit value exactly
 * via plain bitwise OR - no lis-value +1 correction is ever needed
 * (unlike the classic "lis+addi" idiom, which does need that
 * correction whenever the low half's bit 15 is set, because addi's
 * immediate IS sign-extended). Same "dest is really rA field" layout
 * as enc_or/enc_and/enc_xor/enc_nor. */
static inline uint32_t enc_ori(int rA, int rS, uint16_t uimm)
{
    return (24u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | uimm;
}

/* mflr rD -> rD = LR (XFX-form "mfspr rD, LR"; LR's SPR number is 8,
 * encoded as two 5-bit halves with the LOW 5 bits in bits 20-16 and the
 * HIGH 5 bits in bits 15-11 - for SPR 8 that's low5=8, high5=0, so only
 * the bits16-20 field is ever nonzero for this particular SPR). Used to
 * save this generated block's own incoming return address before this
 * block's own internal call (bctrl, below) overwrites LR with an
 * address inside this same block. */
static inline uint32_t enc_mflr(int rD)
{
    return (31u << 26) | ((uint32_t)rD << 21) | (8u << 16) | (339u << 1);
}

/* mtlr rS -> LR = rS (XFX-form "mtspr LR, rS", same SPR=8 field layout
 * as enc_mflr above but XO=467 and the register field is a SOURCE, not
 * a destination). Used to restore the saved return address into LR
 * right after this block's internal call returns, so this block's own
 * trailing `blr` (emitted by ppc_dynarec_finalize) returns to the REAL
 * caller (ee_jit_try_execute_one) rather than to the address bctrl
 * itself left in LR. */
static inline uint32_t enc_mtlr(int rS)
{
    return (31u << 26) | ((uint32_t)rS << 21) | (8u << 16) | (467u << 1);
}

/* mtctr rS -> CTR = rS (XFX-form "mtspr CTR, rS"; CTR's SPR number is 9,
 * same low5/high5 split as LR above - low5=9, high5=0). Used to load
 * the absolute address of the real C function this block is about to
 * call (materialized via lis+ori, above) into CTR immediately before
 * branching to it. */
static inline uint32_t enc_mtctr(int rS)
{
    return (31u << 26) | ((uint32_t)rS << 21) | (9u << 16) | (467u << 1);
}

/* bctrl -> branch to CTR, set LR = address of the instruction right
 * after this one (XL-form, BO=20 "branch always", BI=0 unused, LK=1).
 * This is this dynarec's first-ever emission of an actual function
 * call - every opcode through Round 890 was pure straight-line
 * register/context manipulation with no calls at all. */
static inline uint32_t enc_bctrl(void)
{
    return (19u << 26) | (20u << 21) | (528u << 1) | 1u;
}

/* Round 892 (task #876): extsb/extsh - X-form sign-extend instructions
 * (same "dest is really rA field, rS at bits6-10 is source, no rB"
 * layout as enc_and/enc_or/enc_xor/enc_nor above, just with an unused
 * rB field instead of a real one). Used to turn LB/LH's freshly-loaded
 * byte/halfword (sitting in the low 8/16 bits of r3 after the
 * ee_mem_read8/16 call, with the upper bits of r3 left unspecified by
 * the PowerPC EABI's sub-word-return-value rules - the ABI only
 * guarantees the CALLEE's own use of the value is correct, not that
 * unused high bits of the caller-visible register are zero) into a
 * properly sign-extended 32-bit value BEFORE this file's usual "store
 * 32-bit result, then srawi by 31 for the 64-bit sign-extension fill
 * word" idiom (already used by ADDIU/ADDU/SUBU/LW/etc.) is applied -
 * that idiom assumes its input is already a correct 32-bit value,
 * which extsb/extsh is what produces from a byte/halfword. Verified
 * bit-for-bit against real devkitPPC (powerpc-eabi-as/-objdump):
 * "extsb r4,r5" -> 0x7CA40774, "extsh r4,r5" -> 0x7CA40734 - both
 * reproduced exactly by the formulas below. */
static inline uint32_t enc_extsb(int rA, int rS)
{
    return (31u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | (954u << 1);
}

static inline uint32_t enc_extsh(int rA, int rS)
{
    return (31u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | (922u << 1);
}

/* Round 894 (task #878): stb rS, d(rA) - same D-form layout as enc_stw
 * (opcode 38 instead of 36), needed because branch_pending is a
 * uint8_t field in ee_state_t - a plain enc_stw there would clobber
 * the 3 adjacent bytes. Verified bit-for-bit against real devkitPPC:
 * "stb r5,684(r3)" -> 0x98A302AC, reproduced exactly by the formula
 * below. */
static inline uint32_t enc_stb(int rS, int rA, int16_t d)
{
    return (38u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | (uint16_t)d;
}

/* Round 895 (task #879): lbz rD, d(rA) - same D-form layout as enc_lwz,
 * opcode 34 (the standard PowerPC load-family numbering: 32=lwz,
 * 34=lbz, 36=stw, 38=stb - enc_lwz/enc_stb above already use the other
 * two). Needed because the conditional-branch blend technique below
 * reads the CURRENT `branch_pending` byte (to preserve it unchanged on
 * the not-taken path) - every opcode before this round only ever WROTE
 * branch_pending unconditionally (J/JAL/JR/JALR are always "taken", so
 * they never needed to read the old value). Verified bit-for-bit
 * against real devkitPPC (powerpc-eabi-as/-objdump): "lbz r5,684(r3)"
 * -> 0x88A302AC (identical to enc_stb's own verified "stb r5,684(r3)"
 * -> 0x98A302AC encoding, except opcode 34 vs 38 - 0x88 vs 0x98 in the
 * top byte is exactly that 4-bit opcode difference shifted into place),
 * reproduced exactly by the formula below. */
static inline uint32_t enc_lbz(int rD, int rA, int16_t d)
{
    return (34u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | (uint16_t)d;
}

/* Round 914 (task #899): lhz/sth rD/rS, d(rA) - the halfword-width
 * members of the same D-form load/store family as enc_lwz/enc_lbz
 * (opcode 32) and enc_stw/enc_stb (opcode 36): standard PowerPC
 * numbering is 32=lwz, 34=lbz, 40=lhz, 36=stw, 38=stb, 44=sth. Needed
 * for MMI's 16-bit-lane SIMD family (PADDH/PSUBH first, this round) -
 * lhz zero-extends the loaded halfword into a 32-bit GPR (matching
 * this dynarec's existing lbz usage, which also zero-extends), and
 * sth truncates a 32-bit GPR down to its low 16 bits on write - exactly
 * the semantics needed to reproduce ee_core.c's set_lane_h()'s own
 * `(uint16_t)(...)` truncating cast without any extra masking
 * instruction. */
static inline uint32_t enc_lhz(int rD, int rA, int16_t d)
{
    return (40u << 26) | ((uint32_t)rD << 21) | ((uint32_t)rA << 16) | (uint16_t)d;
}

static inline uint32_t enc_sth(int rS, int rA, int16_t d)
{
    return (44u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) | (uint16_t)d;
}

/* Round 894 (task #878): rlwinm rA, rS, SH, MB, ME (M-form) - rotate
 * left rS by SH bits then mask to the contiguous bit range [MB, ME]
 * (PPC bit numbering, MSB=0). Only used here as "rA = rS & 0xF0000000"
 * (SH=0, MB=0, ME=3) to extract J/JAL's real-hardware "keep the top 4
 * bits of the CURRENT instruction's own address" behavior - this_pc is
 * read from context at runtime (not baked into the generated code), so
 * the same cached block stays correct even though J/JAL's absolute
 * target genuinely depends on WHERE the instruction sits in memory,
 * not just its raw encoding (see the op==0x02 dispatch block's own
 * comment for why this is still safe under this dynarec's instruction-
 * encoding-keyed cache). Verified bit-for-bit against real devkitPPC:
 * "rlwinm r4,r5,0,0,3" -> 0x54A40006, reproduced exactly by the
 * formula below. */
static inline uint32_t enc_rlwinm(int rA, int rS, int sh, int mb, int me)
{
    return (21u << 26) | ((uint32_t)rS << 21) | ((uint32_t)rA << 16) |
           ((uint32_t)sh << 11) | ((uint32_t)mb << 6) | ((uint32_t)me << 1);
}

/* Round 903 (task #884) additions: lfs/stfs/fadds/fsubs/fmuls/fdivs -
 * this dynarec's FIRST real PPC750 floating-point instructions, needed
 * for COP1.S's genuine arithmetic family (ADD.S/SUB.S/MUL.S/DIV.S).
 * lfs/stfs are D-form, same rD/rA/d layout as enc_lwz/enc_stw above but
 * addressing the FPR file (opcodes 48/52) - PPC has no GPR<->FPR move
 * instruction, so every bit-pattern handoff between the two register
 * files in this dynarec goes through memory (stw+lfs or stfs+lwz),
 * exactly like the emit_fpu_clamp32() spill/fill sequence below.
 * fadds/fsubs/fdivs are A-form with frB as the second source operand;
 * fmuls is the one arithmetic exception - it uses frC (bits 6-10)
 * instead of frB, per the real PPC ISA's multiply-specific field
 * layout. All six encodings verified bit-for-bit against real devkitPPC
 * (powerpc-eabi-as/-objdump): "lfs f0,24(r1)" -> 0xC0010018, "stfs
 * f0,24(r1)" -> 0xD0010018, "fadds f0,f1,f2" -> 0xEC01102A, "fsubs
 * f3,f4,f5" -> 0xEC642828, "fmuls f6,f7,f8" -> 0xECC70232, "fdivs
 * f9,f10,f11" -> 0xED2A5824 - all reproduced exactly by the formulas
 * below. */
static inline uint32_t enc_lfs(int frD, int rA, int16_t d)
{
    return (48u << 26) | ((uint32_t)frD << 21) | ((uint32_t)rA << 16) | (uint16_t)d;
}

static inline uint32_t enc_stfs(int frS, int rA, int16_t d)
{
    return (52u << 26) | ((uint32_t)frS << 21) | ((uint32_t)rA << 16) | (uint16_t)d;
}

static inline uint32_t enc_fadds(int frD, int frA, int frB)
{
    return (59u << 26) | ((uint32_t)frD << 21) | ((uint32_t)frA << 16) | ((uint32_t)frB << 11) | (21u << 1);
}

static inline uint32_t enc_fsubs(int frD, int frA, int frB)
{
    return (59u << 26) | ((uint32_t)frD << 21) | ((uint32_t)frA << 16) | ((uint32_t)frB << 11) | (20u << 1);
}

static inline uint32_t enc_fmuls(int frD, int frA, int frC)
{
    return (59u << 26) | ((uint32_t)frD << 21) | ((uint32_t)frA << 16) | ((uint32_t)frC << 6) | (25u << 1);
}

/* Round 908 (task #893): fsel frD,frA,frC,frB - "frD = (frA >= 0.0) ?
 * frC : frB", opcode 63 (the double-precision-shaped A-form space,
 * same family fdivs/fabs/etc live in - NOT opcode 59's single-precision
 * space), xo=23. Used to build VU0 VMAX/VMINI's real IEEE compare-
 * select without a branch: diff=a-b, then fsel(diff, pick_a, pick_b).
 * Verified against real devkitPPC output before use: "fsel f1,f2,f3,f4"
 * -> 0xFC2220EE, "fsel f2,f0,f1,f3" -> 0xFC40186E - both reproduced
 * exactly by the formula below. */
static inline uint32_t enc_fsel(int frD, int frA, int frC, int frB)
{
    return (63u << 26) | ((uint32_t)frD << 21) | ((uint32_t)frA << 16) | ((uint32_t)frB << 11) | ((uint32_t)frC << 6) | (23u << 1);
}

/* Round 906 (task #890): fcmpu/mfcr - this dynarec's first use of real
 * PPC750 CR-based comparison, needed for C.EQ.S/C.LT.S/C.LE.S. Real
 * hardware float compare (fcmpu) correctly handles the -0.0==+0.0 edge
 * case and can't produce a NaN-unordered result here (this dynarec's
 * clamp routine already collapses every NaN/Infinity input to a signed
 * Fmax before either operand reaches fcmpu), so it's a safer and
 * simpler choice than trying to hand-roll an integer bit-pattern
 * ordering trick (which would need special-casing the -0.0 vs +0.0
 * boundary to avoid a wrong C.EQ.S result - real hardware just handles
 * it correctly for free). fcmpu sets crfD's 4 bits to (FL,FG,FE,FU) -
 * always cr0 here (crfD=0) since this dynarec has no use for any other
 * CR field. mfcr copies the whole 32-bit CR into a GPR, whose top
 * nibble (bits 31/30/29/28 in normal C shift terms) is then exactly
 * cr0's FL/FG/FE/FU - extracted branchlessly afterward via
 * enc_rlwinm's rotate-then-mask-to-bit0 idiom (rotate left by i+1,
 * mask [31,31]), no real PPC branch needed. Both encodings verified
 * bit-for-bit against real devkitPPC (powerpc-eabi-as/-objdump):
 * "fcmpu cr0,f0,f1" -> 0xFC000800, "fcmpu cr1,f2,f3" -> 0xFC821800,
 * "mfcr r4" -> 0x7C800026, "mfcr r10" -> 0x7D400026 - all reproduced
 * exactly by the formulas below. */
static inline uint32_t enc_fcmpu(int crfD, int frA, int frB)
{
    return (63u << 26) | ((uint32_t)crfD << 23) | ((uint32_t)frA << 16) | ((uint32_t)frB << 11);
}

static inline uint32_t enc_mfcr(int rD)
{
    return (31u << 26) | ((uint32_t)rD << 21) | (19u << 1);
}

static inline uint32_t enc_fdivs(int frD, int frA, int frB)
{
    return (59u << 26) | ((uint32_t)frD << 21) | ((uint32_t)frA << 16) | ((uint32_t)frB << 11) | (18u << 1);
}

/* Round 906b (task #891): lfd/stfd/fctiwz - needed for CVT.W.S, this
 * dynarec's first opcode that does a genuine float->int CONVERSION
 * (every prior FPU opcode stayed within float<->float, or moved raw
 * 32-bit bit patterns without touching the FPU at all). fctiwz is part
 * of the base PowerPC ISA (Book I) and IS present on Gekko/Broadway -
 * unlike fsqrts/fsqrt (Round 905's finding), float/int conversion is
 * not one of the operations Gekko's FPU omits. fctiwz converts the
 * double-precision value in frB to a 32-bit integer using round-
 * toward-zero, storing the result in the LOW-order 32 bits of frD (the
 * high-order 32 bits are undefined per the ISA) - so the result has to
 * be spilled to memory with stfd and read back as a plain 32-bit word,
 * there's no direct FPR-to-GPR move instruction. lfd loads a raw
 * double-precision value (used here to load fpr[fs], which lfs already
 * auto-promotes single->double on load, exactly like every fs/ft load
 * elsewhere in this file). All three encodings verified bit-for-bit
 * against real devkitPPC (powerpc-eabi-as/-objdump): "lfd f0,8(r1)" ->
 * 0xC8010008, "stfd f1,16(r1)" -> 0xD8210010, "fctiwz f2,f3" ->
 * 0xFC40181E, "fctiwz f0,f0" -> 0xFC00001E - all reproduced exactly by
 * the formulas below. */
static inline uint32_t enc_lfd(int frD, int rA, int16_t d)
{
    return (50u << 26) | ((uint32_t)frD << 21) | ((uint32_t)rA << 16) | (uint16_t)d;
}

static inline uint32_t enc_stfd(int frS, int rA, int16_t d)
{
    return (54u << 26) | ((uint32_t)frS << 21) | ((uint32_t)rA << 16) | (uint16_t)d;
}

static inline uint32_t enc_fctiwz(int frD, int frB)
{
    return (63u << 26) | ((uint32_t)frD << 21) | ((uint32_t)frB << 11) | (15u << 1);
}

/* Round 894 (task #878): byte offsets of the three ee_state_t fields
 * J/JAL/JR/JALR need beyond the gpr[32]+hi+lo+pc/next_pc/sa_reg/cop0[32]
 * region this file's REG_HI/REG_LO addressing already covers - CTX_REG
 * (r3) is exactly `&st->gpr[0]`, which is also byte offset 0 of the
 * WHOLE ee_state_t struct (see ee_jit.c's _Static_assert on
 * offsetof(ee_state_t, gpr) == 0), so any ee_state_t field is reachable
 * from generated code as a plain lwz/stw/stb at its real offset - no
 * new addressing mechanism needed, just new constants:
 *
 *   - EXC_THIS_PC_OFFSET: `exc_this_pc`, the CURRENT instruction's own
 *     address, published by ee_step() before ee_jit_try_execute_one()
 *     is ever called (see that call site's comment in ee_core.c). This
 *     is what lets J/JAL/branch targets be computed correctly even
 *     though this dynarec's cache is keyed by instruction ENCODING
 *     (not address) - the immediate/index field bits are baked into
 *     the generated code at compile time (safe: they're part of the
 *     encoding), but this_pc itself is read from context at every
 *     execution (safe: it varies correctly per call site even when the
 *     same cached block runs for the same encoding at a different
 *     address).
 *   - NEXT_PC_OFFSET: `next_pc` - by the time this dynarec's code runs,
 *     ee_step() has already set this to the correct NON-branch
 *     fallthrough (this_pc+8); J/JAL/JR/JALR must overwrite it with the
 *     real jump target, exactly like the interpreter's own BRANCH_TO()
 *     macro does.
 *   - BRANCH_PENDING_OFFSET: `branch_pending` (uint8_t) - must be set
 *     to 1 for every one of these four opcodes (all unconditional,
 *     always "taken"), marking that the NEXT instruction executes in a
 *     branch-delay slot, exactly like BRANCH_TO()'s call sites do.
 *
 * These three raw offsets are asserted against real offsetof(ee_state_t,
 * ...) values in ee_jit.c, right next to the existing HI_IDX/LO_IDX
 * _Static_assert block - if ee_core.h's struct layout ever changes,
 * that fires a compile error instead of silently corrupting control
 * flow the next time J/JAL/JR/JALR gets JIT-compiled.
 *
 * Round 896 (task #880) update: PC_OFFSET (`pc`, 544) joins this list -
 * the "likely" branch family (BEQL/BNEL/BLEZL/BGTZL/BLTZL/BGEZL) is the
 * first opcode category that needs to write `pc` directly rather than
 * only `next_pc`. Real MIPS II+ "likely" semantics NULLIFY the delay
 * slot when the branch isn't taken - ee_core.c's own interpreter
 * (search for "Likely" in ee_core.c) does this by directly overwriting
 * BOTH `st->pc = fallthrough_pc + 4` (= this_pc + 8, skipping the delay
 * slot instruction's normal fetch address entirely) AND `st->next_pc =
 * fallthrough_pc + 8` (= this_pc + 12) on the not-taken path, WITHOUT
 * going through BRANCH_TO()/branch_pending at all - see
 * emit_branch_blend_likely()'s own comment for how this dynarec
 * reproduces that exactly. */
#define EXC_THIS_PC_OFFSET     ((int16_t)1456)
#define NEXT_PC_OFFSET         ((int16_t)548)
#define BRANCH_PENDING_OFFSET  ((int16_t)684)
#define PC_OFFSET              ((int16_t)544)

/* Round 891 (task #875): absolute addresses of the two real EE
 * memory-access entry points LW/SW need to call. Declared here with a
 * `void *` state-pointer parameter instead of pulling in
 * "core/ee/ee_core.h" for ee_state_t - a pointer's calling-convention
 * representation never depends on its pointee type, so this preserves
 * this file's established narrow contract with the wider codebase (see
 * the file's own top comment and HI_IDX/LO_IDX's comment above): it
 * only needs these two functions' names, parameter types, and calling
 * convention, never ee_state_t's actual layout (which ee_jit.c's
 * _Static_asserts already enforce separately, for the context-array
 * contract these two functions' first parameter shares). The real
 * symbols live in ee_core.c and are linked in unconditionally on the
 * real Wii build (see the project Makefile's SOURCES list).
 *
 * On host-native (non-GEKKO) builds, taking a REAL function pointer's
 * address would be meaningless here: x86_64 host addresses are 64
 * bits, but lis+ori (above) only ever builds a 32-bit constant -
 * correctly, since the real Wii target's entire address space is 32
 * bits - so a truncated host pointer would be garbage, not a
 * usable-but-wrong address. Host builds never execute this generated
 * code as real machine code anyway (see ee_jit.c's GEKKO gate) - only
 * host verify harnesses that INTERPRET (never execute) the generated
 * bytes with a synthetic PPC750 simulator ever reach translate_one()
 * here, so this file instead embeds small fixed sentinel constants
 * such a harness's simulator can recognize by exact value (matching
 * CTR right before a simulated bctrl) and route to its own local test-
 * double implementation - no real address of any kind is needed on
 * that path. */
#ifdef GEKKO
extern uint32_t ee_mem_read32(void *st, uint32_t addr);
extern void     ee_mem_write32(void *st, uint32_t addr, uint32_t val);
#define ADDR_EE_MEM_READ32  ((uint32_t)(uintptr_t)&ee_mem_read32)
#define ADDR_EE_MEM_WRITE32 ((uint32_t)(uintptr_t)&ee_mem_write32)
#else
#define ADDR_EE_MEM_READ32  0x00000101u
#define ADDR_EE_MEM_WRITE32 0x00000102u
#endif

/* Round 892 (task #876): same GEKKO-vs-host dual-address scheme as
 * ADDR_EE_MEM_READ32/WRITE32 immediately above, extended to the
 * byte/halfword memory-access entry points LB/LBU/LH/LHU/SB/SH need.
 * LWU deliberately has NO separate address here - ee_core.c's own LWU
 * case body (op 0x27) calls the exact same ee_mem_read32() LW already
 * uses (see that case's own comment in ee_core.c); LWU and LW only
 * differ in what they do with the 32-bit result afterward (zero- vs
 * sign-extend into the 64-bit destination), never in which C function
 * gets called - so LWU's dispatch block below reuses
 * ADDR_EE_MEM_READ32 directly. Sentinel values 0x103-0x106 are chosen
 * to be distinct from 0x101/0x102 (already claimed by READ32/WRITE32)
 * and from each other; a host verify harness's simulator matches
 * these exactly against CTR right before a simulated bctrl, same
 * mechanism as Round 891. */
#ifdef GEKKO
extern uint8_t  ee_mem_read8(void *st, uint32_t addr);
extern uint16_t ee_mem_read16(void *st, uint32_t addr);
extern void     ee_mem_write8(void *st, uint32_t addr, uint8_t val);
extern void     ee_mem_write16(void *st, uint32_t addr, uint16_t val);
#define ADDR_EE_MEM_READ8   ((uint32_t)(uintptr_t)&ee_mem_read8)
#define ADDR_EE_MEM_READ16  ((uint32_t)(uintptr_t)&ee_mem_read16)
#define ADDR_EE_MEM_WRITE8  ((uint32_t)(uintptr_t)&ee_mem_write8)
#define ADDR_EE_MEM_WRITE16 ((uint32_t)(uintptr_t)&ee_mem_write16)
#else
#define ADDR_EE_MEM_READ8   0x00000103u
#define ADDR_EE_MEM_READ16  0x00000104u
#define ADDR_EE_MEM_WRITE8  0x00000105u
#define ADDR_EE_MEM_WRITE16 0x00000106u
#endif

/* Round 893 (task #877): LD/SD - the first opcodes whose CALLEE
 * itself deals in a genuine 64-bit value, rather than this dynarec
 * having to synthesize one from two 32-bit calls. Per the PowerPC
 * 32-bit EABI, a `uint64_t` return value comes back split across a
 * register PAIR - r3 holding the high 32 bits, r4 the low 32 bits
 * (the same "high word in the lower-numbered register" convention
 * this file's own REG_HI/REG_LO context-slot layout already uses,
 * which is what makes LD's dispatch block below simpler than LW's:
 * no sign-extension step is needed at all, just two stores straight
 * from r3/r4 into REG_HI(rt)/REG_LO(rt)). A `uint64_t` PARAMETER
 * follows the mirror-image rule: it occupies an aligned register
 * pair, and since `ee_mem_write64(ee_state_t*, uint32_t, uint64_t)`'s
 * first two (32-bit-sized) parameters consume exactly 2 argument
 * words before `val`, the pair falls on r5:r6 with no padding
 * register needed (word-count 2 is already even) - r5 = val's high
 * 32 bits, r6 = val's low 32 bits. */
#ifdef GEKKO
extern uint64_t ee_mem_read64(void *st, uint32_t addr);
extern void     ee_mem_write64(void *st, uint32_t addr, uint64_t val);
#define ADDR_EE_MEM_READ64  ((uint32_t)(uintptr_t)&ee_mem_read64)
#define ADDR_EE_MEM_WRITE64 ((uint32_t)(uintptr_t)&ee_mem_write64)
#else
#define ADDR_EE_MEM_READ64  0x00000107u
#define ADDR_EE_MEM_WRITE64 0x00000108u
#endif

/* Round 905 (task #889): same GEKKO-vs-host dual-address scheme as
 * ADDR_EE_MEM_READ32/etc above, but for the first call trampoline in
 * this file that DOESN'T target one of this project's own ee_mem_*
 * functions - it targets the real linked libm sqrtf(). Empirically
 * verified this round (not from documentation - the fetched IBM Gekko
 * manual PDF's extracted text had zero hits for "fsqrt") that emitting
 * PPC750 fsqrts/fsqrt directly would be unsafe: devkitPPC's own GCC
 * refuses to inline hardware sqrt for -mcpu=750 (compiling
 * `sqrtf(x)` emits a tail-call `b sqrtf`, not an fsqrts instruction),
 * and the real linked libm.a's __ieee754_sqrtf is a from-scratch
 * software bit-twiddling algorithm with zero use of any hardware sqrt
 * opcode anywhere in its body. So SQRT.S/RSQRT.S below call the real
 * library sqrtf() through this trampoline instead of emitting fsqrts,
 * mirroring the ee_mem_* call convention but with a float argument/
 * return in f1 (the EABI's first float arg/return register) rather
 * than integer registers - see the SQRT.S dispatch block for the
 * call-site details. Sentinel 0x109 continues the existing 0x101-0x108
 * numbering (next free value). */
#ifdef GEKKO
extern float sqrtf(float x);
#define ADDR_EE_SQRTF ((uint32_t)(uintptr_t)&sqrtf)
#else
#define ADDR_EE_SQRTF 0x00000109u
#endif

/* Round 906b (task #891): CVT.S.W (int32 -> float) needs a genuine
 * int->float CONVERSION, and real PPC750/Gekko hardware has no
 * instruction for that direction at all - fcfid ("floating convert
 * from integer doubleword") wasn't added to the PowerPC ISA until
 * v2.01 (first shipped in POWER4/PPC970), and Gekko/Broadway is a G3
 * derivative predating that extension entirely (this is a documented
 * ISA-generation fact, not something that needed empirical rediscovery
 * the way SQRT.S's hardware-sqrt gap did). The traditional workaround
 * on such chips is a fragile double-precision bit-manipulation trick
 * (bias with a magic exponent, subtract back out) - deliberately NOT
 * used here, since this dynarec already has a proven, safe pattern for
 * exactly this situation (SQRT.S/RSQRT.S above): call a real C
 * function through the established trampoline instead of hand-rolling
 * float bit tricks. ee_jit_cvt_s_w_helper() below is a trivial one-line
 * wrapper around the exact same `(float)(int32_t)x` cast ee_core.c's
 * own interpreter uses for this opcode, so the JIT and interpreter are
 * GUARANTEED to agree bit-for-bit (both go through the C compiler's own
 * int->float conversion, not two independently-written implementations
 * that could subtly disagree on rounding). Unlike SQRT.S's trampoline
 * (float arg/return, both in f1), this one takes an INTEGER argument in
 * r3 and returns a float in f1 - standard EABI convention assigns
 * argument registers independently per type (integer args to r3-r10,
 * float args to f1-f8), so this "mixed" signature needs no special
 * handling beyond what SQRT.S's own trampoline already established for
 * saving ctx (r15)/LR (r14) across the call. Sentinel 0x10A continues
 * the existing 0x101-0x109 numbering. */
#ifdef GEKKO
static float ee_jit_cvt_s_w_helper(int32_t v)
{
    return (float)v;
}
#define ADDR_EE_CVT_S_W ((uint32_t)(uintptr_t)&ee_jit_cvt_s_w_helper)
#else
#define ADDR_EE_CVT_S_W 0x0000010Au
#endif

/* Round 915 (task #900): MMI2 multiply/divide family - PMULTW/PDIVW/
 * PMULTH/PDIVBW (opcode 0x1C funct 0x09, sa 0x0C/0x0D/0x1C/0x1D). Same
 * GEKKO-vs-host dual-address scheme as ADDR_EE_MEM_READ32/etc above,
 * but the four real implementations (ee_jit_helper_pmultw/pdivw/
 * pmulth/pdivbw) live in ee_core.c rather than this file - unlike
 * ADDR_EE_CVT_S_W's trivial one-line int->float cast, these opcodes
 * need real access to ee_state_t's gpr/hi/lo fields plus the file-
 * private lane_w/lane_h/set_lane_w static-inline helpers that already
 * live in ee_core.c (see that file's own Round 915 comment for the
 * full rationale and the exact ported case-body source). Declared
 * `void *st` here (not `ee_state_t *st`) since this file never needs
 * ee_state_t's full definition - only ee_core.c's actual function
 * bodies dereference it - exactly the same forward-declaration
 * convention ee_mem_read32/etc already use above. Sentinels 0x10B-
 * 0x10E continue the existing 0x101-0x10A numbering (next 4 free
 * values). */
#ifdef GEKKO
extern void ee_jit_helper_pmultw(void *st, int rs, int rt, int rd);
extern void ee_jit_helper_pdivw(void *st, int rs, int rt, int rd);
extern void ee_jit_helper_pmulth(void *st, int rs, int rt, int rd);
extern void ee_jit_helper_pdivbw(void *st, int rs, int rt, int rd);
#define ADDR_EE_JIT_PMULTW ((uint32_t)(uintptr_t)&ee_jit_helper_pmultw)
#define ADDR_EE_JIT_PDIVW  ((uint32_t)(uintptr_t)&ee_jit_helper_pdivw)
#define ADDR_EE_JIT_PMULTH ((uint32_t)(uintptr_t)&ee_jit_helper_pmulth)
#define ADDR_EE_JIT_PDIVBW ((uint32_t)(uintptr_t)&ee_jit_helper_pdivbw)
#else
#define ADDR_EE_JIT_PMULTW 0x0000010Bu
#define ADDR_EE_JIT_PDIVW  0x0000010Cu
#define ADDR_EE_JIT_PMULTH 0x0000010Du
#define ADDR_EE_JIT_PDIVBW 0x0000010Eu
#endif

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
/* Round 890 (task #874) additions: MULT/DIV need more live values at
 * once than the 4-register budget above comfortably allows (quotient,
 * remainder, mask, notmask, plus operands - see the DIV/DIVU block
 * below), so this round adds four more scratch registers. r8-r11 are
 * still ordinary EABI volatile (caller-saved) GPRs, same class as
 * r4-r7 - this file still never touches r1 (stack ptr) or r2/r13
 * (TOC/small-data), per the original scratch-register comment above. */
#define SCRATCH_E 8
#define SCRATCH_F 9
#define SCRATCH_G 10
#define SCRATCH_H 11

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

/* Round 891 (task #875): materializes an arbitrary 32-bit constant into
 * `reg` via lis+ori (see enc_ori's comment for why no lis-value
 * correction is needed with this particular pairing). Used only to
 * load the absolute address of the real C function LW/SW are about to
 * call into r12 immediately before mtctr+bctrl. */
static void emit_load_const32(ppc_codegen_ctx_t *ctx, int reg, uint32_t val)
{
    emit(ctx, enc_addis(reg, 0, (int16_t)(val >> 16)));
    emit(ctx, enc_ori(reg, reg, (uint16_t)(val & 0xFFFFu)));
}

/* Round 903 (task #884): clamps SCRATCH_C's raw float32 bit pattern
 * in place, per PCSX2's real fpu_double()/fpu_check_overflow()/
 * fpu_check_underflow() semantics (ee_core.c lines ~3144-3168):
 * denormals collapse to signed zero (a no-op for exact zero, since
 * its bit pattern already IS "sign, rest 0"), infinities/NaNs
 * (exponent byte == 0xFF, i.e. magnitude >= 0x7F800000) collapse to
 * signed Fmax (0x7F7FFFFF | sign). This ONE routine covers BOTH the
 * input-clamp (fpu_double() is applied to every FPR operand before
 * arithmetic) and the output-clamp (fpu_check_overflow() then
 * fpu_check_underflow() applied to every arithmetic result) cases
 * used by the COP1.S ADD.S/SUB.S/MUL.S dispatch below - the two
 * checks are the same "denormal-or-zero -> signed zero,
 * infinity-or-NaN -> signed Fmax" transform on the exact same two
 * magnitude thresholds, just described differently in ee_core.c's own
 * separate helper functions.
 *
 * Requires the caller to have pre-loaded SCRATCH_A = 0x007FFFFF and
 * SCRATCH_B = 0x7F7FFFFF (the two magnitude thresholds, hoisted once
 * per opcode rather than re-loaded on every clamp call since they're
 * invariant across the fs/ft/result clamp calls a single ADD.S/SUB.S/
 * MUL.S makes). SCRATCH_D/E/F/G/H are used as scratch and left
 * clobbered on return - nothing in this dynarec's calling convention
 * needs them preserved across a single compiled block.
 *
 * Branchless throughout (three-way blend via mask-and-OR), matching
 * this whole file's established style of never emitting real PPC
 * control flow inside a compiled block - see BEQ/BNE's own comment
 * above for the same rationale. The "allOnes iff CA==0/CA==1" mask
 * derivations below reuse the exact subfc/subfe carry-to-mask idiom
 * already established for SLT/SLTU and the branch-condition masks. */
static void emit_fpu_clamp32(ppc_codegen_ctx_t *ctx)
{
    emit(ctx, enc_rlwinm(SCRATCH_D, SCRATCH_C, 0, 1, 31)); /* mag = bits & 0x7FFFFFFF */
    emit(ctx, enc_rlwinm(SCRATCH_E, SCRATCH_C, 0, 0, 0));  /* sign = bits & 0x80000000 */

    /* zero_mask: allOnes iff mag <= 0x007FFFFF (denormal-or-exact-zero) */
    emit(ctx, enc_subfc(SCRATCH_F, SCRATCH_D, SCRATCH_A)); /* F=K1M1-mag, CA=1 iff mag<=K1M1 */
    emit(ctx, enc_subfe(SCRATCH_F, SCRATCH_F, SCRATCH_F)); /* F=allOnes iff CA==0 (mag>K1M1) */
    emit(ctx, enc_nor(SCRATCH_F, SCRATCH_F, SCRATCH_F));   /* F=zero_mask (flip to mag<=K1M1) */

    /* ff_mask: allOnes iff mag > 0x7F7FFFFF (infinity-or-NaN); CA==0
     * already lands on the wanted polarity here, no flip needed. */
    emit(ctx, enc_subfc(SCRATCH_G, SCRATCH_D, SCRATCH_B)); /* G=K2M1-mag, CA=1 iff mag<=K2M1 */
    emit(ctx, enc_subfe(SCRATCH_G, SCRATCH_G, SCRATCH_G)); /* G=ff_mask=allOnes iff mag>K2M1 */

    emit(ctx, enc_or(SCRATCH_H, SCRATCH_B, SCRATCH_E));    /* H=fmax_val = K2M1 | sign */

    emit(ctx, enc_or(SCRATCH_D, SCRATCH_F, SCRATCH_G));    /* D=zero_mask|ff_mask (mag dead) */
    emit(ctx, enc_nor(SCRATCH_D, SCRATCH_D, SCRATCH_D));   /* D=normal_mask */

    emit(ctx, enc_and(SCRATCH_C, SCRATCH_C, SCRATCH_D));   /* bits & normal_mask */
    emit(ctx, enc_and(SCRATCH_E, SCRATCH_E, SCRATCH_F));   /* zero_val(sign) & zero_mask */
    emit(ctx, enc_and(SCRATCH_H, SCRATCH_H, SCRATCH_G));   /* fmax_val & ff_mask */
    emit(ctx, enc_or(SCRATCH_C, SCRATCH_C, SCRATCH_E));
    emit(ctx, enc_or(SCRATCH_C, SCRATCH_C, SCRATCH_H));    /* SCRATCH_C = final clamped bits */
}

/* Round 895 (task #879): given a "taken" mask already computed into
 * SCRATCH_E (0xFFFFFFFF if the branch condition holds, else 0x00000000)
 * and this instruction's compile-time PC-relative displacement (4 +
 * the sign-extended 16-bit offset field * 4 - this is NOT address-
 * dependent, unlike this_pc itself, so it's safe to bake in directly
 * via emit_load_const32, same as J/JAL's low-26-bits-of-target above),
 * computes the branch target from EXC_THIS_PC_OFFSET at runtime and
 * blends it into NEXT_PC_OFFSET/BRANCH_PENDING_OFFSET using the same
 * all-0s/all-1s-mask technique MOVZ/MOVN already use above for
 * conditional register writes - no real PPC branch instruction is
 * emitted, so every generated block stays a single straight-line run
 * with no internal control flow, consistent with this whole file's
 * existing style. On the not-taken path this harmlessly re-stores
 * next_pc/branch_pending's OWN existing values (already the correct
 * fallthrough, per ee_step()'s pre-JIT-call bookkeeping - see
 * NEXT_PC_OFFSET's comment) - a self-store with no effect, exactly
 * mirroring how MOVZ/MOVN unconditionally re-store rd's own old value
 * when their condition doesn't hold. */
static void emit_branch_blend(ppc_codegen_ctx_t *ctx, int32_t disp)
{
    emit(ctx, enc_lwz(SCRATCH_G, CTX_REG, EXC_THIS_PC_OFFSET));
    emit_load_const32(ctx, SCRATCH_A, (uint32_t)disp);
    emit(ctx, enc_add(SCRATCH_G, SCRATCH_G, SCRATCH_A)); /* SCRATCH_G = target */

    emit(ctx, enc_nor(SCRATCH_F, SCRATCH_E, SCRATCH_E)); /* SCRATCH_F = notmask */
    emit(ctx, enc_lwz(SCRATCH_H, CTX_REG, NEXT_PC_OFFSET)); /* old next_pc */
    emit(ctx, enc_and(SCRATCH_G, SCRATCH_G, SCRATCH_E));   /* target & mask */
    emit(ctx, enc_and(SCRATCH_H, SCRATCH_H, SCRATCH_F));   /* old & notmask */
    emit(ctx, enc_or(SCRATCH_G, SCRATCH_G, SCRATCH_H));
    emit(ctx, enc_stw(SCRATCH_G, CTX_REG, NEXT_PC_OFFSET));

    emit(ctx, enc_lbz(SCRATCH_H, CTX_REG, BRANCH_PENDING_OFFSET)); /* old bp byte */
    emit(ctx, enc_addi(SCRATCH_A, 0, 1));                 /* li SCRATCH_A, 1 */
    emit(ctx, enc_and(SCRATCH_A, SCRATCH_A, SCRATCH_E));  /* 1 & mask */
    emit(ctx, enc_and(SCRATCH_H, SCRATCH_H, SCRATCH_F));  /* old_bp & notmask */
    emit(ctx, enc_or(SCRATCH_A, SCRATCH_A, SCRATCH_H));
    emit(ctx, enc_stb(SCRATCH_A, CTX_REG, BRANCH_PENDING_OFFSET));
}

/* Round 896 (task #880): the "Likely" counterpart of emit_branch_blend()
 * above. Given a "taken" mask already computed into SCRATCH_E and this
 * instruction's compile-time PC-relative displacement, reproduces
 * ee_core.c's own Likely-branch semantics exactly:
 *
 *   taken:     next_pc = this_pc + disp; branch_pending = 1 (same as a
 *              regular branch - the delay slot executes normally, then
 *              control transfers to the target).
 *   not taken: pc = this_pc + 8; next_pc = this_pc + 12; branch_pending
 *              is left UNTOUCHED (not set to 0 explicitly - it's
 *              already 0 coming in, same invariant every other not-
 *              taken branch in this file relies on). This NULLIFIES the
 *              delay slot: the instruction physically sitting at
 *              this_pc+4 is skipped entirely rather than executed, by
 *              jumping pc directly past it instead of through the
 *              normal fallthrough+delay-slot path.
 *
 * Both pc and next_pc are computed via the same all-0s/all-1s mask
 * blend as emit_branch_blend() - no real PPC branch instruction, no
 * internal control flow, matching this whole file's architecture.
 * branch_pending's blend is identical to the non-Likely helper's (write
 * 1 when taken, otherwise re-store its own old value unchanged). */
static void emit_branch_blend_likely(ppc_codegen_ctx_t *ctx, int32_t disp)
{
    emit(ctx, enc_lwz(SCRATCH_G, CTX_REG, EXC_THIS_PC_OFFSET)); /* this_pc */
    emit(ctx, enc_addi(SCRATCH_H, SCRATCH_G, 8));  /* skip_pc = this_pc + 8 */
    emit(ctx, enc_addi(SCRATCH_A, SCRATCH_G, 12)); /* skip_next_pc = this_pc + 12 */
    emit_load_const32(ctx, SCRATCH_B, (uint32_t)disp);
    emit(ctx, enc_add(SCRATCH_G, SCRATCH_G, SCRATCH_B)); /* SCRATCH_G = target = this_pc + disp */

    emit(ctx, enc_nor(SCRATCH_F, SCRATCH_E, SCRATCH_E)); /* SCRATCH_F = notmask */

    /* pc = (old_pc & mask) | (skip_pc & notmask) */
    emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, PC_OFFSET)); /* old pc (== fallthrough_pc == this_pc+4) */
    emit(ctx, enc_and(SCRATCH_B, SCRATCH_B, SCRATCH_E));
    emit(ctx, enc_and(SCRATCH_H, SCRATCH_H, SCRATCH_F));
    emit(ctx, enc_or(SCRATCH_B, SCRATCH_B, SCRATCH_H));
    emit(ctx, enc_stw(SCRATCH_B, CTX_REG, PC_OFFSET));

    /* next_pc = (target & mask) | (skip_next_pc & notmask) */
    emit(ctx, enc_and(SCRATCH_G, SCRATCH_G, SCRATCH_E));
    emit(ctx, enc_and(SCRATCH_A, SCRATCH_A, SCRATCH_F));
    emit(ctx, enc_or(SCRATCH_G, SCRATCH_G, SCRATCH_A));
    emit(ctx, enc_stw(SCRATCH_G, CTX_REG, NEXT_PC_OFFSET));

    /* branch_pending = (1 & mask) | (old_bp & notmask) - identical blend
     * to the non-Likely helper. */
    emit(ctx, enc_lbz(SCRATCH_H, CTX_REG, BRANCH_PENDING_OFFSET));
    emit(ctx, enc_addi(SCRATCH_A, 0, 1));
    emit(ctx, enc_and(SCRATCH_A, SCRATCH_A, SCRATCH_E));
    emit(ctx, enc_and(SCRATCH_H, SCRATCH_H, SCRATCH_F));
    emit(ctx, enc_or(SCRATCH_A, SCRATCH_A, SCRATCH_H));
    emit(ctx, enc_stb(SCRATCH_A, CTX_REG, BRANCH_PENDING_OFFSET));
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
/* Round 900 (task #883): REG_HI1/REG_LO1 give the offsets of ud1's two
 * 32-bit halves (bytes 8-11/12-15 of the 16-byte slot) - the upper 64
 * bits of the EE's real 128-bit GPRs, matching ee_core.c's own
 * GPR1(x)==st->gpr[x].ud1 macro. Only LQ/SQ (this round) touch ud1;
 * every opcode before this round only ever reads/writes ud0 via REG_HI/
 * REG_LO. Same big-endian "high word at the lower address" convention
 * as REG_HI/REG_LO - see this header's own endianness note. */
#define REG_HI1(r)   ((int16_t)(REG_SLOT(r) + 8))
#define REG_LO1(r)   ((int16_t)(REG_SLOT(r) + 12))

/* Round 914 (task #899): per-lane byte-offset helpers for MMI's SIMD
 * opcode family, which treats each 128-bit GPR as 4x32-bit / 8x16-bit /
 * 16x8-bit lanes (ee_core.c's own lane_w/lane_h/lane_b + set_lane_w/
 * set_lane_h/set_lane_b helpers, ~line 3195). These do a bit-shift
 * extraction on the plain uint64_t ud0/ud1 fields (e.g. lane_w(r,n) =
 * (uint32_t)((n<2?r.ud0:r.ud1) >> ((n&1)*32))), which is host-endianness-
 * independent AT THE VALUE LEVEL - but this dynarec must reproduce the
 * exact BYTE ADDRESS a real big-endian PPC750/Broadway memory read at
 * that offset would hit, so the mapping isn't simply "lane order ==
 * address order". Worked out from REG_HI/REG_LO/REG_HI1/REG_LO1's own
 * established meaning (REG_HI(r) = high 32 bits of ud0, etc.):
 *   w-lane 0 = REG_LO(r)   (ud0's low 32 bits = lane_w's n=0 term)
 *   w-lane 1 = REG_HI(r)   (ud0's high 32 bits = n=1, ud0>>32)
 *   w-lane 2 = REG_LO1(r)  (ud1's low 32 bits = n=2, ud1>>0)
 *   w-lane 3 = REG_HI1(r)  (ud1's high 32 bits = n=3, ud1>>32)
 * Each of those 4-byte words further splits into 2 halfwords / 4 bytes
 * by big-endian sub-addressing (most-significant sub-field at the
 * LOWEST address within the word) - e.g. h-lane 0 (ud0 bits 0-15, the
 * LOW half of the LOW word) sits at the HIGH two bytes of REG_LO(r),
 * i.e. byte offset REG_LO(r)+2; h-lane 1 (ud0 bits 16-31, the HIGH half
 * of the low word) sits at REG_LO(r)+0. Every one of the 4/8/16 cases
 * below was hand-derived the same way and cross-checked against the
 * bit-shift formulas directly (see this round's STATUS.md writeup for
 * the full worked derivation) - these are NOT guessed from a pattern,
 * every lane's offset traces back to a specific bit-range of a specific
 * ud0/ud1 half. reg/lane are always compile-time-constant instruction
 * fields when called from codegen, so these compute pure compile-time
 * displacement constants - zero runtime cost. */
static int16_t mmi_w_off(int reg, int lane)
{
    switch (lane & 3) {
    case 0:  return REG_LO(reg);
    case 1:  return REG_HI(reg);
    case 2:  return REG_LO1(reg);
    default: return REG_HI1(reg);
    }
}

static int16_t mmi_h_off(int reg, int lane)
{
    switch (lane & 7) {
    case 0:  return (int16_t)(REG_LO(reg) + 2);
    case 1:  return REG_LO(reg);
    case 2:  return (int16_t)(REG_HI(reg) + 2);
    case 3:  return REG_HI(reg);
    case 4:  return (int16_t)(REG_LO1(reg) + 2);
    case 5:  return REG_LO1(reg);
    case 6:  return (int16_t)(REG_HI1(reg) + 2);
    default: return REG_HI1(reg);
    }
}

static int16_t mmi_b_off(int reg, int lane)
{
    int l = lane & 15;
    int word_sel = l >> 2;           /* 0=LO,1=HI,2=LO1,3=HI1 */
    int byte_in_word = 3 - (l & 3);  /* big-endian: sub-lane 0 -> highest byte address */
    int16_t base;
    switch (word_sel) {
    case 0:  base = REG_LO(reg); break;
    case 1:  base = REG_HI(reg); break;
    case 2:  base = REG_LO1(reg); break;
    default: base = REG_HI1(reg); break;
    }
    return (int16_t)(base + byte_in_word);
}

/* Round 902 (task #884): byte offsets of ee_state_t's COP1 (FPU) fields,
 * reached from CTX_REG the same "any ee_state_t field is just a plain
 * lwz/stw at its real offset from &gpr[0]" way as EXC_THIS_PC_OFFSET/
 * NEXT_PC_OFFSET/etc. above (see that comment block). fpr[32] sits
 * right after cop0[32]/branch_pending/tlb[48]/the exception-scratch
 * bytes, at offset 1464 (verified via offsetof(ee_state_t, fpr) against
 * the real struct, not hand-counted) - REG_FPR(f) = 1464 + f*4, well
 * within lwz/stw's 16-bit signed displacement range. fcr31 and acc
 * immediately follow fpr[32] (32*4 = 128 bytes later). These three
 * constants are asserted against real offsetof(...) values in ee_jit.c,
 * same discipline as the other *_OFFSET constants. */
#define REG_FPR(f)      ((int16_t)(1464 + (f) * 4))
#define FCR31_OFFSET    ((int16_t)1592)
#define ACC_OFFSET      ((int16_t)1596)

/* Round 907 (task #892): byte offset of ee_state_t's VU0 macro-mode
 * vector register file (vu0_vf[32][4], uint32_t raw bit patterns - same
 * "reinterpret the bits as float, no clamping" convention ee_core.c's
 * own VADD/VSUB/VMUL case body uses, unlike COP1's fpr[]/REG_FPR() above
 * which DOES clamp). Verified via offsetof(ee_state_t, vu0_vf) == 1728,
 * asserted against the real struct in ee_jit.c (same discipline as
 * REG_FPR/FCR31_OFFSET/ACC_OFFSET). Lane order x=0/y=1/z=2/w=3 matches
 * vu0_vf_write_lane()'s own indexing. Max offset (reg=31,lane=3) is
 * 1728+31*16+12=2236, comfortably within lwz/stw/lfs/stfs's 16-bit
 * signed displacement range. */
#define VU0_VF_OFF(reg, lane)  ((int16_t)(1728 + (reg) * 16 + (lane) * 4))

/* Round 908 (task #893): byte offsets of ee_state_t's VU0 accumulator
 * (vu0_acc[4], flat uint32_t array - NOT the same field as COP1's single-
 * float ACC_OFFSET=1596 above, a totally different register on real
 * hardware) and the VU0 control-register file (cop2_ctrl[32], which
 * vu0_vi_read()/vu0_vi_write() in ee_core.c index directly by register
 * number - VI0 is hardwired like MIPS r0, everything else including the
 * CLIP flag register at index 18 is a plain slot). Both verified via
 * offsetof() against the real ee_state_t struct (vu0_acc=10464,
 * cop2_ctrl=1600) before use here, same discipline as every other
 * offset macro in this file. VCLIP (Round 908) always reads/writes
 * control register 18 - that's a fixed part of the opcode's own real
 * semantics (REG_CLIP_FLAG in PCSX2's VU.h), not a field extracted from
 * the instruction encoding, so no reg==0 runtime guard is ever needed
 * for it (18 is a compile-time constant, never 0). */
#define VU0_ACC_OFF(lane)      ((int16_t)(10464 + (lane) * 4))
#define COP2_CTRL_OFF(idx)     ((int16_t)(1600 + (idx) * 4))

/* Round 890 (task #874): HI/LO pseudo-register indices. The R5900 has
 * two dedicated 64-bit registers (HI, LO - used by MULT/MULTU/DIV/
 * DIVU/MFHI/MTHI/MFLO/MTLO) that this dynarec's context array didn't
 * previously have anywhere to put: `ppc_dynarec_gpr128_t gpr[32]` only
 * ever modeled the 32 real MIPS GPRs. Rather than widening the
 * ppc_block_fn signature to take a second pointer (which would touch
 * every existing opcode block and the ee_jit.c call site), HI and LO
 * are addressed as if they were simply MIPS registers 32 and 33 in the
 * SAME flat array the real GPRs live in - REG_SLOT/REG_HI/REG_LO above
 * already generalize to any integer index with no changes needed, so
 * `REG_HI(HI_IDX)`/`REG_LO(HI_IDX)` just fall out of the existing
 * macros. This is only valid because `ee_state_t` (ee_core.h) was
 * reordered in this same round to place `hi, lo` immediately after
 * `gpr[32]` in memory - see that struct's own comment, and see
 * ee_jit.c's `_Static_assert`s next to the `fn(...)` call site, which
 * are what actually enforce this contract at compile time (this file
 * deliberately still doesn't #include ee_core.h, to keep its only real
 * dependency on the wider codebase as narrow as it's always been: "a
 * flat array of 16-byte register slots"). */
#define HI_IDX 32
#define LO_IDX 33

int ppc_dynarec_init(ppc_codegen_ctx_t *ctx, size_t max_instructions)
{
    memset(ctx, 0, sizeof(*ctx));

    /* Round 890 (task #874) update: DIV/DIVU is now the worst case at
     * 32 PPC instructions per MIPS instruction (2 setup loads + divw/
     * divwu + mullw + subf for the raw quotient/remainder + 4 for the
     * mask/notmask computation + 2 srawi sign-fills + 2x(2 loads + 2
     * ands + 1 or + 1 store) for the LO blend + 2x(2 loads + 2 ands + 1
     * or + 1 store) for the HI blend - see ppc_dynarec_translate_one's
     * DIV/DIVU block; verified by direct emit() count, not just this
     * prose tally). MULT/MULTU is cheaper at up to 13 (11 unconditional
     * + 2 more if rd!=0); MFHI/MTHI/MFLO/MTLO cheapest of this round's
     * additions at 4. Previously MOVN's 20 PPC instructions (Round 889)
     * were the worst case; SLT/SLTI's 13, SLTU/SLTIU's 11, the 64-bit
     * logical ops OR/AND/XOR/NOR's 8, and ADDIU/ADDU/SUBU's 5-6 remain
     * comfortably under this round's new ceiling), plus one trailing
     * blr.
     *
     * Round 891 (task #875) update: LW/SW (the first opcodes that call
     * a real C function - see the ADDR_EE_MEM_READ32/WRITE32 comment
     * above) add a small stack-frame prologue/epilogue and call
     * sequence around that. LW's worst case (rt!=0) is 18 instructions,
     * SW's fixed case is 13 - both still comfortably under DIV/DIVU's
     * 32-instruction ceiling above, so no change to `words` was needed
     * this round.
     *
     * Round 892 (task #876) update: LB/LBU/LH/LHU add one widening
     * instruction (extsb/extsh/andi.) on top of LW's shape, for a worst
     * case of 19; LWU matches LW's shape exactly (18); SB/SH match SW's
     * shape exactly (13). All still comfortably under DIV/DIVU's
     * 32-instruction ceiling, so again no change to `words` was needed.
     *
     * Round 893 (task #877) update: LD is actually CHEAPER than LW at
     * 17 instructions (no sign-extension step needed - see
     * ADDR_EE_MEM_READ64's comment); SD is 14 (one more than SW, for
     * the extra high-word load of its 64-bit value). Both still well
     * under the 32-instruction ceiling.
     *
     * Round 895 (task #879) update: BEQ/BNE (8-9 setup instructions,
     * shared with emit_branch_blend's fixed 15-instruction tail) top
     * out at 24; BLEZ/BGTZ (8-9 setup instructions + the same 15-
     * instruction tail) top out at 24 as well. Both still comfortably
     * under DIV/DIVU's 32-instruction ceiling, so again no change to
     * `words` was needed this round.
     *
     * Round 896 (task #880) update: the "likely" branches (BLTZL/BGEZL/
     * BEQL/BNEL/BLEZL/BGTZL) need emit_branch_blend_likely() instead of
     * emit_branch_blend() - 22 instructions instead of 15, because the
     * not-taken path has to blend THREE fields (pc/next_pc/branch_pending)
     * instead of two. BNEL is the new worst case at 33 instructions total
     * (11-instruction mask computation + the 22-instruction helper body),
     * which EXCEEDS the previous 32-instruction ceiling. Bumped to 40 for
     * headroom.
     *
     * Round 903 (task #884) update: ADD.S/SUB.S/MUL.S each emit ~61
     * instructions (2x 15-instruction emit_fpu_clamp32() calls for the
     * operands + a 3rd for the result, plus the stack-frame/spill/fill
     * plumbing around them - see that dispatch block's own comment).
     * Round 904: bumped to 128 - DIV.S's divide-by-zero special case
     * (mask/blend on top of the same 3x emit_fpu_clamp32 pattern) comes
     * to 81 words, and the MADD/MSUB ACC-register family (Round 906) is
     * expected to need a comparable amount, so this jumps straight to
     * 128 rather than another narrow bump. */
    size_t words = max_instructions * 128 + 1;
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
    if (ctx->used_words + 128 > ctx->capacity_words) /* Round 904: was 80, DIV.S's ~81-word worst case (Round 903 was 61) */
        return -1; /* out of buffer space */

    uint32_t op    = (mips_instr >> 26) & 0x3F;
    uint32_t rs    = (mips_instr >> 21) & 0x1F;
    uint32_t rt    = (mips_instr >> 16) & 0x1F;
    uint32_t rd    = (mips_instr >> 11) & 0x1F;
    uint32_t sa    = (mips_instr >> 6) & 0x1F; /* Round 888: shift-amount field, used by SLL/SRL/SRA */
    int32_t  imm   = (int16_t)(mips_instr & 0xFFFF);
    uint32_t funct = mips_instr & 0x3F;

    if (op == 0x08 || op == 0x09) {
        /* MIPS: addi/addiu rt, rs, imm -> gpr[rt] = sign_extend_64(
         * (int32_t)(gpr[rs].lo32 + imm)). ADDIU is a 32-bit-result op:
         * unlike OR below, the result is computed in 32 bits and then
         * sign-extended into the full 64-bit register - it does NOT
         * just OR/copy the upper half through unchanged. Real
         * hardware always discards writes to $zero (rt==0), so skip
         * emitting anything for that case rather than mutate gpr[0].
         *
         * Round 897 (task #881) update: ADDI (op 0x08) joins ADDIU here
         * unchanged - real MIPS ADDI traps on signed 32-bit overflow,
         * but this project's own interpreter (ee_core.c's ADDI/ADDIU
         * case, see that file's own comment) deliberately does NOT
         * implement that trap (documented simplification, matches the
         * DADDI/DADDIU pair's identical choice one case below it) - so
         * ADDI and ADDIU are byte-for-byte identical from this dynarec's
         * perspective, same as they already are in the interpreter. */
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

    if (op == 0x0C || op == 0x0D || op == 0x0E) {
        /* Round 897 (task #881): MIPS andi/ori/xori rt, rs, uimm ->
         * genuine full 64-bit bitwise ops against a ZERO-extended
         * (never sign-extended) 16-bit immediate - confirmed against
         * ee_core.c's own case bodies: `GPR(rt) = GPR(rs) & (uint64_t)
         * uimm` etc., where uimm is the raw unsigned 16-bit field. Same
         * "no truncation, combine both halves independently" rule as
         * the register-register AND/OR/XOR/NOR block above, but here
         * the immediate's implicit upper 48 bits are always exactly
         * zero, which lets the high-word combine collapse to something
         * simpler than a general AND/OR/XOR of two registers:
         *   ANDI: hi_result = hi_rs & 0            = always 0
         *   ORI:  hi_result = hi_rs | 0            = hi_rs unchanged
         *   XORI: hi_result = hi_rs ^ 0            = hi_rs unchanged
         * ANDI additionally uses andi. directly on the loaded low word
         * (Round 886's encoder, already zero-extends its UIMM exactly
         * like real MIPS ANDI needs) rather than materializing uimm
         * into a second register first - one instruction cheaper than
         * ORI/XORI, which need enc_ori/enc_xori's rS-then-OR/XOR shape
         * since there's no single-instruction PPC form that ORs/XORs a
         * register directly against an immediate INTO A DIFFERENT
         * register while also being usable as the low-word compute
         * (ori/xori's rA=dest, rS=source, UIMM=immediate layout already
         * IS that single instruction - andi. is the odd one out only
         * because it also sets CR0, forcing the "." dot form). Discard
         * writes to $zero (rt==0). */
        if (rt == 0)
            return 0;
        uint32_t uimm = mips_instr & 0xFFFFu;
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        if (op == 0x0C) { /* ANDI */
            emit(ctx, enc_andi_dot(SCRATCH_A, SCRATCH_A, (uint16_t)uimm));
            emit(ctx, enc_addi(SCRATCH_B, 0, 0)); /* li SCRATCH_B, 0 - hi always 0 */
        } else {
            emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_HI(rs)));
            if (op == 0x0D) { /* ORI */
                emit(ctx, enc_ori(SCRATCH_A, SCRATCH_A, (uint16_t)uimm));
            } else { /* op == 0x0E: XORI */
                emit(ctx, enc_xori(SCRATCH_A, SCRATCH_A, (uint16_t)uimm));
            }
            /* hi word (SCRATCH_B) already holds hi_rs unchanged - ORI/
             * XORI with a zero-extended immediate never touches it. */
        }
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rt)));
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

    if (op == 0x00 && (funct == 0x2C || funct == 0x2D || funct == 0x2E || funct == 0x2F)) {
        /* Round 898 (task #882): dadd/daddu/dsub/dsubu rd, rs, rt -> a
         * genuine FULL 64-bit add/subtract, no 32-bit-then-sign-extend
         * shortcut available here (unlike ADDU/SUBU above) - this is the
         * EE's native 64-bit register width, so the result must be
         * computed across both hi/lo halves with carry/borrow correctly
         * propagated between them, using the standard multi-word
         * add-with-carry / subtract-with-borrow idiom (same technique
         * SLT/SLTU (Round 886) already established for 64-bit COMPARE -
         * this is the same idiom applied to actual arithmetic results).
         * Real MIPS DADD/DSUB trap on signed 64-bit overflow where
         * DADDU/DSUBU don't, but - exactly like ADDI/ADDIU (Round 897)
         * and ADDU/SUBU above - this project's own interpreter
         * deliberately never implements that trap (see ee_core.c's own
         * comment at its DADD/DADDU case), so all four funct codes here
         * share one dispatch block with zero behavioral difference. */
        if (rd == 0)
            return 0;
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(rt)));
        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_HI(rs)));
        emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, REG_HI(rt)));
        if (funct == 0x2C || funct == 0x2D) { /* DADD / DADDU */
            emit(ctx, enc_addc(SCRATCH_A, SCRATCH_A, SCRATCH_B)); /* lo = rs.lo+rt.lo, sets CA */
            emit(ctx, enc_adde(SCRATCH_C, SCRATCH_C, SCRATCH_D)); /* hi = rs.hi+rt.hi+CA */
        } else { /* funct == 0x2E || 0x2F: DSUB / DSUBU. subfc/subfe
                  * compute rB-rA (PPC's reversed operand order, same
                  * convention already used by SUBU/emit_slt_core above);
                  * we want rs-rt, so rA=SCRATCH_B/D (rt), rB=SCRATCH_A/C
                  * (rs), chaining subfc's borrow (CA) into subfe for the
                  * high half. */
            emit(ctx, enc_subfc(SCRATCH_A, SCRATCH_B, SCRATCH_A)); /* lo = rs.lo-rt.lo, sets CA (borrow) */
            emit(ctx, enc_subfe(SCRATCH_C, SCRATCH_D, SCRATCH_C)); /* hi = rs.hi-rt.hi-(1-CA) */
        }
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rd)));
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

    if (op == 0x00 && (funct == 0x38 || funct == 0x3A || funct == 0x3B)) {
        /* Round 898 (task #882): dsll/dsrl/dsra rd, rt, sa -> a genuine
         * FULL 64-bit shift by the literal 5-bit sa field (0-31; the
         * sa+32 range is DSLL32/DSRL32/DSRA32, three separate funct
         * codes NOT covered this round - see the STATUS.md writeup for
         * why they're deferred). PPC750 has no 64-bit shift instruction,
         * so this is synthesized from two 32-bit halves using the
         * standard "double-precision shift" idiom: bits that cross the
         * hi/lo boundary are reconstructed by shifting the OTHER half by
         * the complementary amount (32-sa) and OR-ing it in - e.g. for a
         * left shift, dsll's new hi word gets (rt.hi << sa) from its own
         * bits PLUS (rt.lo >> (32-sa)), the bits that "spilled over" from
         * the low word. sa and 32-sa are both compile-time constants
         * (sa is a literal instruction field), materialized once via
         * `li` and fed to the same slw/srw/sraw variable-shift encoders
         * Round 888's SLL/SRL/SRA family already established (so no new
         * PPC encoders are needed this round, only new dispatch logic).
         *
         * The sa==0 edge case needs no special-casing: it materializes
         * 32-sa == 32, and the real PowerPC ISA defines slw/srw/sraw
         * with a shift-COUNT register whose value is 32 or greater as
         * "result is all-zero" (or all-sign-bits for sraw) regardless of
         * the count's low 5 bits - checked bit 26 of the count operand,
         * which 32 sets. That's exactly the semantics an sa==0 shift
         * needs: zero bits should cross the hi/lo boundary at all, and
         * "shift by 32" naturally produces exactly that zero contribution
         * with no extra branch or conditional logic (verified explicitly
         * by this round's host-native harness, see docs/STATUS.md).
         *
         * The "spilled bits" combine (rt.lo>>32-sa for DSLL; rt.hi<<32-sa
         * for DSRL/DSRA's low word) is ALWAYS a logical (unsigned) shift
         * even for DSRA - those are genuine data bits crossing the word
         * boundary, not sign-extension fill; only DSRA's own high-word
         * shift (of rt.hi, which occupies the vacated most-significant
         * bits) needs to be arithmetic (sraw) to reproduce MIPS64's
         * sign-preserving right shift. rd==0 is a true no-op, declined
         * like every other opcode above. */
        if (rd == 0)
            return 0;
        {
            int32_t m = 32 - (int32_t)sa; /* complementary shift amount; 32 when sa==0 */
            emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_HI(rt)));
            emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(rt)));
            emit(ctx, enc_addi(SCRATCH_C, 0, (int16_t)sa)); /* li SCRATCH_C, sa */
            emit(ctx, enc_addi(SCRATCH_D, 0, (int16_t)m));  /* li SCRATCH_D, 32-sa */
            if (funct == 0x38) { /* DSLL */
                emit(ctx, enc_slw(SCRATCH_E, SCRATCH_B, SCRATCH_C)); /* new_lo = rt.lo << sa */
                emit(ctx, enc_slw(SCRATCH_F, SCRATCH_A, SCRATCH_C)); /* hi_part1 = rt.hi << sa */
                emit(ctx, enc_srw(SCRATCH_A, SCRATCH_B, SCRATCH_D)); /* hi_part2 = rt.lo >> (32-sa) */
                emit(ctx, enc_or(SCRATCH_F, SCRATCH_F, SCRATCH_A));  /* new_hi = hi_part1 | hi_part2 */
                emit(ctx, enc_stw(SCRATCH_E, CTX_REG, REG_LO(rd)));
                emit(ctx, enc_stw(SCRATCH_F, CTX_REG, REG_HI(rd)));
            } else { /* funct == 0x3A || 0x3B: DSRL / DSRA */
                if (funct == 0x3A) {
                    emit(ctx, enc_srw(SCRATCH_E, SCRATCH_A, SCRATCH_C));  /* new_hi = rt.hi >> sa (logical) */
                } else {
                    emit(ctx, enc_sraw(SCRATCH_E, SCRATCH_A, SCRATCH_C)); /* new_hi = rt.hi >> sa (arithmetic) */
                }
                emit(ctx, enc_srw(SCRATCH_F, SCRATCH_B, SCRATCH_C)); /* lo_part1 = rt.lo >> sa */
                emit(ctx, enc_slw(SCRATCH_B, SCRATCH_A, SCRATCH_D)); /* lo_part2 = rt.hi << (32-sa) */
                emit(ctx, enc_or(SCRATCH_F, SCRATCH_F, SCRATCH_B));  /* new_lo = lo_part1 | lo_part2 */
                emit(ctx, enc_stw(SCRATCH_F, CTX_REG, REG_LO(rd)));
                emit(ctx, enc_stw(SCRATCH_E, CTX_REG, REG_HI(rd)));
            }
        }
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

    if (op == 0x00 && (funct == 0x0A || funct == 0x0B)) {
        /* MIPS: movz/movn rd, rs, rt -> conditional 64-bit register
         * move: if (rd && test(GPR(rt))) GPR(rd) = GPR(rs); - test is
         * "==0" for MOVZ, "!=0" for MOVN (see ee_core.c's own case
         * bodies right above this comment's citation). This is the
         * FIRST dynarec opcode in a genuinely new class: every prior
         * opcode always overwrites rd unconditionally, so its OLD
         * value never mattered and never needed to be read back from
         * context. Here, when the condition is false, rd must be left
         * completely untouched - so this block loads rd's OLD hi/lo
         * words too, and blends old-vs-new per word with a branch-free
         * mask trick (PPC750 predates the "isel" conditional-select
         * instruction, so an actual PPC branch or a bitmask blend are
         * the only two options - the mask blend was chosen to keep
         * this dynarec's straight-line-only code-generation model,
         * with no new branch-target/label bookkeeping needed).
         *
         * The mask is built ENTIRELY from already-verified encoders
         * used elsewhere in this file - no new PPC encodings needed
         * for this opcode pair at all:
         *   1. rtOr = rt.hi | rt.lo (enc_or, from the AND/OR/XOR/NOR
         *      block above) - the full 64-bit "is rt zero" test
         *      collapsed into one 32-bit OR, since MIPS's condition is
         *      over the FULL 64-bit rt, not just its low word.
         *   2. subfc(throwaway, one, rtOr) sets CA = 1 iff rtOr>=1
         *      (i.e. rtOr!=0), reusing the exact same subfc/CA-setting
         *      idiom emit_slt_core uses for its borrow chain.
         *   3. subfe(mask, one, one) = one-one+CA-1 = CA-1, giving
         *      mask=0xFFFFFFFF when CA=0 (rtOr==0, i.e. MOVZ's "move"
         *      condition) or mask=0 when CA=1 (rtOr!=0) - this is
         *      exactly emit_slt_core's documented "self-subtract
         *      carry-to-mask trick" (rD==rA==rB), just reused here for
         *      a different comparison. For MOVN the desired condition
         *      is inverted, so one extra `nor mask,mask,mask` (a
         *      bitwise NOT, already used elsewhere to build SLTU's
         *      64-bit logical NOR) flips 0<->0xFFFFFFFF.
         *   4. notmask = NOT mask (another enc_nor reuse).
         *   5. Per word (hi, then lo): result = (rs_word & mask) |
         *      (rd_old_word & notmask) - two enc_and's and one enc_or,
         *      the classic branch-free "bit select" idiom.
         * rd==0 is declined exactly like every other opcode (matching
         * ee_core.c's own `if (rd && ...)` guard - $zero is never a
         * valid move target either way). */
        if (rd == 0)
            return 0;
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rt)));
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_HI(rt)));
        emit(ctx, enc_or(SCRATCH_A, SCRATCH_A, SCRATCH_B)); /* SCRATCH_A = rtOr */
        emit(ctx, enc_addi(SCRATCH_B, 0, 1)); /* li SCRATCH_B, 1 */
        emit(ctx, enc_subfc(SCRATCH_D, SCRATCH_B, SCRATCH_A)); /* SCRATCH_D = rtOr-1 (throwaway), CA = (rtOr != 0) */
        emit(ctx, enc_subfe(SCRATCH_A, SCRATCH_B, SCRATCH_B)); /* SCRATCH_A = mask: 0xFFFFFFFF iff rtOr==0, else 0 */
        if (funct == 0x0B) /* MOVN wants the opposite condition */
            emit(ctx, enc_nor(SCRATCH_A, SCRATCH_A, SCRATCH_A)); /* SCRATCH_A = ~mask */

        /* --- lo word --- */
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_LO(rd))); /* rd's OLD lo word */
        emit(ctx, enc_and(SCRATCH_D, SCRATCH_B, SCRATCH_A)); /* SCRATCH_D = rs.lo & mask */
        emit(ctx, enc_nor(SCRATCH_B, SCRATCH_A, SCRATCH_A)); /* SCRATCH_B = notmask (reused for hi below too) */
        emit(ctx, enc_and(SCRATCH_C, SCRATCH_C, SCRATCH_B)); /* SCRATCH_C = rd_old.lo & notmask */
        emit(ctx, enc_or(SCRATCH_D, SCRATCH_D, SCRATCH_C)); /* SCRATCH_D = blended lo result */
        emit(ctx, enc_stw(SCRATCH_D, CTX_REG, REG_LO(rd)));

        /* --- hi word (SCRATCH_A=mask, SCRATCH_B=notmask still valid) --- */
        emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, REG_HI(rs)));
        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_HI(rd))); /* rd's OLD hi word */
        emit(ctx, enc_and(SCRATCH_D, SCRATCH_D, SCRATCH_A)); /* SCRATCH_D = rs.hi & mask */
        emit(ctx, enc_and(SCRATCH_C, SCRATCH_C, SCRATCH_B)); /* SCRATCH_C = rd_old.hi & notmask */
        emit(ctx, enc_or(SCRATCH_D, SCRATCH_D, SCRATCH_C)); /* SCRATCH_D = blended hi result */
        emit(ctx, enc_stw(SCRATCH_D, CTX_REG, REG_HI(rd)));
        return 0;
    }

    if (op == 0x00 && (funct == 0x10 || funct == 0x11 || funct == 0x12 || funct == 0x13)) {
        /* MIPS: mfhi/mthi/mflo/mtlo - full 64-bit copies between a real
         * GPR and the HI or LO pseudo-slot (see HI_IDX/LO_IDX above),
         * exactly mirroring ee_core.c's own `GPR(rd) = st->hi.ud0` /
         * `st->hi.ud0 = GPR(rs)` bodies. Unlike every arithmetic opcode
         * above, MFHI/MFLO's "rd==0 declines" guard is the ONLY
         * rd-related guard needed (MTHI/MTLO have no destination GPR to
         * guard at all - $zero is a perfectly normal, always-zero
         * SOURCE for MTHI/MTLO, same as any other read of $zero). */
        int src_idx = (funct == 0x11 || funct == 0x13) ? (int)rs : (funct == 0x10 ? HI_IDX : LO_IDX);
        int dst_idx = (funct == 0x10 || funct == 0x12) ? (int)rd : (funct == 0x11 ? HI_IDX : LO_IDX);
        if ((funct == 0x10 || funct == 0x12) && rd == 0)
            return 0;
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_HI(src_idx)));
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(src_idx)));
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_HI(dst_idx)));
        emit(ctx, enc_stw(SCRATCH_B, CTX_REG, REG_LO(dst_idx)));
        return 0;
    }

    if (op == 0x00 && (funct == 0x18 || funct == 0x19)) {
        /* MIPS: mult/multu rs, rt -> a genuine 32x32->64 widening
         * multiply (signed for MULT, unsigned for MULTU), which PPC750
         * only offers as a pair of separate 32-bit-result instructions
         * (mullw for the low half, mulhw/mulhwu for the high half - see
         * their shared comment above). Per ee_core.c's own case bodies,
         * BOTH 32-bit halves of the result get independently
         * sign-extended into their own 64-bit HI/LO slot (this is real
         * R5900 hardware behavior, not an interpreter quirk - it
         * applies even for MULTU's unsigned high half), and this
         * happens UNCONDITIONALLY regardless of rd - only the OPTIONAL
         * `if (rd) GPR(rd) = st->lo.ud0` extra copy is gated on rd!=0.
         * So, unlike every earlier opcode's `if (rd==0) return 0` early
         * decline, that guard here would be WRONG - it would silently
         * skip the always-real HI/LO side effect whenever a real
         * program computed a product into $zero on purpose (legal and
         * not unusual, e.g. as a pure remainder-via-DIV setup) . */
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs))); /* rs32 */
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(rt))); /* rt32 */
        emit(ctx, enc_mullw(SCRATCH_C, SCRATCH_A, SCRATCH_B)); /* lo32 */
        if (funct == 0x18) /* MULT: signed high half */
            emit(ctx, enc_mulhw(SCRATCH_D, SCRATCH_A, SCRATCH_B));
        else /* MULTU: unsigned high half */
            emit(ctx, enc_mulhwu(SCRATCH_D, SCRATCH_A, SCRATCH_B));
        emit(ctx, enc_srawi(SCRATCH_E, SCRATCH_C, 31)); /* sign fill of lo32 -> new LO.hi */
        emit(ctx, enc_stw(SCRATCH_E, CTX_REG, REG_HI(LO_IDX)));
        emit(ctx, enc_stw(SCRATCH_C, CTX_REG, REG_LO(LO_IDX)));
        emit(ctx, enc_srawi(SCRATCH_F, SCRATCH_D, 31)); /* sign fill of hi32 -> new HI.hi */
        emit(ctx, enc_stw(SCRATCH_F, CTX_REG, REG_HI(HI_IDX)));
        emit(ctx, enc_stw(SCRATCH_D, CTX_REG, REG_LO(HI_IDX)));
        if (rd != 0) {
            emit(ctx, enc_stw(SCRATCH_E, CTX_REG, REG_HI(rd)));
            emit(ctx, enc_stw(SCRATCH_C, CTX_REG, REG_LO(rd)));
        }
        return 0;
    }

    if (op == 0x00 && (funct == 0x1A || funct == 0x1B)) {
        /* MIPS: div/divu rs, rt -> quotient into LO, remainder into HI
         * (see ee_core.c's own case bodies) - but ONLY if rt32 != 0;
         * per real MIPS semantics, HI/LO are left COMPLETELY UNCHANGED
         * on divide-by-zero (there's no MIPS-I/II trap for this, unlike
         * some later ISA revisions' optional trap instructions). This
         * dynarec has no branch-emission capability at all (see the
         * MOVZ/MOVN comment above), so the divide-by-zero case can't be
         * skipped with a real branch - instead this reuses Round 889's
         * branch-free mask-blend idiom: unconditionally execute
         * divw/divwu (safe - PPC's ISA-defined behavior for a zero
         * divisor is an UNDEFINED result value, never a trap, when OE
         * isn't set, which it never is here), compute the remainder
         * from that possibly-garbage quotient the same way regardless,
         * then blend the (possibly-garbage) new HI/LO against their OLD
         * values using a mask built from "is rt32 != 0" - the garbage
         * is simply discarded by the mask whenever rt32==0. DIV/DIVU
         * never touch any GPR (no `if (rd)` in ee_core.c's case bodies
         * at all - only HI/LO), so `rd` is deliberately unused here. */
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs))); /* rs32 */
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(rt))); /* rt32 */
        if (funct == 0x1A) /* DIV: signed */
            emit(ctx, enc_divw(SCRATCH_C, SCRATCH_A, SCRATCH_B));
        else /* DIVU: unsigned */
            emit(ctx, enc_divwu(SCRATCH_C, SCRATCH_A, SCRATCH_B));
        emit(ctx, enc_mullw(SCRATCH_D, SCRATCH_C, SCRATCH_B)); /* quotient*rt32 */
        emit(ctx, enc_subf(SCRATCH_E, SCRATCH_D, SCRATCH_A)); /* remainder = rs32 - quotient*rt32 */

        /* mask/notmask from "rt32 != 0", same subfc/subfe carry-to-mask
         * idiom as MOVZ/MOVN (Round 889), just testing rt32 directly
         * (a single 32-bit word) instead of an OR'd 64-bit pair - DIV/
         * DIVU's rt32 truncation (see ee_core.c's own `rt32` decode) is
         * already a 32-bit value by definition, no OR-of-hi/lo needed.
         * subfe(F,F,F) after subfc gives F = -1+CA: CA=1 (rt32!=0) ->
         * F=0; CA=0 (rt32==0) -> F=0xFFFFFFFF - i.e. F comes out as
         * NOTMASK directly (the "keep old value" condition), saving the
         * extra invert MOVN needed; mask is then just NOT(notmask). */
        emit(ctx, enc_addi(SCRATCH_F, 0, 1)); /* li F, 1 */
        emit(ctx, enc_subfc(SCRATCH_A, SCRATCH_F, SCRATCH_B)); /* A=throwaway, CA=(rt32!=0) (rs32's old value no longer needed) */
        emit(ctx, enc_subfe(SCRATCH_F, SCRATCH_F, SCRATCH_F)); /* F = notmask */
        emit(ctx, enc_nor(SCRATCH_G, SCRATCH_F, SCRATCH_F)); /* G = mask */

        emit(ctx, enc_srawi(SCRATCH_H, SCRATCH_C, 31)); /* sign fill of quotient -> new LO.hi */
        /* --- blend LO (quotient) --- */
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_HI(LO_IDX))); /* old LO.hi */
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(LO_IDX))); /* old LO.lo */
        emit(ctx, enc_and(SCRATCH_D, SCRATCH_H, SCRATCH_G)); /* new LO.hi & mask */
        emit(ctx, enc_and(SCRATCH_A, SCRATCH_A, SCRATCH_F)); /* old LO.hi & notmask */
        emit(ctx, enc_or(SCRATCH_D, SCRATCH_D, SCRATCH_A));
        emit(ctx, enc_stw(SCRATCH_D, CTX_REG, REG_HI(LO_IDX)));
        emit(ctx, enc_and(SCRATCH_D, SCRATCH_C, SCRATCH_G)); /* new LO.lo & mask */
        emit(ctx, enc_and(SCRATCH_B, SCRATCH_B, SCRATCH_F)); /* old LO.lo & notmask */
        emit(ctx, enc_or(SCRATCH_D, SCRATCH_D, SCRATCH_B));
        emit(ctx, enc_stw(SCRATCH_D, CTX_REG, REG_LO(LO_IDX)));

        /* --- blend HI (remainder) - mask(G)/notmask(F) still valid --- */
        emit(ctx, enc_srawi(SCRATCH_H, SCRATCH_E, 31)); /* sign fill of remainder -> new HI.hi */
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_HI(HI_IDX))); /* old HI.hi */
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(HI_IDX))); /* old HI.lo */
        emit(ctx, enc_and(SCRATCH_D, SCRATCH_H, SCRATCH_G)); /* new HI.hi & mask */
        emit(ctx, enc_and(SCRATCH_A, SCRATCH_A, SCRATCH_F)); /* old HI.hi & notmask */
        emit(ctx, enc_or(SCRATCH_D, SCRATCH_D, SCRATCH_A));
        emit(ctx, enc_stw(SCRATCH_D, CTX_REG, REG_HI(HI_IDX)));
        emit(ctx, enc_and(SCRATCH_D, SCRATCH_E, SCRATCH_G)); /* new HI.lo & mask */
        emit(ctx, enc_and(SCRATCH_B, SCRATCH_B, SCRATCH_F)); /* old HI.lo & notmask */
        emit(ctx, enc_or(SCRATCH_D, SCRATCH_D, SCRATCH_B));
        emit(ctx, enc_stw(SCRATCH_D, CTX_REG, REG_LO(HI_IDX)));
        return 0;
    }

    if (op == 0x23) {
        /* MIPS: lw rt, imm(rs) -> gpr[rt] = sext32(ee_mem_read32(st,
         * rs32+imm)); the memory READ happens even when rt==0 (real
         * loads to $zero still perform their side effects - see
         * ee_core.c's own `if (rt) GPR(rt) = ...; else
         * ee_mem_read32(...);` case body), only the register WRITE is
         * skipped. This is this dynarec's first-ever opcode that calls
         * a real C function instead of just moving bits around, and the
         * first that must preserve registers across that call per the
         * PowerPC EABI:
         *
         *   - r3-r12 are volatile (caller need not preserve them across
         *     a call) - CTX_REG(r3) itself is exactly ee_mem_read32's
         *     first argument, so no register-shuffling is needed to set
         *     that argument up, but r3 is NOT valid to use as the
         *     context pointer again once bctrl returns (it now holds
         *     the loaded value, ee_mem_read32's real return value).
         *   - r14/r15 are non-volatile: safe to use as OUR OWN scratch
         *     across the call (the callee, ee_mem_read32, must
         *     preserve them for us) - r15 holds a saved copy of the
         *     context pointer (needed again after the call, to store
         *     the loaded value back), r14 holds this whole generated
         *     block's own incoming return address (LR), which bctrl's
         *     own "branch AND LINK" semantics would otherwise clobber
         *     with an address inside this very block.
         *   - BUT r14/r15 being non-volatile from ee_mem_read32's point
         *     of view cuts both ways: THIS block is itself a callee (of
         *     ee_jit_try_execute_one, via the plain C function-pointer
         *     call in ee_jit.c), so it must not clobber its OWN
         *     caller's r14/r15 either. Hence the small stack frame
         *     below: the caller's r14/r15 are spilled there at entry
         *     and reloaded right before this block returns. (No back-
         *     chain word is written at [r1+0] - nothing ever needs to
         *     unwind through JIT-generated code here - so this frame is
         *     privately consistent but not a fully conformant EABI
         *     frame; documented rather than silently assumed away.) */
        emit(ctx, enc_addi(1, 1, -32));              /* grow frame */
        emit(ctx, enc_stw(14, 1, 8));                 /* save caller's r14 */
        emit(ctx, enc_stw(15, 1, 12));                /* save caller's r15 */
        emit(ctx, enc_or(15, 3, 3));                  /* r15 = ctx (mr r15,r3) */
        emit(ctx, enc_mflr(14));                      /* r14 = this block's real return address */
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs))); /* rs32 */
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm)); /* r4 = addr (arg2); r3=ctx already arg1 */
        emit_load_const32(ctx, 12, ADDR_EE_MEM_READ32);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* r3 = ee_mem_read32(ctx, addr) */
        emit(ctx, enc_mtlr(14));                      /* restore this block's real return address */
        if (rt != 0) {
            /* r3 = loaded 32-bit value, r15 = saved ctx pointer (still
             * valid - non-volatile, untouched by the call). LW
             * sign-extends into the full 64-bit destination register,
             * same srawi-by-31 fill-word idiom as ADDIU/ADDU/SLL. */
            emit(ctx, enc_stw(3, 15, REG_LO(rt)));
            emit(ctx, enc_srawi(SCRATCH_A, 3, 31));
            emit(ctx, enc_stw(SCRATCH_A, 15, REG_HI(rt)));
        }
        emit(ctx, enc_lwz(14, 1, 8));                  /* restore caller's r14 */
        emit(ctx, enc_lwz(15, 1, 12));                 /* restore caller's r15 */
        emit(ctx, enc_addi(1, 1, 32));                 /* shrink frame */
        return 0;
    }

    if (op == 0x2B) {
        /* MIPS: sw rt, imm(rs) -> ee_mem_write32(st, rs32+imm,
         * (uint32_t)gpr[rt]); no rt==0 guard exists in ee_core.c's own
         * case body - reading $zero as the value-to-store is always
         * valid (and always 0), so this always emits unconditionally.
         * Simpler than LW: no result flows back, so no r15 (saved ctx)
         * is needed - only r14 (saved return address) crosses the call. */
        emit(ctx, enc_addi(1, 1, -32));
        emit(ctx, enc_stw(14, 1, 8));
        emit(ctx, enc_mflr(14));
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs))); /* rs32 */
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(rt))); /* value (arg3) - read before r3/r4 get set up */
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm)); /* r4 = addr (arg2); r3=ctx already arg1 */
        emit_load_const32(ctx, 12, ADDR_EE_MEM_WRITE32);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* ee_mem_write32(ctx, addr, val) */
        emit(ctx, enc_mtlr(14));
        emit(ctx, enc_lwz(14, 1, 8));                  /* restore caller's r14 */
        emit(ctx, enc_addi(1, 1, 32));                 /* shrink frame */
        return 0;
    }

    if (op == 0x20 || op == 0x24) {
        /* MIPS: lb/lbu rt, imm(rs). Both call ee_mem_read8() - real
         * ee_core.c's case 0x20/0x24 bodies are identical except for
         * how the loaded byte is widened afterward (sign- vs zero-
         * extend), same split as LW/LWU below. Same read-always-
         * happens-even-when-rt==0 rule as LW (see that block's own
         * comment for the full register-preservation walkthrough this
         * reuses verbatim - CTX_REG/r15/r14/stack-frame discipline is
         * identical here, just a different callee address and an
         * extra widening step before the store-back). */
        int is_signed = (op == 0x20);
        emit(ctx, enc_addi(1, 1, -32));
        emit(ctx, enc_stw(14, 1, 8));
        emit(ctx, enc_stw(15, 1, 12));
        emit(ctx, enc_or(15, 3, 3));
        emit(ctx, enc_mflr(14));
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm));
        emit_load_const32(ctx, 12, ADDR_EE_MEM_READ8);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* r3 = ee_mem_read8(ctx, addr) */
        emit(ctx, enc_mtlr(14));
        if (is_signed) {
            /* LB: turn the loaded byte (low 8 bits of r3, upper bits
             * unspecified by the ABI - see enc_extsb's own comment)
             * into a properly sign-extended 32-bit value first. */
            emit(ctx, enc_extsb(3, 3));
        } else {
            /* LBU: zero-extend by masking to just the low byte -
             * andi. clears the upper 24 bits unconditionally, which is
             * exactly what "zero-extend" means here (and leaves bit31
             * clear, so the srawi-by-31 fill word below correctly
             * comes out 0 too - the 64-bit HIGH half of a zero-
             * extended value is always 0). */
            emit(ctx, enc_andi_dot(3, 3, 0x00FF));
        }
        if (rt != 0) {
            emit(ctx, enc_stw(3, 15, REG_LO(rt)));
            emit(ctx, enc_srawi(SCRATCH_A, 3, 31));
            emit(ctx, enc_stw(SCRATCH_A, 15, REG_HI(rt)));
        }
        emit(ctx, enc_lwz(14, 1, 8));
        emit(ctx, enc_lwz(15, 1, 12));
        emit(ctx, enc_addi(1, 1, 32));
        return 0;
    }

    if (op == 0x21 || op == 0x25) {
        /* MIPS: lh/lhu rt, imm(rs) - identical shape to LB/LBU above,
         * just calling ee_mem_read16() and using extsh/a 16-bit mask
         * instead of extsb/an 8-bit mask for the widening step. */
        int is_signed = (op == 0x21);
        emit(ctx, enc_addi(1, 1, -32));
        emit(ctx, enc_stw(14, 1, 8));
        emit(ctx, enc_stw(15, 1, 12));
        emit(ctx, enc_or(15, 3, 3));
        emit(ctx, enc_mflr(14));
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm));
        emit_load_const32(ctx, 12, ADDR_EE_MEM_READ16);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* r3 = ee_mem_read16(ctx, addr) */
        emit(ctx, enc_mtlr(14));
        if (is_signed) {
            emit(ctx, enc_extsh(3, 3));
        } else {
            emit(ctx, enc_andi_dot(3, 3, 0xFFFF));
        }
        if (rt != 0) {
            emit(ctx, enc_stw(3, 15, REG_LO(rt)));
            emit(ctx, enc_srawi(SCRATCH_A, 3, 31));
            emit(ctx, enc_stw(SCRATCH_A, 15, REG_HI(rt)));
        }
        emit(ctx, enc_lwz(14, 1, 8));
        emit(ctx, enc_lwz(15, 1, 12));
        emit(ctx, enc_addi(1, 1, 32));
        return 0;
    }

    if (op == 0x27) {
        /* MIPS: lwu rt, imm(rs) -> gpr[rt] = zero_extend_64(
         * ee_mem_read32(st, rs32+imm)). Calls the exact same
         * ee_mem_read32() as LW above (see ADDR_EE_MEM_READ32's own
         * comment) - the only difference from LW is that the 64-bit
         * destination's high half is always 0 here (zero-extend)
         * instead of a sign-extension fill word, so this block skips
         * the srawi entirely and just stores a literal 0 (materialized
         * via `addi r,0,0`, the standard PPC "li" idiom) to REG_HI. */
        emit(ctx, enc_addi(1, 1, -32));
        emit(ctx, enc_stw(14, 1, 8));
        emit(ctx, enc_stw(15, 1, 12));
        emit(ctx, enc_or(15, 3, 3));
        emit(ctx, enc_mflr(14));
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm));
        emit_load_const32(ctx, 12, ADDR_EE_MEM_READ32);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* r3 = ee_mem_read32(ctx, addr) */
        emit(ctx, enc_mtlr(14));
        if (rt != 0) {
            emit(ctx, enc_stw(3, 15, REG_LO(rt)));
            emit(ctx, enc_addi(SCRATCH_A, 0, 0));      /* li SCRATCH_A, 0 */
            emit(ctx, enc_stw(SCRATCH_A, 15, REG_HI(rt)));
        }
        emit(ctx, enc_lwz(14, 1, 8));
        emit(ctx, enc_lwz(15, 1, 12));
        emit(ctx, enc_addi(1, 1, 32));
        return 0;
    }

    if (op == 0x28 || op == 0x29) {
        /* MIPS: sb/sh rt, imm(rs) -> ee_mem_write8/16(st, rs32+imm,
         * (uintN_t)gpr[rt]). Same shape as SW above (no rt==0 guard in
         * ee_core.c's own case bodies - reading $zero as the value-to-
         * store is always valid, and always 0) - the low 8/16 bits of
         * REG_LO(rt) are passed to the callee exactly as loaded, with
         * no explicit masking needed on this side: per the PowerPC
         * EABI, a sub-word parameter type is the CALLEE's
         * responsibility to narrow (the caller need not clear the
         * argument register's upper bits), same reasoning as this
         * round's LB/LH return-value comment but mirrored for
         * arguments instead of return values. */
        uint32_t addr_const = (op == 0x28) ? ADDR_EE_MEM_WRITE8 : ADDR_EE_MEM_WRITE16;
        emit(ctx, enc_addi(1, 1, -32));
        emit(ctx, enc_stw(14, 1, 8));
        emit(ctx, enc_mflr(14));
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(rt)));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm));
        emit_load_const32(ctx, 12, addr_const);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* ee_mem_write8/16(ctx, addr, val) */
        emit(ctx, enc_mtlr(14));
        emit(ctx, enc_lwz(14, 1, 8));
        emit(ctx, enc_addi(1, 1, 32));
        return 0;
    }

    if (op == 0x37) {
        /* MIPS: ld rt, imm(rs) -> gpr[rt] = ee_mem_read64(st,
         * rs32+imm) - a plain 64-bit copy, no sign/zero-extension of
         * any kind (unlike every load this dynarec has handled so
         * far). Same read-always-happens-even-when-rt==0 rule as
         * LW/LB/LH (ee_core.c's own `if (rt) GPR(rt) = ...; else
         * ee_mem_read64(...);` case body). See
         * ADDR_EE_MEM_READ64's comment above for the r3:r4 hi:lo
         * return-value convention this block relies on - it's what
         * lets the post-call code be just two stores with no srawi/
         * extsb/extsh/andi. widening step at all. */
        emit(ctx, enc_addi(1, 1, -32));
        emit(ctx, enc_stw(14, 1, 8));
        emit(ctx, enc_stw(15, 1, 12));
        emit(ctx, enc_or(15, 3, 3));
        emit(ctx, enc_mflr(14));
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm));
        emit_load_const32(ctx, 12, ADDR_EE_MEM_READ64);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* r3:r4 = ee_mem_read64(ctx, addr), hi:lo */
        emit(ctx, enc_mtlr(14));
        if (rt != 0) {
            emit(ctx, enc_stw(3, 15, REG_HI(rt)));
            emit(ctx, enc_stw(4, 15, REG_LO(rt)));
        }
        emit(ctx, enc_lwz(14, 1, 8));
        emit(ctx, enc_lwz(15, 1, 12));
        emit(ctx, enc_addi(1, 1, 32));
        return 0;
    }

    if (op == 0x3F) {
        /* MIPS: sd rt, imm(rs) -> ee_mem_write64(st, rs32+imm,
         * gpr[rt]) - no rt==0 guard in ee_core.c's own case body
         * (reading $zero as the value-to-store is always valid, and
         * always 0), same as SW/SB/SH. The 64-bit value is loaded
         * into r5:r6 (hi:lo) BEFORE r4 is overwritten with the
         * computed address - same "load the value first, finalize the
         * address into SCRATCH_A/r4 last" ordering SW already uses,
         * just with an extra register for the value's high word. */
        emit(ctx, enc_addi(1, 1, -32));
        emit(ctx, enc_stw(14, 1, 8));
        emit(ctx, enc_mflr(14));
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_HI(rt))); /* r5 = val hi (arg3 hi) */
        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_LO(rt))); /* r6 = val lo (arg3 lo) */
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs))); /* rs32 */
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm)); /* r4 = addr (arg2) */
        emit_load_const32(ctx, 12, ADDR_EE_MEM_WRITE64);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* ee_mem_write64(ctx, addr, val) */
        emit(ctx, enc_mtlr(14));
        emit(ctx, enc_lwz(14, 1, 8));
        emit(ctx, enc_addi(1, 1, 32));
        return 0;
    }

    if (op == 0x22 || op == 0x26) {
        /* Round 900 (task #883): lwl/lwr rt, imm(rs) - the unaligned-word
         * load pair. Both read ONE aligned 32-bit word at (rs32+imm)&~3
         * and merge PART of it (selected by the low 2 bits of the real,
         * unaligned address) into rt, leaving the other part of rt's
         * existing value untouched - LWL fills rt's high-order bytes
         * from the read word's low-order bytes, LWR does the mirror
         * image. ee_core.c's own LWL_MASK/LWL_SHIFT/LWR_MASK/LWR_SHIFT
         * lookup tables (indexed by the runtime 2-bit `shift = addr&3`)
         * are NOT reproduced as tables here - every entry collapses to a
         * simple formula of `shift`, computed with plain register
         * arithmetic instead:
         *   LWL_SHIFT[shift] = 24 - 8*shift   (array: 24,16,8,0)
         *   LWL_MASK[shift]  = 0xFFFFFFFF >> (8*shift+8) (array: 0xffffff,0xffff,0xff,0)
         *   LWR_SHIFT[shift] = 8*shift               (array: 0,8,16,24)
         *   LWR_MASK[shift]  = ~(0xFFFFFFFF >> LWR_SHIFT[shift]) (array: 0,0xff000000,0xffff0000,0xffffff00)
         * (each verified against ee_core.c's literal table contents
         * before writing any codegen). The shift==3 (LWL) / shift==0
         * (LWR-mask) edge cases divide by "shift 32", which is exactly
         * where real PPC750 slw/srw's own ">=32 -> zero" hardware rule
         * (already relied on by DSLL/DSRL/DSRA, Round 898) does the
         * right thing with no special-case branch needed.
         *
         * Values needed AFTER the ee_mem_read32() call (shift, rt32,
         * the two shift amounts) are NOT trusted to survive the call in
         * a volatile scratch register (r4-r11 are caller-saved/volatile
         * per the PowerPC EABI - a real callee is free to clobber them)
         * - instead everything except the call's own arguments is
         * RECOMPUTED from context (via r15, the non-volatile saved ctx
         * pointer) after bctrl returns, same conservative discipline
         * LW/LB/LH already established. */
        int is_lwl = (op == 0x22);
        emit(ctx, enc_addi(1, 1, -32));
        emit(ctx, enc_stw(14, 1, 8));
        emit(ctx, enc_stw(15, 1, 12));
        emit(ctx, enc_or(15, 3, 3));
        emit(ctx, enc_mflr(14));
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm)); /* unaligned addr */
        emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, 0, 0, 29));   /* aligned addr (r4, arg2) */
        emit_load_const32(ctx, 12, ADDR_EE_MEM_READ32);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* r3 = mem (aligned word) */
        emit(ctx, enc_mtlr(14));

        if (rt != 0) {
            /* post-call: recompute addr/shift fresh from ctx (r15) */
            emit(ctx, enc_lwz(SCRATCH_A, 15, REG_LO(rs)));
            emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm)); /* unaligned addr */
            emit(ctx, enc_andi_dot(SCRATCH_D, SCRATCH_A, 3));         /* D = shift (0..3) */
            emit(ctx, enc_addi(SCRATCH_E, 0, 3));
            emit(ctx, enc_slw(SCRATCH_E, SCRATCH_D, SCRATCH_E));      /* E = shift8 = shift<<3 */

            if (is_lwl) {
                emit(ctx, enc_addi(SCRATCH_F, 0, 24));
                emit(ctx, enc_subf(SCRATCH_F, SCRATCH_E, SCRATCH_F));   /* F = 24-shift8 = LWL_SHIFT */
                emit(ctx, enc_slw(SCRATCH_G, 3, SCRATCH_F));            /* G = mem << LWL_SHIFT */
                emit(ctx, enc_addi(SCRATCH_H, 0, 8));
                emit(ctx, enc_add(SCRATCH_E, SCRATCH_E, SCRATCH_H));    /* E = shift8+8 = mask_shift */
                emit(ctx, enc_addi(SCRATCH_H, 0, -1));
                emit(ctx, enc_srw(SCRATCH_H, SCRATCH_H, SCRATCH_E));    /* H = LWL_MASK = 0xFFFFFFFF>>mask_shift */
                emit(ctx, enc_lwz(SCRATCH_C, 15, REG_LO(rt)));          /* C = rt32 */
                emit(ctx, enc_and(SCRATCH_C, SCRATCH_C, SCRATCH_H));    /* rt32 & mask */
                emit(ctx, enc_or(SCRATCH_C, SCRATCH_C, SCRATCH_G));     /* result = (rt32&mask)|(mem<<shift) */
                emit(ctx, enc_stw(SCRATCH_C, 15, REG_LO(rt)));
                emit(ctx, enc_srawi(SCRATCH_A, SCRATCH_C, 31));         /* LWL always full-sign-extends */
                emit(ctx, enc_stw(SCRATCH_A, 15, REG_HI(rt)));
            } else { /* LWR */
                emit(ctx, enc_srw(SCRATCH_G, 3, SCRATCH_E));            /* G = mem >> LWR_SHIFT(=shift8) */
                emit(ctx, enc_addi(SCRATCH_H, 0, -1));
                emit(ctx, enc_srw(SCRATCH_H, SCRATCH_H, SCRATCH_E));    /* 0xFFFFFFFF>>LWR_SHIFT */
                emit(ctx, enc_nor(SCRATCH_H, SCRATCH_H, SCRATCH_H));    /* H = LWR_MASK = ~that */
                emit(ctx, enc_lwz(SCRATCH_C, 15, REG_LO(rt)));          /* C = rt32 */
                emit(ctx, enc_and(SCRATCH_C, SCRATCH_C, SCRATCH_H));    /* rt32 & mask */
                emit(ctx, enc_or(SCRATCH_C, SCRATCH_C, SCRATCH_G));     /* result = (rt32&mask)|(mem>>shift) */
                emit(ctx, enc_stw(SCRATCH_C, 15, REG_LO(rt)));
                /* shift==0: full sign-extend (like LW). shift!=0: leave
                 * rt's existing hi word untouched. Blend via the
                 * standard is-zero-mask idiom (subfc/subfe self-
                 * subtract trick, same one BEQ/BLEZ already use). */
                emit(ctx, enc_addi(SCRATCH_A, 0, 1));
                emit(ctx, enc_subfc(SCRATCH_B, SCRATCH_A, SCRATCH_D));  /* CA = (shift != 0) */
                emit(ctx, enc_subfe(SCRATCH_B, SCRATCH_A, SCRATCH_A));  /* B = iszero_mask */
                emit(ctx, enc_srawi(SCRATCH_G, SCRATCH_C, 31));         /* sign-fill candidate */
                emit(ctx, enc_lwz(SCRATCH_H, 15, REG_HI(rt)));          /* preserve candidate (old hi) */
                emit(ctx, enc_and(SCRATCH_G, SCRATCH_G, SCRATCH_B));    /* sign_fill & iszero_mask */
                emit(ctx, enc_nor(SCRATCH_B, SCRATCH_B, SCRATCH_B));    /* notmask */
                emit(ctx, enc_and(SCRATCH_H, SCRATCH_H, SCRATCH_B));    /* old_hi & notmask */
                emit(ctx, enc_or(SCRATCH_G, SCRATCH_G, SCRATCH_H));
                emit(ctx, enc_stw(SCRATCH_G, 15, REG_HI(rt)));
            }
        }
        emit(ctx, enc_lwz(14, 1, 8));
        emit(ctx, enc_lwz(15, 1, 12));
        emit(ctx, enc_addi(1, 1, 32));
        return 0;
    }

    if (op == 0x2A || op == 0x2E) {
        /* Round 900 (task #883): swl/swr rt, imm(rs) - the unaligned-
         * word store pair, and the store-side mirror of LWL/LWR above.
         * Real hardware genuinely performs a READ of the existing
         * aligned word, merges in the bytes selected from rt, and
         * WRITES the merged word back - two real ee_mem_read32/write32
         * calls in one generated block (this dynarec's first opcode
         * that calls a real C function twice), each with its own
         * register-preservation discipline since either call is free to
         * clobber r4-r11. SWL_MASK/SWL_SHIFT and SWR_MASK/SWR_SHIFT
         * collapse to the SAME formulas as LWL/LWR's masks above (SWL's
         * mask is a LEFT shift of 0xFFFFFFFF instead of LWL's right
         * shift - see the inline comments below for the exact mapping),
         * verified against ee_core.c's literal tables before writing
         * any codegen. */
        int is_swl = (op == 0x2A);
        emit(ctx, enc_addi(1, 1, -32));
        emit(ctx, enc_stw(14, 1, 8));
        emit(ctx, enc_stw(15, 1, 12));
        emit(ctx, enc_or(15, 3, 3));
        emit(ctx, enc_mflr(14));

        /* --- call #1: read the existing aligned word --- */
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm));
        emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, 0, 0, 29)); /* aligned addr (r4) */
        emit_load_const32(ctx, 12, ADDR_EE_MEM_READ32);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* r3 = old mem word */
        emit(ctx, enc_mtlr(14));

        /* --- post-call: recompute addr/shift, derive the merged value.
         * SCRATCH_A(r4) ends this section holding the UNALIGNED addr
         * (aligned again right before call #2, in place - already r4,
         * the exact arg2 register, so no extra move is needed);
         * SCRATCH_B(r5) ends up holding the merged value - also already
         * the exact arg3-low register SD's convention uses, so no extra
         * move is needed there either. */
        emit(ctx, enc_or(SCRATCH_C, 3, 3));            /* C = old mem (save before r3 gets reused) */
        emit(ctx, enc_lwz(SCRATCH_A, 15, REG_LO(rs)));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm)); /* unaligned addr -> A (r4) */
        emit(ctx, enc_andi_dot(SCRATCH_D, SCRATCH_A, 3));         /* D = shift */
        emit(ctx, enc_addi(SCRATCH_E, 0, 3));
        emit(ctx, enc_slw(SCRATCH_E, SCRATCH_D, SCRATCH_E));      /* E = shift8 */
        emit(ctx, enc_lwz(SCRATCH_G, 15, REG_LO(rt)));            /* G = rt32 */

        if (is_swl) {
            emit(ctx, enc_addi(SCRATCH_F, 0, 24));
            emit(ctx, enc_subf(SCRATCH_F, SCRATCH_E, SCRATCH_F));    /* F = 24-shift8 = SWL_SHIFT */
            emit(ctx, enc_srw(SCRATCH_B, SCRATCH_G, SCRATCH_F));     /* B(r5) = rt32 >> SWL_SHIFT */
            emit(ctx, enc_addi(SCRATCH_H, 0, 8));
            emit(ctx, enc_add(SCRATCH_E, SCRATCH_E, SCRATCH_H));     /* E = mask_shift = shift8+8 */
            emit(ctx, enc_addi(SCRATCH_H, 0, -1));
            emit(ctx, enc_slw(SCRATCH_H, SCRATCH_H, SCRATCH_E));     /* H = SWL_MASK = 0xFFFFFFFF<<mask_shift */
        } else { /* SWR */
            emit(ctx, enc_slw(SCRATCH_B, SCRATCH_G, SCRATCH_E));     /* B(r5) = rt32 << SWR_SHIFT(=shift8) */
            emit(ctx, enc_addi(SCRATCH_F, 0, 32));
            emit(ctx, enc_subf(SCRATCH_F, SCRATCH_E, SCRATCH_F));    /* F = 32-shift8 (NOT shift8 itself -
                                                                        * SWR_MASK[shift] = 0xFFFFFFFF>>(32-shift8),
                                                                        * a different formula from LWR_MASK's
                                                                        * 0xFFFFFFFF>>shift8 term - verified against
                                                                        * ee_core.c's real SWR_MASK table
                                                                        * {0,0xff,0xffff,0xffffff} this round after
                                                                        * a host-native harness caught the original
                                                                        * (wrong) shift8-only formula. */
            emit(ctx, enc_addi(SCRATCH_H, 0, -1));
            emit(ctx, enc_srw(SCRATCH_H, SCRATCH_H, SCRATCH_F));     /* H = SWR_MASK = 0xFFFFFFFF>>(32-shift8) */
        }
        emit(ctx, enc_and(SCRATCH_C, SCRATCH_C, SCRATCH_H));   /* old_mem & mask */
        emit(ctx, enc_or(SCRATCH_B, SCRATCH_B, SCRATCH_C));    /* B(r5) = merged value */

        /* --- call #2: write the merged word back --- */
        emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, 0, 0, 29)); /* A(r4) = aligned addr, in place */
        emit(ctx, enc_or(3, 15, 15));                          /* r3 = ctx */
        emit_load_const32(ctx, 12, ADDR_EE_MEM_WRITE32);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* ee_mem_write32(ctx, aligned, merged) */
        emit(ctx, enc_mtlr(14));

        emit(ctx, enc_lwz(14, 1, 8));
        emit(ctx, enc_lwz(15, 1, 12));
        emit(ctx, enc_addi(1, 1, 32));
        return 0;
    }

    if (op == 0x1E) {
        /* Round 900 (task #883): lq rt, imm(rs) - 128-bit load. Address
         * is masked to 16-byte alignment (real hardware ignores the low
         * 4 bits rather than faulting, unlike LW/LD - ee_core.c's own
         * `(rs32 + imm) & ~0xFu`). Matches ee_core.c's real PCSX2-ported
         * behavior of skipping the read ENTIRELY when rt==$0 (unlike
         * every other load in this dynarec, which still performs the
         * read for its memory side effects even when the destination is
         * discarded) - declining outright for rt==0 reproduces that
         * exactly, consistent with every rd==0/rt==0 guard elsewhere in
         * this file. This is this dynarec's first opcode that needs TWO
         * ee_mem_read64() calls in one block (one for ud0/GPR(rt), one
         * for ud1/GPR1(rt) - see REG_HI1/REG_LO1's own comment) - r15
         * (saved ctx) is restored into r3 before the second call, since
         * the first call's bctrl clobbers r3 with its own return value. */
        if (rt == 0)
            return 0;
        emit(ctx, enc_addi(1, 1, -32));
        emit(ctx, enc_stw(14, 1, 8));
        emit(ctx, enc_stw(15, 1, 12));
        emit(ctx, enc_or(15, 3, 3));
        emit(ctx, enc_mflr(14));

        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm));
        emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, 0, 0, 27)); /* &= ~0xF (aligned addr, r4) */
        emit_load_const32(ctx, 12, ADDR_EE_MEM_READ64);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* r3:r4 = GPR(rt).hi:lo (ud0) */
        emit(ctx, enc_mtlr(14));
        emit(ctx, enc_stw(3, 15, REG_HI(rt)));
        emit(ctx, enc_stw(4, 15, REG_LO(rt)));

        emit(ctx, enc_or(3, 15, 15));                  /* r3 = ctx again for call #2 */
        emit(ctx, enc_lwz(SCRATCH_A, 15, REG_LO(rs)));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm));
        emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, 0, 0, 27));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, 8));   /* aligned+8 (r4) */
        emit_load_const32(ctx, 12, ADDR_EE_MEM_READ64);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* r3:r4 = GPR1(rt).hi:lo (ud1) */
        emit(ctx, enc_mtlr(14));
        emit(ctx, enc_stw(3, 15, REG_HI1(rt)));
        emit(ctx, enc_stw(4, 15, REG_LO1(rt)));

        emit(ctx, enc_lwz(14, 1, 8));
        emit(ctx, enc_lwz(15, 1, 12));
        emit(ctx, enc_addi(1, 1, 32));
        return 0;
    }

    if (op == 0x1F) {
        /* Round 900 (task #883): sq rt, imm(rs) - 128-bit store, the
         * mirror of LQ above. Same 16-byte alignment masking. Always
         * writes both halves, including when rt==$0 (whose value is
         * always zero) - matches ee_core.c exactly, no rt==0 guard
         * needed (unlike LQ). Two ee_mem_write64() calls, same
         * r15-restore-into-r3 discipline as LQ's two reads. */
        emit(ctx, enc_addi(1, 1, -32));
        emit(ctx, enc_stw(14, 1, 8));
        emit(ctx, enc_stw(15, 1, 12));
        emit(ctx, enc_or(15, 3, 3));
        emit(ctx, enc_mflr(14));

        emit(ctx, enc_lwz(SCRATCH_B, 15, REG_HI(rt)));  /* val (ud0) hi -> r5 */
        emit(ctx, enc_lwz(SCRATCH_C, 15, REG_LO(rt)));  /* val (ud0) lo -> r6 */
        emit(ctx, enc_lwz(SCRATCH_A, 15, REG_LO(rs)));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm));
        emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, 0, 0, 27)); /* aligned addr (r4) */
        emit_load_const32(ctx, 12, ADDR_EE_MEM_WRITE64);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* ee_mem_write64(ctx, aligned, GPR(rt)) */
        emit(ctx, enc_mtlr(14));

        emit(ctx, enc_or(3, 15, 15));                  /* r3 = ctx again */
        emit(ctx, enc_lwz(SCRATCH_B, 15, REG_HI1(rt))); /* val (ud1) hi -> r5 */
        emit(ctx, enc_lwz(SCRATCH_C, 15, REG_LO1(rt))); /* val (ud1) lo -> r6 */
        emit(ctx, enc_lwz(SCRATCH_A, 15, REG_LO(rs)));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm));
        emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, 0, 0, 27));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, 8));   /* aligned+8 (r4) */
        emit_load_const32(ctx, 12, ADDR_EE_MEM_WRITE64);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* ee_mem_write64(ctx, aligned+8, GPR1(rt)) */
        emit(ctx, enc_mtlr(14));

        emit(ctx, enc_lwz(14, 1, 8));
        emit(ctx, enc_lwz(15, 1, 12));
        emit(ctx, enc_addi(1, 1, 32));
        return 0;
    }

    if (op == 0x11) {
        /* Round 902 (task #884): COP1 (FPU) data-movement and bit-level
         * opcodes - the first slice of the FPU family, deliberately
         * scoped to exclude any opcode that needs REAL floating-point
         * arithmetic (ADD.S/SUB.S/MUL.S/DIV.S/SQRT.S/etc - those need
         * genuine PPC750 FPU instructions (lfs/fadds/fsubs/stfs/...),
         * MIPS-vs-PPC rounding/denormal/exception-flag differences to
         * reconcile, and PCSX2's own overflow/underflow clamping
         * (fpu_check_overflow/fpu_check_underflow) ported faithfully -
         * a substantially bigger lift saved for a later round in this
         * arc). Every opcode below operates on the FPR/GPR/FCR31 raw
         * 32-bit bit patterns with plain integer loads/stores/logical
         * ops, exactly like ee_core.c's own case bodies do (see
         * ee_core.c's `case 0x11:` COP1 block) - no float hardware
         * touched at all, so none of the arithmetic-scope caveats above
         * apply here.
         *
         * COP1's own sub-opcode field is `rs` (bits 25-21) - reusing the
         * exact same decode variable this dynarec already extracts for
         * every other opcode, no new field needed. MFC1/CFC1/MTC1/CTC1
         * (rs==0x00/0x02/0x04/0x06) address the FPR/FCR with `rd`
         * directly (matching ee_core.c's own case bodies under this
         * `switch(rs)`, NOT COP1.S's fd/fs/ft convention below).
         * COP1.S (rs==0x10) is a further `funct`-selected sub-dispatch
         * with its own fd=sa/fs=rd/ft=rt field convention (also matching
         * ee_core.c exactly) - only MOV.S/ABS.S/NEG.S are implemented
         * here, the three funct values that are pure bit-twiddles. */
        if (rs == 0x00) { /* MFC1: if (rt) GPR(rt) = sext32(fpr[rd]) */
            if (rt == 0)
                return 0;
            emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_FPR(rd)));
            emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rt)));
            emit(ctx, enc_srawi(SCRATCH_B, SCRATCH_A, 31));
            emit(ctx, enc_stw(SCRATCH_B, CTX_REG, REG_HI(rt)));
            return 0;
        }
        if (rs == 0x04) { /* MTC1: fpr[rd] = rt32 (no sign-extension - a
                            * raw 32-bit bit-pattern copy) */
            emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rt)));
            emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_FPR(rd)));
            return 0;
        }
        if (rs == 0x02) { /* CFC1: rd is a COMPILE-TIME constant (part of
                            * the instruction encoding), so this
                            * specializes to one of three fixed shapes
                            * per rd value rather than branching in
                            * generated code. */
            if (rt == 0)
                return 0;
            if (rd == 31) {
                emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, FCR31_OFFSET));
                emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rt)));
                emit(ctx, enc_srawi(SCRATCH_B, SCRATCH_A, 31));
                emit(ctx, enc_stw(SCRATCH_B, CTX_REG, REG_HI(rt)));
            } else if (rd == 0) {
                /* GPR(rt) = sext32(0x2E00) - positive, so hi is always 0. */
                emit(ctx, enc_addi(SCRATCH_A, 0, 0x2E00));
                emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rt)));
                emit(ctx, enc_addi(SCRATCH_B, 0, 0));
                emit(ctx, enc_stw(SCRATCH_B, CTX_REG, REG_HI(rt)));
            } else {
                emit(ctx, enc_addi(SCRATCH_A, 0, 0));
                emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rt)));
                emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_HI(rt)));
            }
            return 0;
        }
        if (rs == 0x06) { /* CTC1: only rd==31 does anything real
                            * (fcr31 = rt32); every other rd is a
                            * documented no-op, matching ee_core.c's own
                            * `if (rd == 31) ...` guard with no else. */
            if (rd != 31)
                return 0;
            emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rt)));
            emit(ctx, enc_stw(SCRATCH_A, CTX_REG, FCR31_OFFSET));
            return 0;
        }
        if (rs == 0x10) { /* COP1.S - fd=sa, fs=rd, ft=rt (ee_core.c's
                            * own convention, reused verbatim). */
            uint32_t fd = sa, fs = rd;
            if (funct == 0x06) { /* MOV.S: fpr[fd] = fpr[fs] */
                emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_FPR(fs)));
                emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_FPR(fd)));
                return 0;
            }
            if (funct == 0x05) { /* ABS.S: fpr[fd] = fpr[fs] & 0x7fffffff -
                                   * rlwinm with mb=1,me=31,sh=0 clears just
                                   * bit 0 (the sign bit), verified to equal
                                   * AND-with-0x7FFFFFFF exactly. */
                emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_FPR(fs)));
                emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, 0, 1, 31));
                emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_FPR(fd)));
                return 0;
            }
            if (funct == 0x07) { /* NEG.S: fpr[fd] = fpr[fs] ^ 0x80000000 -
                                   * xoris only touches the upper 16 bits,
                                   * exactly the bits 0x80000000 lives in. */
                emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_FPR(fs)));
                emit(ctx, enc_xoris(SCRATCH_A, SCRATCH_A, 0x8000));
                emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_FPR(fd)));
                return 0;
            }
            if (funct == 0x00 || funct == 0x01 || funct == 0x02) {
                /* Round 903 (task #884): ADD.S/SUB.S/MUL.S - this
                 * dynarec's first genuine PPC750 floating-point
                 * arithmetic. ft is COP1.S's `rt` field (fs=rd/fd=sa
                 * already bound above, matching ee_core.c's own
                 * convention for this sub-opcode).
                 *
                 * PPC has no GPR<->FPR move instruction, so every
                 * bit-pattern handoff between the two register files
                 * goes through a small private stack frame (pushed/
                 * popped right here, offset 8 within it, never
                 * assumed shared with any other opcode's frame use -
                 * see LW/SW's own call-trampoline frame for the
                 * unrelated convention those opcodes use instead).
                 * fs/ft are each: load raw bits -> emit_fpu_clamp32()
                 * (reproduces fpu_double()'s denormal/infinity input
                 * handling) -> spill to stack -> lfs into an FPR.
                 * Then the real float op, then the mirror-image
                 * result path: stfs -> lwz -> emit_fpu_clamp32() again
                 * (this time reproducing fpu_check_overflow()+
                 * fpu_check_underflow()'s output clamping - the SAME
                 * transform, see that function's own comment) -> stw
                 * to fpr[fd]. SCRATCH_A/SCRATCH_B hold the clamp
                 * routine's two magnitude-threshold constants,
                 * hoisted once here since they're invariant across
                 * all three clamp calls this opcode makes. */
                emit(ctx, enc_addi(1, 1, -16)); /* push 16-byte scratch frame */
                emit_load_const32(ctx, SCRATCH_A, 0x007FFFFFu); /* K1M1 */
                emit_load_const32(ctx, SCRATCH_B, 0x7F7FFFFFu); /* K2M1 */

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_FPR(fs)));
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(0, 1, 8)); /* f0 = clamped fs */

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_FPR(rt)));
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(1, 1, 8)); /* f1 = clamped ft */

                if (funct == 0x00)
                    emit(ctx, enc_fadds(2, 0, 1));
                else if (funct == 0x01)
                    emit(ctx, enc_fsubs(2, 0, 1));
                else
                    emit(ctx, enc_fmuls(2, 0, 1)); /* fmuls uses frC, not frB */

                emit(ctx, enc_stfs(2, 1, 8));
                emit(ctx, enc_lwz(SCRATCH_C, 1, 8));
                emit_fpu_clamp32(ctx); /* overflow-then-underflow output clamp */
                emit(ctx, enc_stw(SCRATCH_C, CTX_REG, REG_FPR(fd)));

                emit(ctx, enc_addi(1, 1, 16)); /* pop scratch frame */
                return 0;
            }
            if (funct == 0x03) {
                /* Round 904 (task #884): DIV.S. Re-verified against
                 * ee_core.c's real DIV.S body (case 0x03, ~line 7973)
                 * before implementing: unlike ADD.S/SUB.S/MUL.S, the
                 * divide-by-zero condition is tested on the RAW
                 * divisor bits BEFORE any fpu_double() clamp - "if
                 * ((divisor & 0x7F800000) == 0)" (denormal counts as
                 * zero too, matching fpu_double()'s own zero_mask
                 * trigger exactly) - producing a signed +/-Fmax result
                 * whose sign is the XOR of the RAW (unclamped) operand
                 * signs, entirely bypassing the real division. This
                 * can't be reproduced by just letting a real PPC750
                 * fdivs run on the clamped operands and feeding the
                 * IEEE Infinity/NaN result through emit_fpu_clamp32():
                 * that shortcut gives the right answer whenever the
                 * dividend is nonzero (IEEE division's sign-of-
                 * infinity rule already matches the XOR formula), but
                 * is WRONG for the 0/0-class case (both operands
                 * denormal-or-zero) - real PPC hardware's 0/0 produces
                 * a canonical NaN with an implementation-defined sign
                 * bit (typically always positive), not sign=XOR(dividend,
                 * divisor) - so the two paths would disagree on, e.g.,
                 * (-0.0)/(+0.0). Handled instead as an explicit
                 * branchless blend: compute the "would-be" divide-by-
                 * zero result from the RAW operands, compute a mask
                 * for whether the raw divisor's exponent field is
                 * zero, ALSO run the normal clamp->fdivs->clamp path
                 * unconditionally (its result is simply discarded when
                 * the mask fires), then mask-blend the two - preserving
                 * this file's "every compiled block is a straight-line
                 * run" invariant. Uses the already-encoded-but-unused
                 * enc_fdivs from Round 903. Reuses SCRATCH_B for both
                 * K2M1 (the clamp routine's overflow threshold, needed
                 * later) and FPU_POS_FMAX in the divide-by-zero
                 * result - they're bit-identical (0x7F7FFFFF), so
                 * loading it once and using it for both roles is exact,
                 * not a coincidental shortcut. Uses a private 32-byte
                 * stack frame (double the 16 bytes ADD.S/SUB.S/MUL.S
                 * push, since this opcode needs to stash the raw
                 * dividend/divisor plus the divide-by-zero mask/result
                 * alongside the existing GPR<->FPR transfer slot). */
                emit(ctx, enc_addi(1, 1, -32)); /* push 32-byte scratch frame */

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_FPR(fs))); /* raw dividend */
                emit(ctx, enc_stw(SCRATCH_C, 1, 0));
                emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, REG_FPR(rt))); /* raw divisor (ft=rt) */
                emit(ctx, enc_stw(SCRATCH_D, 1, 4));

                emit_load_const32(ctx, SCRATCH_A, 0x007FFFFFu); /* K1M1 */
                emit_load_const32(ctx, SCRATCH_B, 0x7F7FFFFFu); /* K2M1 == FPU_POS_FMAX */

                /* divzero_mask = allOnes iff (divisor & 0x7F800000) == 0 */
                emit(ctx, enc_rlwinm(SCRATCH_E, SCRATCH_D, 0, 1, 8)); /* E = divisor exponent field */
                emit(ctx, enc_addi(SCRATCH_F, 0, 0));                 /* F = 0 */
                emit(ctx, enc_subfc(SCRATCH_G, SCRATCH_E, SCRATCH_F)); /* G=0-E, CA=1 iff E==0 */
                emit(ctx, enc_subfe(SCRATCH_G, SCRATCH_G, SCRATCH_G)); /* G=allOnes iff E!=0, else 0 */
                emit(ctx, enc_nor(SCRATCH_G, SCRATCH_G, SCRATCH_G));   /* flip: G=divzero_mask */
                emit(ctx, enc_stw(SCRATCH_G, 1, 12));

                /* zero_div_result = ((divisor ^ dividend) & 0x80000000) | FMAX */
                emit(ctx, enc_xor(SCRATCH_H, SCRATCH_D, SCRATCH_C));
                emit(ctx, enc_rlwinm(SCRATCH_H, SCRATCH_H, 0, 0, 0)); /* sign bit only */
                emit(ctx, enc_or(SCRATCH_H, SCRATCH_H, SCRATCH_B));   /* | K2M1(=FMAX) */
                emit(ctx, enc_stw(SCRATCH_H, 1, 16));

                /* Normal path: clamp both operands, real fdivs, clamp result -
                 * A/B still hold K1M1/K2M1 untouched by the mask/sign work above. */
                emit(ctx, enc_lwz(SCRATCH_C, 1, 0)); /* reload raw dividend */
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(0, 1, 8)); /* f0 = clamped dividend */

                emit(ctx, enc_lwz(SCRATCH_C, 1, 4)); /* reload raw divisor */
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(1, 1, 8)); /* f1 = clamped divisor */

                emit(ctx, enc_fdivs(2, 0, 1));
                emit(ctx, enc_stfs(2, 1, 8));
                emit(ctx, enc_lwz(SCRATCH_C, 1, 8));
                emit_fpu_clamp32(ctx); /* SCRATCH_C = clamped normal-path result */

                /* Blend: final = divzero_mask ? zero_div_result : normal_result */
                emit(ctx, enc_lwz(SCRATCH_D, 1, 12)); /* divzero_mask */
                emit(ctx, enc_lwz(SCRATCH_E, 1, 16)); /* zero_div_result */
                emit(ctx, enc_nor(SCRATCH_F, SCRATCH_D, SCRATCH_D)); /* notmask */
                emit(ctx, enc_and(SCRATCH_E, SCRATCH_E, SCRATCH_D));
                emit(ctx, enc_and(SCRATCH_C, SCRATCH_C, SCRATCH_F));
                emit(ctx, enc_or(SCRATCH_C, SCRATCH_C, SCRATCH_E));
                emit(ctx, enc_stw(SCRATCH_C, CTX_REG, REG_FPR(fd)));

                emit(ctx, enc_addi(1, 1, 32)); /* pop scratch frame */
                return 0;
            }
            if (funct == 0x04) {
                /* Round 905 (task #889): SQRT.S. Re-verified against
                 * ee_core.c's real SQRT.S body (case 0x04, ~line 7996):
                 * the source operand is `ft` (COP1.S's `rt` field) -
                 * fs/rd are UNUSED, a documented real-hardware/PCSX2
                 * quirk carried over exactly, not a copy typo (SQRT.S
                 * computes sqrt(ft); fs plays no part). Raw ft's
                 * exponent field is tested for zero (denormal counts as
                 * zero, matching fpu_double()'s own zero_mask trigger)
                 * BEFORE any clamp: on a hit the result is simply ft's
                 * own sign bit (signed zero), skipping sqrtf entirely.
                 * The two nonzero-exponent branches in ee_core.c (one
                 * for ft negative, one for ft non-negative) are
                 * mathematically identical - both reduce to
                 * `sqrtf(fabsf(fpu_double(ft)))` - so this dynarec
                 * computes that single expression unconditionally for
                 * the normal path and blends it against the signed-zero
                 * special case, same branchless-blend structure as
                 * DIV.S. No output clamp is applied afterward (verified
                 * absent from the real case body) - sqrtf() of a
                 * clamped, non-negative, non-huge input can't overflow
                 * float32 range, so none is needed.
                 *
                 * Real PPC750/Gekko hardware does not safely support
                 * fsqrts/fsqrt (see ADDR_EE_SQRTF's own comment above
                 * for the empirical GCC/libm-disassembly evidence this
                 * round found) - this calls the real linked sqrtf()
                 * through a C-function-call trampoline instead of
                 * emitting a hardware sqrt instruction. Unlike the
                 * LW/SW trampolines (which pass ctx as an integer arg in
                 * r3 and get an integer result back in r3), this call's
                 * argument and return value are both a single float in
                 * f1 (the EABI's first float arg/return register) -
                 * CTX_REG itself (r3) carries no argument here, but per
                 * the EABI r3-r12 are ALL still caller-saved/volatile
                 * across the call (sqrtf is free to use any of them as
                 * scratch), so CTX_REG must be saved into r15
                 * (non-volatile) before the call and restored after -
                 * same LR-via-r14/ctx-via-r15 convention as LW/SW's own
                 * trampoline, just applied here because a plain float
                 * call still can't be trusted not to clobber r3, not
                 * because this call takes ctx as an argument. */
                emit(ctx, enc_addi(1, 1, -32)); /* push 32-byte scratch frame */

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_FPR(rt))); /* raw ft (source, NOT fs) */
                emit(ctx, enc_stw(SCRATCH_C, 1, 0));

                emit_load_const32(ctx, SCRATCH_A, 0x007FFFFFu); /* K1M1 */
                emit_load_const32(ctx, SCRATCH_B, 0x7F7FFFFFu); /* K2M1 */

                /* iszero_mask = allOnes iff (ft & 0x7F800000) == 0 */
                emit(ctx, enc_rlwinm(SCRATCH_E, SCRATCH_C, 0, 1, 8));
                emit(ctx, enc_addi(SCRATCH_F, 0, 0));
                emit(ctx, enc_subfc(SCRATCH_G, SCRATCH_E, SCRATCH_F));
                emit(ctx, enc_subfe(SCRATCH_G, SCRATCH_G, SCRATCH_G));
                emit(ctx, enc_nor(SCRATCH_G, SCRATCH_G, SCRATCH_G));   /* G = iszero_mask */
                emit(ctx, enc_stw(SCRATCH_G, 1, 4));

                /* special_result = ft_raw & 0x80000000 (signed zero) */
                emit(ctx, enc_rlwinm(SCRATCH_H, SCRATCH_C, 0, 0, 0));
                emit(ctx, enc_stw(SCRATCH_H, 1, 8));

                /* Normal path operand: fabsf(fpu_double(ft)) - clamp then clear sign */
                emit(ctx, enc_lwz(SCRATCH_C, 1, 0)); /* reload raw ft */
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_rlwinm(SCRATCH_C, SCRATCH_C, 0, 1, 31)); /* fabsf: clear sign bit */
                emit(ctx, enc_stw(SCRATCH_C, 1, 12));
                emit(ctx, enc_lfs(1, 1, 12)); /* f1 = fabsf(fpu_double(ft)) - EABI float arg reg */

                emit(ctx, enc_stw(14, 1, 16)); /* save caller's r14 */
                emit(ctx, enc_stw(15, 1, 20)); /* save caller's r15 */
                emit(ctx, enc_or(15, CTX_REG, CTX_REG)); /* r15 = ctx (mr r15,r3) */
                emit(ctx, enc_mflr(14));                 /* r14 = this block's real return address */
                emit_load_const32(ctx, 12, ADDR_EE_SQRTF);
                emit(ctx, enc_mtctr(12));
                emit(ctx, enc_bctrl());                  /* f1 = sqrtf(f1) */
                emit(ctx, enc_mtlr(14));                 /* restore this block's real return address */
                emit(ctx, enc_or(CTX_REG, 15, 15));      /* restore ctx into r3 (mr r3,r15) - r3 is
                                                           * volatile, sqrtf() may have clobbered it */
                emit(ctx, enc_lwz(14, 1, 16));           /* restore caller's r14 */
                emit(ctx, enc_lwz(15, 1, 20));           /* restore caller's r15 */

                emit(ctx, enc_stfs(1, 1, 12));           /* spill sqrtf's f1 result */
                emit(ctx, enc_lwz(SCRATCH_C, 1, 12));    /* SCRATCH_C = normal-path result bits */

                /* Blend: final = iszero_mask ? special_result : normal_result */
                emit(ctx, enc_lwz(SCRATCH_D, 1, 4));  /* iszero_mask */
                emit(ctx, enc_lwz(SCRATCH_E, 1, 8));  /* special_result */
                emit(ctx, enc_nor(SCRATCH_F, SCRATCH_D, SCRATCH_D));
                emit(ctx, enc_and(SCRATCH_E, SCRATCH_E, SCRATCH_D));
                emit(ctx, enc_and(SCRATCH_C, SCRATCH_C, SCRATCH_F));
                emit(ctx, enc_or(SCRATCH_C, SCRATCH_C, SCRATCH_E));
                emit(ctx, enc_stw(SCRATCH_C, CTX_REG, REG_FPR(fd)));

                emit(ctx, enc_addi(1, 1, 32)); /* pop scratch frame */
                return 0;
            }
            if (funct == 0x16) {
                /* Round 905 (task #889): RSQRT.S = fs / sqrt(ft).
                 * Re-verified against ee_core.c's real RSQRT.S body
                 * (case 0x16, ~line 8018): same raw-ft-exponent-zero
                 * special case as SQRT.S (denormal counts as zero), but
                 * the special result differs in TWO ways from DIV.S's
                 * divide-by-zero case: it's (ft_sign)|FMAX with NO XOR
                 * against fs's sign (verified directly from source -
                 * easy to get wrong by pattern-matching DIV.S's XOR
                 * instead of re-reading), and there's no distinct
                 * "0/0-class" wrinkle to prove here since only ft's
                 * exponent is ever tested. The two nonzero-exponent
                 * branches again reduce to one shared expression for
                 * the denominator - denom = sqrtf(fabsf(fpu_double(ft))),
                 * byte-for-byte the same computation SQRT.S's own
                 * normal path makes (reused here, not re-derived) - then
                 * the normal-path result is fpu_double(fs) / denom, WITH
                 * the standard overflow-then-underflow output clamp
                 * applied afterward (SQRT.S has none, this opcode does -
                 * confirmed present in both non-zero-exponent branches
                 * of the real case body).
                 *
                 * Same sqrtf() call-trampoline convention as SQRT.S (ctx
                 * saved into r15/LR into r14 across the call, restored
                 * after - see that block's own comment for why).
                 * IMPORTANT: SCRATCH_A/SCRATCH_B (K1M1/K2M1) do NOT
                 * survive the call - r3-r12 are all EABI volatile,
                 * sqrtf() is free to clobber any of them - so they're
                 * reloaded via emit_load_const32 a second time after the
                 * call, immediately before the fs-clamp and (later) the
                 * output clamp that both need them. Uses a 32-byte frame
                 * like SQRT.S, with one extra slot to stash the sqrtf()
                 * result (denom) across the post-call fs-clamp/lfs
                 * sequence before the final fdivs. */
                emit(ctx, enc_addi(1, 1, -32)); /* push 32-byte scratch frame */

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_FPR(rt))); /* raw ft */
                emit(ctx, enc_stw(SCRATCH_C, 1, 0));

                emit_load_const32(ctx, SCRATCH_A, 0x007FFFFFu); /* K1M1 */
                emit_load_const32(ctx, SCRATCH_B, 0x7F7FFFFFu); /* K2M1 == FPU_POS_FMAX */

                /* iszero_mask = allOnes iff (ft & 0x7F800000) == 0 */
                emit(ctx, enc_rlwinm(SCRATCH_E, SCRATCH_C, 0, 1, 8));
                emit(ctx, enc_addi(SCRATCH_F, 0, 0));
                emit(ctx, enc_subfc(SCRATCH_G, SCRATCH_E, SCRATCH_F));
                emit(ctx, enc_subfe(SCRATCH_G, SCRATCH_G, SCRATCH_G));
                emit(ctx, enc_nor(SCRATCH_G, SCRATCH_G, SCRATCH_G));   /* G = iszero_mask */
                emit(ctx, enc_stw(SCRATCH_G, 1, 4));

                /* special_result = (ft_sign) | FMAX - NO xor with fs, unlike DIV.S */
                emit(ctx, enc_rlwinm(SCRATCH_H, SCRATCH_C, 0, 0, 0)); /* ft sign bit only */
                emit(ctx, enc_or(SCRATCH_H, SCRATCH_H, SCRATCH_B));   /* | K2M1(=FMAX) */
                emit(ctx, enc_stw(SCRATCH_H, 1, 8));

                /* denom operand: fabsf(fpu_double(ft)) - identical to SQRT.S's normal path */
                emit(ctx, enc_lwz(SCRATCH_C, 1, 0)); /* reload raw ft */
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_rlwinm(SCRATCH_C, SCRATCH_C, 0, 1, 31)); /* fabsf */
                emit(ctx, enc_stw(SCRATCH_C, 1, 12));
                emit(ctx, enc_lfs(1, 1, 12)); /* f1 = fabsf(fpu_double(ft)) */

                emit(ctx, enc_stw(14, 1, 16)); /* save caller's r14 */
                emit(ctx, enc_stw(15, 1, 20)); /* save caller's r15 */
                emit(ctx, enc_or(15, CTX_REG, CTX_REG));
                emit(ctx, enc_mflr(14));
                emit_load_const32(ctx, 12, ADDR_EE_SQRTF);
                emit(ctx, enc_mtctr(12));
                emit(ctx, enc_bctrl());                  /* f1 = sqrtf(f1) = denom */
                emit(ctx, enc_mtlr(14));
                emit(ctx, enc_or(CTX_REG, 15, 15));      /* restore ctx into r3 */
                emit(ctx, enc_lwz(14, 1, 16));
                emit(ctx, enc_lwz(15, 1, 20));

                emit(ctx, enc_stfs(1, 1, 24));           /* spill denom to its own slot */

                /* Reload K1M1/K2M1 - clobbered by the call, needed again below */
                emit_load_const32(ctx, SCRATCH_A, 0x007FFFFFu);
                emit_load_const32(ctx, SCRATCH_B, 0x7F7FFFFFu);

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_FPR(fs))); /* raw fs (dividend) */
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 12));
                emit(ctx, enc_lfs(0, 1, 12)); /* f0 = clamped fs */
                emit(ctx, enc_lfs(1, 1, 24)); /* f1 = denom */

                emit(ctx, enc_fdivs(2, 0, 1));           /* f2 = fs / denom */
                emit(ctx, enc_stfs(2, 1, 12));
                emit(ctx, enc_lwz(SCRATCH_C, 1, 12));
                emit_fpu_clamp32(ctx); /* overflow-then-underflow output clamp */

                /* Blend: final = iszero_mask ? special_result : normal_result */
                emit(ctx, enc_lwz(SCRATCH_D, 1, 4));  /* iszero_mask */
                emit(ctx, enc_lwz(SCRATCH_E, 1, 8));  /* special_result */
                emit(ctx, enc_nor(SCRATCH_F, SCRATCH_D, SCRATCH_D));
                emit(ctx, enc_and(SCRATCH_E, SCRATCH_E, SCRATCH_D));
                emit(ctx, enc_and(SCRATCH_C, SCRATCH_C, SCRATCH_F));
                emit(ctx, enc_or(SCRATCH_C, SCRATCH_C, SCRATCH_E));
                emit(ctx, enc_stw(SCRATCH_C, CTX_REG, REG_FPR(fd)));

                emit(ctx, enc_addi(1, 1, 32)); /* pop scratch frame */
                return 0;
            }
            if (funct == 0x28 || funct == 0x29) {
                /* Round 905 (task #889): MAX.S/MIN.S. Re-verified
                 * against ee_core.c's real bodies (case 0x28/0x29,
                 * ~line 8038): `fpr[fd] = fp_max(fpr[fs], fpr[ft])` /
                 * `fp_min(...)` - a bit-level SIGNED-32-bit-int max/min
                 * on the RAW register contents, ported straight from
                 * PCSX2's FPU.cpp, with NO fpu_double() input clamp and
                 * NO output clamp anywhere in either real case body
                 * (unlike every other COP1.S arithmetic opcode this
                 * dynarec has shipped so far) - confirmed by re-reading,
                 * not assumed by analogy. fp_max/fp_min both special-
                 * case "both operands negative" (sa<0 && sb<0) to invert
                 * which raw signed-int comparison they use - this is NOT
                 * a plain IEEE-monotonic bit-pattern trick (that only
                 * holds when both values share the same sign or one is
                 * non-negative), so no shortcut is available: this
                 * needs a genuine signed compare, not just a select on
                 * the sign bits.
                 *
                 * Computed as two independent branchless masks - less_mask
                 * (allOnes iff sa<sb) and greater_mask (allOnes iff
                 * sa>sb) - via the exact same sign-flip + subfc/subfe
                 * borrow-to-mask idiom emit_slt_core uses for its signed
                 * SLT path (just on a single 32-bit word here, not a
                 * 64-bit hi/lo pair, since fp_max/fp_min operate on
                 * plain 32-bit ints), plus a bothneg_mask from the two
                 * operands' sign bits via srawi-by-31 (the same
                 * fill-word idiom LW/ADDIU/etc use for sign-extension).
                 * MAX.S and MIN.S share this entire mask/pick computation
                 * - they only differ in which pick (min or max of the
                 * raw signed ints) gets used for the bothneg case vs the
                 * default case, so both funct values are handled in one
                 * block with a plain C-level (compile-time) branch for
                 * that last step, not two near-duplicate blocks. No FPRs
                 * or stack frame are touched at all - pure GPR bitwise
                 * ops on the raw register contents. */
                emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_FPR(fs))); /* a = fpr[fs] raw */
                emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_FPR(rt)));  /* b = fpr[ft] raw (ft=rt) */

                emit(ctx, enc_srawi(SCRATCH_F, SCRATCH_A, 31)); /* signA: allOnes iff a<0 */
                emit(ctx, enc_srawi(SCRATCH_G, SCRATCH_B, 31)); /* signB: allOnes iff b<0 */
                emit(ctx, enc_and(SCRATCH_C, SCRATCH_F, SCRATCH_G)); /* C = bothneg_mask */

                emit(ctx, enc_xoris(SCRATCH_F, SCRATCH_A, 0x8000)); /* flippedA */
                emit(ctx, enc_xoris(SCRATCH_G, SCRATCH_B, 0x8000)); /* flippedB */

                emit(ctx, enc_subfc(SCRATCH_H, SCRATCH_G, SCRATCH_F)); /* H=flippedA-flippedB */
                emit(ctx, enc_subfe(SCRATCH_D, SCRATCH_H, SCRATCH_H)); /* D = less_mask (sa<sb) */

                emit(ctx, enc_subfc(SCRATCH_H, SCRATCH_F, SCRATCH_G)); /* H=flippedB-flippedA */
                emit(ctx, enc_subfe(SCRATCH_E, SCRATCH_H, SCRATCH_H)); /* E = greater_mask (sa>sb) */

                /* pick_min = D? a : b -> F */
                emit(ctx, enc_nor(SCRATCH_H, SCRATCH_D, SCRATCH_D));
                emit(ctx, enc_and(SCRATCH_F, SCRATCH_A, SCRATCH_D));
                emit(ctx, enc_and(SCRATCH_H, SCRATCH_B, SCRATCH_H));
                emit(ctx, enc_or(SCRATCH_F, SCRATCH_F, SCRATCH_H));    /* F = pick_min */

                /* pick_max = E? a : b -> G */
                emit(ctx, enc_nor(SCRATCH_H, SCRATCH_E, SCRATCH_E));
                emit(ctx, enc_and(SCRATCH_G, SCRATCH_A, SCRATCH_E));
                emit(ctx, enc_and(SCRATCH_H, SCRATCH_B, SCRATCH_H));
                emit(ctx, enc_or(SCRATCH_G, SCRATCH_G, SCRATCH_H));    /* G = pick_max */

                if (funct == 0x28) {
                    /* MAX.S: bothneg -> pick_min(F), else -> pick_max(G) */
                    emit(ctx, enc_nor(SCRATCH_H, SCRATCH_C, SCRATCH_C));
                    emit(ctx, enc_and(SCRATCH_F, SCRATCH_F, SCRATCH_C));
                    emit(ctx, enc_and(SCRATCH_G, SCRATCH_G, SCRATCH_H));
                    emit(ctx, enc_or(SCRATCH_F, SCRATCH_F, SCRATCH_G));
                } else {
                    /* MIN.S: bothneg -> pick_max(G), else -> pick_min(F) */
                    emit(ctx, enc_nor(SCRATCH_H, SCRATCH_C, SCRATCH_C));
                    emit(ctx, enc_and(SCRATCH_G, SCRATCH_G, SCRATCH_C));
                    emit(ctx, enc_and(SCRATCH_F, SCRATCH_F, SCRATCH_H));
                    emit(ctx, enc_or(SCRATCH_F, SCRATCH_F, SCRATCH_G));
                }
                emit(ctx, enc_stw(SCRATCH_F, CTX_REG, REG_FPR(fd)));
                return 0;
            }
            if (funct == 0x18 || funct == 0x19 || funct == 0x1A) {
                /* Round 906 (task #890): ADDA.S/SUBA.S/MULA.S - ACC =
                 * fs OP ft, ported from PCSX2's ADDA_S()/SUBA_S()/
                 * MULA_S(). Re-verified against ee_core.c's real case
                 * bodies (~lines 8048-8066): identical to Round 903's
                 * ADD.S/SUB.S/MUL.S clamp->op->clamp structure, the
                 * ONLY difference is the destination - ACC_OFFSET
                 * instead of fpr[fd] (this sub-opcode's `sa`/fd field
                 * is unused; ACC is a single fixed register, not
                 * selected by any instruction field). */
                emit(ctx, enc_addi(1, 1, -16));
                emit_load_const32(ctx, SCRATCH_A, 0x007FFFFFu);
                emit_load_const32(ctx, SCRATCH_B, 0x7F7FFFFFu);

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_FPR(fs)));
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(0, 1, 8));

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_FPR(rt)));
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(1, 1, 8));

                if (funct == 0x18)
                    emit(ctx, enc_fadds(2, 0, 1));
                else if (funct == 0x19)
                    emit(ctx, enc_fsubs(2, 0, 1));
                else
                    emit(ctx, enc_fmuls(2, 0, 1));

                emit(ctx, enc_stfs(2, 1, 8));
                emit(ctx, enc_lwz(SCRATCH_C, 1, 8));
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, CTX_REG, ACC_OFFSET)); /* -> ACC, not fpr[fd] */

                emit(ctx, enc_addi(1, 1, 16));
                return 0;
            }
            if (funct == 0x1E || funct == 0x1F) {
                /* Round 906 (task #890): MADDA.S/MSUBA.S - ACC =
                 * fpu_double(ACC) +/- (fpu_double(fs)*fpu_double(ft)),
                 * ported from PCSX2's MADDA_S()/MSUBA_S(). Re-verified
                 * against ee_core.c (~lines 8094-8108): the intermediate
                 * product is used DIRECTLY in the add/sub, with NO
                 * second fpu_double() pass on it - unlike MADD.S/MSUB.S
                 * below, which DO reclamp the product. That asymmetry
                 * is real (matches PCSX2 exactly), not an oversight to
                 * "fix" by making it consistent with MADD.S. */
                emit(ctx, enc_addi(1, 1, -16));
                emit_load_const32(ctx, SCRATCH_A, 0x007FFFFFu);
                emit_load_const32(ctx, SCRATCH_B, 0x7F7FFFFFu);

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_FPR(fs))); /* f0 = clamped fs */
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(0, 1, 8));

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_FPR(rt))); /* f1 = clamped ft */
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(1, 1, 8));

                emit(ctx, enc_fmuls(2, 0, 1)); /* f2 = product, NOT reclamped */

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, ACC_OFFSET)); /* f1 = clamped acc (reuse f1) */
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(1, 1, 8));

                if (funct == 0x1E)
                    emit(ctx, enc_fadds(3, 1, 2)); /* f3 = acc + product */
                else
                    emit(ctx, enc_fsubs(3, 1, 2)); /* f3 = acc - product */

                emit(ctx, enc_stfs(3, 1, 8));
                emit(ctx, enc_lwz(SCRATCH_C, 1, 8));
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, CTX_REG, ACC_OFFSET));

                emit(ctx, enc_addi(1, 1, 16));
                return 0;
            }
            if (funct == 0x1C || funct == 0x1D) {
                /* Round 906 (task #890): MADD.S/MSUB.S - fd = ACC +/-
                 * (fs*ft), ported from PCSX2's MADD_S()/MSUB_S().
                 * Re-verified against ee_core.c (~lines 8067-8093): real
                 * hardware/PCSX2 quirk worth preserving exactly - the
                 * intermediate product IS run through fpu_double() a
                 * SECOND time when read back for the add/sub (PCSX2's
                 * own FPRreg temp: temp.f = fpuDouble(fs)*fpuDouble(ft);
                 * then fpuDouble(temp.UL) again) - unlike MADDA.S/
                 * MSUBA.S above, which don't do this second pass. Not a
                 * simplification target - ported as-is, verified against
                 * source rather than assumed consistent with the ACC
                 * variants. Destination is fpr[fd] (a real register
                 * field this time), not ACC. */
                emit(ctx, enc_addi(1, 1, -16));
                emit_load_const32(ctx, SCRATCH_A, 0x007FFFFFu);
                emit_load_const32(ctx, SCRATCH_B, 0x7F7FFFFFu);

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_FPR(fs))); /* f0 = clamped fs */
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(0, 1, 8));

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_FPR(rt))); /* f1 = clamped ft */
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(1, 1, 8));

                emit(ctx, enc_fmuls(2, 0, 1)); /* f2 = product */
                emit(ctx, enc_stfs(2, 1, 8));
                emit(ctx, enc_lwz(SCRATCH_C, 1, 8));
                emit_fpu_clamp32(ctx); /* SECOND clamp pass on the product - the MADD.S/MSUB.S quirk */
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(0, 1, 8)); /* f0 = re-clamped product (reuse f0) */

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, ACC_OFFSET)); /* f1 = clamped acc (reuse f1) */
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(1, 1, 8));

                if (funct == 0x1C)
                    emit(ctx, enc_fadds(2, 1, 0)); /* f2 = acc + reclamped_product */
                else
                    emit(ctx, enc_fsubs(2, 1, 0)); /* f2 = acc - reclamped_product */

                emit(ctx, enc_stfs(2, 1, 8));
                emit(ctx, enc_lwz(SCRATCH_C, 1, 8));
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, CTX_REG, REG_FPR(fd))); /* -> fd, NOT ACC */

                emit(ctx, enc_addi(1, 1, 16));
                return 0;
            }
            if (funct == 0x32 || funct == 0x34 || funct == 0x36) {
                /* Round 906 (task #890): C.EQ.S/C.LT.S/C.LE.S -
                 * ee_core.c's real bodies (~lines 8110-8121) compare
                 * fpu_double(fs) against fpu_double(ft) with a plain C
                 * `==`/`<`/`<=` and set-or-clear fcr31 bit 0x00800000
                 * (bit23) accordingly - no third "unordered" outcome is
                 * ever reachable in this dynarec's version, since
                 * emit_fpu_clamp32() already collapses every NaN input
                 * to a signed Fmax before either operand can reach the
                 * compare. Uses real hardware fcmpu (this dynarec's
                 * first CR-based instruction) rather than a hand-rolled
                 * integer bit-pattern ordering trick - deliberately, to
                 * get -0.0==+0.0 correct for free instead of having to
                 * special-case it (see enc_fcmpu's own comment above).
                 * mfcr pulls CR0's FL/FG/FE bits into a GPR; each is
                 * extracted to a clean 0/1 via enc_rlwinm's rotate-then-
                 * mask-to-bit0 idiom (same style already used throughout
                 * this file's mask-based branchless-select code). C.LE.S
                 * needs both FL and FE, ORed together. The final bit
                 * write is a branchless read-clear-OR sequence on
                 * fcr31, touching only bit23 - every other fcr31 bit
                 * (rounding mode etc.) survives unmodified, matching
                 * the real case bodies' own `|=`/`&= ~` pattern exactly. */
                emit(ctx, enc_addi(1, 1, -16));
                emit_load_const32(ctx, SCRATCH_A, 0x007FFFFFu);
                emit_load_const32(ctx, SCRATCH_B, 0x7F7FFFFFu);

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_FPR(fs))); /* f0 = clamped fs */
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(0, 1, 8));

                emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_FPR(rt))); /* f1 = clamped ft */
                emit_fpu_clamp32(ctx);
                emit(ctx, enc_stw(SCRATCH_C, 1, 8));
                emit(ctx, enc_lfs(1, 1, 8));

                emit(ctx, enc_fcmpu(0, 0, 1)); /* cr0 = compare(f0, f1) */
                emit(ctx, enc_mfcr(SCRATCH_D)); /* SCRATCH_D bits31/30/29 = FL/FG/FE */

                if (funct == 0x32) { /* C.EQ.S: want FE (IBM bit2 = normal bit29) */
                    emit(ctx, enc_rlwinm(SCRATCH_E, SCRATCH_D, 3, 31, 31));
                } else if (funct == 0x34) { /* C.LT.S: want FL (IBM bit0 = normal bit31) */
                    emit(ctx, enc_rlwinm(SCRATCH_E, SCRATCH_D, 1, 31, 31));
                } else { /* C.LE.S: FL | FE */
                    emit(ctx, enc_rlwinm(SCRATCH_F, SCRATCH_D, 1, 31, 31)); /* FL */
                    emit(ctx, enc_rlwinm(SCRATCH_G, SCRATCH_D, 3, 31, 31)); /* FE */
                    emit(ctx, enc_or(SCRATCH_E, SCRATCH_F, SCRATCH_G));
                }

                emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, FCR31_OFFSET));
                emit(ctx, enc_rlwinm(SCRATCH_D, SCRATCH_D, 0, 9, 7));   /* clear bit23 (wrap-mask keep-all-but) */
                emit(ctx, enc_rlwinm(SCRATCH_E, SCRATCH_E, 23, 0, 8));  /* cond << 23 */
                emit(ctx, enc_or(SCRATCH_D, SCRATCH_D, SCRATCH_E));
                emit(ctx, enc_stw(SCRATCH_D, CTX_REG, FCR31_OFFSET));

                emit(ctx, enc_addi(1, 1, 16));
                return 0;
            }
            if (funct == 0x24) {
                /* Round 906b (task #891): CVT.W.S (float -> int32).
                 * Re-verified against ee_core.c's real case 0x24 body
                 * (~lines 7989-7995): tests raw fpr[fs]'s exponent field
                 * (bits 30-23, no fpu_double() clamp involved anywhere
                 * in the real body - confirmed absent) against threshold
                 * 0x4E800000; within range, the result is a plain C
                 * `(int32_t)(float)` truncating cast (round toward
                 * zero); out of range, the result saturates to
                 * INT32_MIN/INT32_MAX by sign. Uses fctiwz (PowerPC
                 * Book I base ISA, present on Gekko/Broadway - unlike
                 * fsqrt, this direction of float<->int conversion was
                 * never one of the omitted ops) instead of a call
                 * trampoline: fctiwz converts the double-promoted value
                 * in a source FPR to a 32-bit integer (round toward
                 * zero) stored in the LOW 32 bits of the destination FPR
                 * (high 32 bits undefined per the ISA), so the result
                 * has to be spilled via stfd and read back as a plain
                 * word - there's no direct FPR->GPR move instruction.
                 * fctiwz runs UNCONDITIONALLY (even for the out-of-range
                 * case, whose result the blend below discards) - this is
                 * safe because FPSCR's VE (invalid-operation-exception-
                 * enable) bit defaults to 0 and is never touched by this
                 * dynarec, so an out-of-range or NaN input to fctiwz
                 * just sets a sticky FPSCR flag (unused by this
                 * emulator, like every other FPU exception-cause flag
                 * documented as unmodeled elsewhere in this file) rather
                 * than trapping - matches this whole file's existing
                 * "don't model FPU exception control" simplification. */
                emit(ctx, enc_addi(1, 1, -16));

                emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_FPR(fs))); /* raw fs bits */

                /* mag_exp = raw & 0x7F800000 (exponent field only) */
                emit(ctx, enc_rlwinm(SCRATCH_B, SCRATCH_A, 0, 1, 8));

                /* gt_mask = allOnes iff mag_exp > 0x4E800000 (OUT of range).
                 * subfc(D,B,C) computes D=C-B with CA=1 iff C>=B (i.e. iff
                 * threshold>=mag_exp, i.e. in-range); subfe(E,D,D) then
                 * turns that CA into -1+CA (0 when CA=1/in-range, allOnes
                 * when CA=0/out-of-range) - the standard "borrow-to-mask"
                 * idiom, which yields the OUT-of-range mask, not le_mask
                 * (re-verified by direct hex trace: mag_exp=0x3F800000
                 * (in-range) -> CA=1 -> E=0; mag_exp=0x7F000000 (out-of-
                 * range) -> CA=0 -> E=0xFFFFFFFF - confirmed against the
                 * host-native harness too, see r906b_cop1_cvt_bc1_verify.c).
                 * The blend below is written accordingly: normal_val gets
                 * masked by ~E (in-range), clamp_val by E (out-of-range). */
                emit_load_const32(ctx, SCRATCH_C, 0x4E800000u);
                emit(ctx, enc_subfc(SCRATCH_D, SCRATCH_B, SCRATCH_C));
                emit(ctx, enc_subfe(SCRATCH_E, SCRATCH_D, SCRATCH_D));

                /* clamp_val = 0x7fffffff ^ signmask; signmask = srawi(raw,31) -
                 * yields 0x7fffffff for fs>=0, 0x80000000 for fs<0 (since
                 * 0x7fffffff ^ 0xFFFFFFFF == 0x80000000 exactly). */
                emit(ctx, enc_srawi(SCRATCH_F, SCRATCH_A, 31));
                emit_load_const32(ctx, SCRATCH_G, 0x7fffffffu);
                emit(ctx, enc_xor(SCRATCH_G, SCRATCH_G, SCRATCH_F));

                /* normal path: fctiwz on the double-promoted fs value */
                emit(ctx, enc_lfs(0, CTX_REG, REG_FPR(fs))); /* f0 = fpr[fs] (single->double promote on load) */
                emit(ctx, enc_fctiwz(1, 0));                 /* f1's low word = truncated int32 */
                emit(ctx, enc_stfd(1, 1, 0));                /* spill 8 bytes at frame offset 0 */
                emit(ctx, enc_lwz(SCRATCH_H, 1, 4));          /* SCRATCH_H = normal_val (low word) */

                /* Blend: final = gt_mask(E) ? clamp_val : normal_val - i.e.
                 * normal_val is masked by ~E (in-range, SCRATCH_D) and
                 * clamp_val by E (out-of-range) directly. (Round 906b
                 * follow-up fix: the original blend had normal_val masked
                 * by E and clamp_val by ~D, which is backwards given E's
                 * real polarity as derived above - caught by
                 * r906b_cop1_cvt_bc1_verify.c's in-range test cases,
                 * which failed 4/4 before this fix and pass after.) */
                emit(ctx, enc_nor(SCRATCH_D, SCRATCH_E, SCRATCH_E)); /* notmask = in-range mask */
                emit(ctx, enc_and(SCRATCH_H, SCRATCH_H, SCRATCH_D));
                emit(ctx, enc_and(SCRATCH_G, SCRATCH_G, SCRATCH_E));
                emit(ctx, enc_or(SCRATCH_H, SCRATCH_H, SCRATCH_G));
                emit(ctx, enc_stw(SCRATCH_H, CTX_REG, REG_FPR(fd)));

                emit(ctx, enc_addi(1, 1, 16));
                return 0;
            }
            return -1; /* every other COP1.S funct: not yet JIT-compiled,
                        * fall back to the interpreter. */
        }
        if (rs == 0x14) {
            /* Round 906b (task #891): COP1.W - only CVT.S.W (int32 ->
             * float) is real here, matching ee_core.c's own
             * `case 0x14:` body (~lines 8127-8135) which halts on any
             * other funct. fd=sa, fs=rd (rt unused), same fd/fs
             * convention COP1.S already uses. */
            if (funct != 0x20)
                return -1;
            uint32_t fd = sa, fs = rd;

            /* Round 906b (task #891): CVT.S.W (int32 -> float) needs a
             * genuine int->float CONVERSION that real PPC750/Gekko
             * hardware simply cannot do (fcfid postdates this chip's ISA
             * generation - see ADDR_EE_CVT_S_W's own comment above for
             * why). Calls the trivial C helper ee_jit_cvt_s_w_helper()
             * through the same call-trampoline convention SQRT.S/RSQRT.S
             * already established, but with an INTEGER argument in r3
             * (not a float in f1) and a float RETURN in f1 - standard
             * EABI assigns argument registers independently per type, so
             * this "mixed" signature needs no new convention, just the
             * same ctx-via-r15/LR-via-r14 save/restore across the call. */
            emit(ctx, enc_addi(1, 1, -16));
            emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_FPR(fs))); /* raw int32 bits = arg */
            emit(ctx, enc_stw(SCRATCH_A, 1, 8));

            emit(ctx, enc_stw(14, 1, 0)); /* save caller's r14 */
            emit(ctx, enc_stw(15, 1, 4)); /* save caller's r15 */
            emit(ctx, enc_or(15, CTX_REG, CTX_REG)); /* r15 = ctx (mr r15,r3) */
            emit(ctx, enc_mflr(14));                 /* r14 = this block's real return address */
            emit(ctx, enc_lwz(3, 1, 8));              /* r3 = int32 argument (overwrites ctx - saved in r15) */
            emit_load_const32(ctx, 12, ADDR_EE_CVT_S_W);
            emit(ctx, enc_mtctr(12));
            emit(ctx, enc_bctrl());                  /* f1 = ee_jit_cvt_s_w_helper(r3) */
            emit(ctx, enc_mtlr(14));                 /* restore this block's real return address */
            emit(ctx, enc_or(CTX_REG, 15, 15));      /* restore ctx into r3 (mr r3,r15) */
            emit(ctx, enc_lwz(14, 1, 0));             /* restore caller's r14 */
            emit(ctx, enc_lwz(15, 1, 4));             /* restore caller's r15 */

            emit(ctx, enc_stfs(1, 1, 8));             /* spill helper's f1 result */
            emit(ctx, enc_lwz(SCRATCH_A, 1, 8));      /* SCRATCH_A = result bits */
            emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_FPR(fd)));

            emit(ctx, enc_addi(1, 1, 16));
            return 0;
        }
        if (rs == 0x08) {
            /* Round 906b (task #891): BC1F/BC1T/BC1FL/BC1TL - branch on
             * the FP condition flag (fcr31 bit 0x00800000, set by
             * C.EQ/LT/LE.S). Re-verified against ee_core.c's real
             * case 0x08 body (~lines 8136-8186): rt selects the
             * sub-variant (0=BC1F,1=BC1T,2=BC1FL,3=BC1TL), matching
             * PCSX2's tbl_COP1_BC1[32] convention already cited there.
             * The "likely" pair (BC1FL/BC1TL) uses the exact same
             * nullify-delay-slot-on-not-taken semantics as every other
             * likely branch already JIT'd in this file (BEQL/BNEL/etc,
             * Round 896) - routed through the SAME emit_branch_blend_
             * likely() helper, not a new mechanism.
             *
             * Condition mask is computed branchlessly: shifting fcr31
             * left by 8 moves bit23 into the MSB (bit31), then srawi by
             * 31 replicates that MSB across the whole word, producing a
             * real allOnes/allZeros mask directly (not just a 0/1 bit,
             * which emit_branch_blend()/emit_branch_blend_likely() both
             * require) - this exact "slwi 8" shape (rlwinm sh=8,mb=0,
             * me=23) was verified against real devkitPPC powerpc-eabi-
             * as/objdump output ("slwi 4,3,8" -> "rlwinm r4,r3,8,0,23")
             * before use here. */
            if (rt == 0x00 || rt == 0x01 || rt == 0x02 || rt == 0x03) {
                emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, FCR31_OFFSET));
                emit(ctx, enc_rlwinm(SCRATCH_B, SCRATCH_A, 8, 0, 23)); /* bit23 -> bit31 (MSB) */
                emit(ctx, enc_srawi(SCRATCH_E, SCRATCH_B, 31));        /* allOnes iff flag SET (BC1T cond) */
                if (rt == 0x00 || rt == 0x02) /* BC1F / BC1FL want flag CLEAR */
                    emit(ctx, enc_nor(SCRATCH_E, SCRATCH_E, SCRATCH_E));
                if (rt == 0x00 || rt == 0x01)
                    emit_branch_blend(ctx, 4 + (imm * 4));
                else
                    emit_branch_blend_likely(ctx, 4 + (imm * 4));
                return 0;
            }
            return -1; /* no other COP1 BC sub-opcode is real - matches
                        * ee_core.c's own halt() default case exactly. */
        }
        return -1; /* everything else under COP1 (op==0x11): not yet
                     * JIT-compiled, fall back to the interpreter. */
    }

    if (op == 0x12) {
        /* Round 907 (task #892): COP2 (VU0 macro mode) - this dynarec's
         * first VU0 opcodes. Re-verified against ee_core.c's real
         * `case 0x12:` body (~lines 8194-8307): rs<0x10 selects the
         * scalar transfer family (MFC2/QMFC2/CFC2/MTC2/QMTC2/CTC2, not
         * yet JIT'd - falls through below); rs>=0x10 is the CO-format
         * vector family, where rs = 0x10 | destmask (destmask bit3=X,
         * bit2=Y, bit1=Z, bit0=W, matching vu0_vf_write_lane()'s lane
         * index 0=X..3=W) and FT/FS/FD sit at bits 20-16/15-11/10-6
         * respectively - the exact same field layout the interpreter's
         * own case body comment documents (confirmed against real
         * vsub.xyzw/viswr encodings there). VADD(funct=0x28)/
         * VMUL(funct=0x2A)/VSUB(funct=0x2C): FD[lane] = FS[lane] OP
         * FT[lane] for every lane destmask selects - real float
         * arithmetic on the reinterpreted bit patterns, NO clamping
         * anywhere (confirmed absent from the real case body, unlike
         * COP1's fpu_clamp32) - so this is simpler than any COP1.S
         * opcode: just lfs both operands directly from vu0_vf (no
         * clamp-then-spill-to-stack step), one real fadds/fsubs/fmuls,
         * one stfs back to vu0_vf. destmask is a compile-time-constant
         * field of THIS instruction's own encoding (not a runtime
         * value), so the active lanes are simply unrolled here at
         * JIT-compile time - no runtime branching/masking needed at
         * all, unlike BC1's runtime fcr31 condition (Round 906b). The
         * broadcast row (funct 0x00-0x1F), VMADD/VMSUB's ACC operand
         * (funct 0x29/0x2D), VMAX/VMINI (funct 0x2B/0x2F), and the
         * scalar MFC2-family transfers are deliberately left to later
         * rounds (#893 onward) - not yet exercised by the traced boot
         * path, same "real, tested, roadmap-directed" scoping this
         * whole JIT arc has followed since Round 887. */
        uint32_t rs = (mips_instr >> 21) & 0x1Fu;
        if (rs >= 0x10u) {
            uint32_t destmask = rs & 0xFu;
            uint32_t ft = (mips_instr >> 16) & 0x1Fu;
            uint32_t fs = (mips_instr >> 11) & 0x1Fu;
            uint32_t fd = (mips_instr >> 6) & 0x1Fu;
            uint32_t funct = mips_instr & 0x3Fu;
            if (funct <= 0x1Fu) {
                /* Round 913b (task #898 continuation): the full
                 * broadcast row (funct 0x00-0x1F). Re-verified against
                 * ee_core.c's real case body (lines 8308-8375, its own
                 * "else if (funct <= 0x1F)" branch), which itself
                 * confirms this against PCSX2's R5900OpcodeTables.cpp
                 * SPECIAL1 table's first 4 rows (8 columns x 4 rows):
                 * 0x00-0x03 VADDx/y/z/w, 0x04-0x07 VSUBx/y/z/w,
                 * 0x08-0x0B VMADDx/y/z/w, 0x0C-0x0F VMSUBx/y/z/w,
                 * 0x10-0x13 VMAXx/y/z/w, 0x14-0x17 VMINIx/y/z/w,
                 * 0x18-0x1B VMULx/y/z/w, 0x1C VMULq/0x1D VMAXi/0x1E
                 * VMULi/0x1F VMINIi. bc_lane=funct&3 selects which FT
                 * lane is broadcast (or, for the Q/I row, which
                 * control register); base_op=(funct>>2)&7 selects the
                 * arithmetic op (0=ADD,1=SUB,2=MADD,3=MSUB,4=MAX,
                 * 5=MINI,6=MUL,7=Q/I-row-with-op-selected-by-bc_lane-
                 * instead). Every one of these fields is a function of
                 * `funct` alone - a compile-time constant field of
                 * this instruction's own encoding - so op_kind and
                 * the broadcast source (VF[ft][bc_lane] vs a control
                 * register) are BOTH resolved entirely at JIT-compile
                 * time here: unlike the interpreter's runtime
                 * op_kind switch, this codegen emits only the exact
                 * instruction sequence the resolved op_kind needs, no
                 * runtime branching at all - the same "compile-time-
                 * constant field, no runtime branch" treatment every
                 * other CO-format op in this file already uses for
                 * destmask/reg==0. The broadcast scalar is loaded ONCE
                 * into f1 before the lane loop (it's lane-invariant),
                 * matching the interpreter's own single `ub`/`b`
                 * computation outside its loop. VMADDx/y/z/w and
                 * VMSUBx/y/z/w read the same fixed VU0_ACC_OFF
                 * accumulator VOPMSUB/VMADD/VMSUB (Rounds 908/913)
                 * already established (no reg==0 concept for ACC).
                 * VMAX/VMINI (both the x/y/z/w and Q/I-row VMAXi/
                 * VMINIi forms) reuse the exact fsubs+fsel idiom
                 * Round 908's non-broadcast VMAX/VMINI already
                 * established (real hardware ternary, not the sign-
                 * magnitude bit trick COP1.S's MAX.S/MIN.S uses). The
                 * Q/I control registers (cop2_ctrl[22]/cop2_ctrl[21])
                 * already store raw float bit patterns (established
                 * by VDIV/VRSQRT, Round 909), so reading one via a
                 * direct `lfs` from COP2_CTRL_OFF needs no int->float
                 * conversion - it's a plain bit reinterpretation, same
                 * as every other Q/I read in this file. */
                uint32_t bc_lane = funct & 0x3u;
                uint32_t base_op = (funct >> 2) & 0x7u;
                uint32_t op_kind;
                int use_vi_broadcast = 0;
                uint32_t vi_reg = 0;
                if (base_op == 7u) {
                    use_vi_broadcast = 1;
                    if (bc_lane == 0u)      { op_kind = 6u; vi_reg = 22u; } /* VMULq: Q */
                    else if (bc_lane == 1u) { op_kind = 4u; vi_reg = 21u; } /* VMAXi: I */
                    else if (bc_lane == 2u) { op_kind = 6u; vi_reg = 21u; } /* VMULi: I */
                    else                    { op_kind = 5u; vi_reg = 21u; } /* VMINIi: I */
                } else {
                    op_kind = base_op;
                }
                if (use_vi_broadcast)
                    emit(ctx, enc_lfs(1, CTX_REG, COP2_CTRL_OFF(vi_reg))); /* f1 = broadcast scalar (Q or I) */
                else
                    emit(ctx, enc_lfs(1, CTX_REG, VU0_VF_OFF(ft, bc_lane))); /* f1 = FT[bc_lane] */
                for (int lane = 0; lane < 4; lane++) {
                    if (!(destmask & (0x8u >> lane))) continue;
                    emit(ctx, enc_lfs(0, CTX_REG, VU0_VF_OFF(fs, (uint32_t)lane))); /* f0 = FS[lane] */
                    if (op_kind == 0u) {
                        emit(ctx, enc_fadds(2, 0, 1));                     /* VADDx/y/z/w: f2 = f0 + f1 */
                    } else if (op_kind == 1u) {
                        emit(ctx, enc_fsubs(2, 0, 1));                     /* VSUBx/y/z/w: f2 = f0 - f1 */
                    } else if (op_kind == 2u) {
                        emit(ctx, enc_lfs(3, CTX_REG, VU0_ACC_OFF((uint32_t)lane))); /* f3 = ACC[lane] */
                        emit(ctx, enc_fmuls(4, 0, 1));                     /* f4 = FS*bc */
                        emit(ctx, enc_fadds(2, 3, 4));                     /* VMADDx/y/z/w: f2 = ACC + FS*bc */
                    } else if (op_kind == 3u) {
                        emit(ctx, enc_lfs(3, CTX_REG, VU0_ACC_OFF((uint32_t)lane))); /* f3 = ACC[lane] */
                        emit(ctx, enc_fmuls(4, 0, 1));                     /* f4 = FS*bc */
                        emit(ctx, enc_fsubs(2, 3, 4));                     /* VMSUBx/y/z/w: f2 = ACC - FS*bc */
                    } else if (op_kind == 4u) {
                        emit(ctx, enc_fsubs(3, 0, 1));                     /* f3 = FS - bc (sign drives fsel) */
                        emit(ctx, enc_fsel(2, 3, 0, 1));                   /* VMAXx/y/z/w or VMAXi: f2 = (FS>=bc) ? FS : bc */
                    } else if (op_kind == 5u) {
                        emit(ctx, enc_fsubs(3, 0, 1));                     /* f3 = FS - bc */
                        emit(ctx, enc_fsel(2, 3, 1, 0));                   /* VMINIx/y/z/w or VMINIi: f2 = (FS>=bc) ? bc : FS */
                    } else {
                        emit(ctx, enc_fmuls(2, 0, 1));                     /* VMULx/y/z/w or VMULq/VMULi: f2 = FS*bc */
                    }
                    if (fd != 0) /* writes to VF00 are discarded on real hardware */
                        emit(ctx, enc_stfs(2, CTX_REG, VU0_VF_OFF(fd, (uint32_t)lane)));
                }
                return 0;
            }
            if (funct == 0x28u || funct == 0x2Au || funct == 0x2Cu ||
                funct == 0x2Bu || funct == 0x2Fu) {
                /* Round 908 (task #893) BUGFIX + extension: this loop
                 * previously (Round 907, commit f796680) stored to
                 * VU0_VF_OFF(fd,lane) unconditionally, unlike every
                 * other real VF write in this file - vu0_vf_write_lane()
                 * in ee_core.c silently discards writes to VF00 ("if
                 * (reg == 0) return;", since VF00 is hardwired to
                 * (0,0,0,1.0f) and any write would corrupt reads of it
                 * elsewhere). fd is a compile-time-constant field of
                 * THIS instruction's own encoding, so the guard costs
                 * nothing at runtime - either the whole per-lane store
                 * is emitted or it isn't. Caught during Round 908
                 * research (re-deriving VABS/VOPMSUB's own reg==0
                 * conventions surfaced the gap), fixed here before it
                 * could compound into more opcodes copying the same
                 * bug. VMAX(0x2B)/VMINI(0x2F) joined this same combined
                 * per-lane loop this round too - confirmed against
                 * ee_core.c's real case body (lines 8273-8307) that all
                 * five of VADD/VMUL/VMAX/VSUB/VMINI share ONE combined
                 * interpreter case, `FD[lane] = FS[lane] OP FT[lane]`,
                 * and that VMAX/VMINI use a PLAIN ternary comparison
                 * (`(a>b)?a:b`), NOT the sign-magnitude bit trick
                 * COP1.S's MAX.S/MIN.S (Round 905) uses - this project's
                 * own source comment there confirms that's deliberate.
                 * Implemented via real PowerPC `fsel`: diff=fsubs(a,b),
                 * then fsel picks a or b by diff's sign - functionally
                 * identical to the ternary except at exact a==b, where
                 * both branches already agree. */
                for (int lane = 0; lane < 4; lane++) {
                    if (!(destmask & (0x8u >> lane)))
                        continue;
                    emit(ctx, enc_lfs(0, CTX_REG, VU0_VF_OFF(fs, (uint32_t)lane))); /* f0 = FS[lane] */
                    emit(ctx, enc_lfs(1, CTX_REG, VU0_VF_OFF(ft, (uint32_t)lane))); /* f1 = FT[lane] */
                    if (funct == 0x28u)
                        emit(ctx, enc_fadds(2, 0, 1));      /* VADD: f2 = f0 + f1 */
                    else if (funct == 0x2Cu)
                        emit(ctx, enc_fsubs(2, 0, 1));      /* VSUB: f2 = f0 - f1 */
                    else if (funct == 0x2Au)
                        emit(ctx, enc_fmuls(2, 0, 1));      /* VMUL: f2 = f0 * f1 */
                    else {
                        emit(ctx, enc_fsubs(3, 0, 1));      /* f3 = f0 - f1 (sign drives fsel) */
                        if (funct == 0x2Bu)
                            emit(ctx, enc_fsel(2, 3, 0, 1)); /* VMAX: f2 = (f0>=f1) ? f0 : f1 */
                        else
                            emit(ctx, enc_fsel(2, 3, 1, 0)); /* VMINI: f2 = (f0>=f1) ? f1 : f0 */
                    }
                    if (fd != 0) /* writes to VF00 are discarded on real hardware */
                        emit(ctx, enc_stfs(2, CTX_REG, VU0_VF_OFF(fd, (uint32_t)lane))); /* FD[lane] = f2 */
                }
                return 0;
            }
            if (funct == 0x2Eu) {
                /* Round 908 (task #893): VOPMSUB - outer-product
                 * multiply-subtract. Confirmed against ee_core.c's real
                 * case body (lines 8409-8431, itself matching PCSX2's
                 * VUops.cpp _vuOPMSUB) that this op ALWAYS writes
                 * exactly xyz - there is no destmask field for it at
                 * all on real hardware, unlike every other CO-format op
                 * this file handles - so destmask is deliberately never
                 * consulted here: FD.x=ACC.x-FS.y*FT.z,
                 * FD.y=ACC.y-FS.z*FT.x, FD.z=ACC.z-FS.x*FT.y (the
                 * cyclic y/z/x, z/x/y, x/y/z lane pairing is exactly
                 * ee_core.c's own, not a guess). */
                emit(ctx, enc_lfs(0, CTX_REG, VU0_VF_OFF(fs, 1))); /* f0 = FS.y */
                emit(ctx, enc_lfs(1, CTX_REG, VU0_VF_OFF(ft, 2))); /* f1 = FT.z */
                emit(ctx, enc_fmuls(2, 0, 1));                     /* f2 = FS.y * FT.z */
                emit(ctx, enc_lfs(3, CTX_REG, VU0_ACC_OFF(0)));    /* f3 = ACC.x */
                emit(ctx, enc_fsubs(4, 3, 2));                     /* f4 = ACC.x - FS.y*FT.z */
                if (fd != 0)
                    emit(ctx, enc_stfs(4, CTX_REG, VU0_VF_OFF(fd, 0))); /* FD.x */

                emit(ctx, enc_lfs(0, CTX_REG, VU0_VF_OFF(fs, 2))); /* f0 = FS.z */
                emit(ctx, enc_lfs(1, CTX_REG, VU0_VF_OFF(ft, 0))); /* f1 = FT.x */
                emit(ctx, enc_fmuls(2, 0, 1));                     /* f2 = FS.z * FT.x */
                emit(ctx, enc_lfs(3, CTX_REG, VU0_ACC_OFF(1)));    /* f3 = ACC.y */
                emit(ctx, enc_fsubs(4, 3, 2));                     /* f4 = ACC.y - FS.z*FT.x */
                if (fd != 0)
                    emit(ctx, enc_stfs(4, CTX_REG, VU0_VF_OFF(fd, 1))); /* FD.y */

                emit(ctx, enc_lfs(0, CTX_REG, VU0_VF_OFF(fs, 0))); /* f0 = FS.x */
                emit(ctx, enc_lfs(1, CTX_REG, VU0_VF_OFF(ft, 1))); /* f1 = FT.y */
                emit(ctx, enc_fmuls(2, 0, 1));                     /* f2 = FS.x * FT.y */
                emit(ctx, enc_lfs(3, CTX_REG, VU0_ACC_OFF(2)));    /* f3 = ACC.z */
                emit(ctx, enc_fsubs(4, 3, 2));                     /* f4 = ACC.z - FS.x*FT.y */
                if (fd != 0)
                    emit(ctx, enc_stfs(4, CTX_REG, VU0_VF_OFF(fd, 2))); /* FD.z */
                return 0;
            }
            if ((funct & 0x3Cu) == 0x3Cu) {
                /* Round 908 (task #893): SPECIAL2 sub-dispatch - real
                 * sub-opcode index formula confirmed against ee_core.c's
                 * own comment (matching PCSX2's R5900OpcodeTables.cpp):
                 * idx = (instr&0x3) | ((instr>>4)&0x7C). Only VABS(29)
                 * and VCLIP(31) are JIT'd this round - every other
                 * SPECIAL2 sub-opcode (VITOF/VFTOI/VMOVE/VMR32/VLQI/
                 * VSQI/VLQD/VSQD/VISWR/VRINIT/VRXOR/etc) falls back to
                 * the interpreter below. */
                uint32_t idx = (mips_instr & 0x3u) | ((mips_instr >> 4) & 0x7Cu);
                if (idx == 29) {
                    /* VABS: FT[lane] = FS[lane] & 0x7FFFFFFF (bit-level
                     * abs, no float op at all) per destmask lane.
                     * Confirmed against ee_core.c lines 8547-8556 that
                     * the DESTINATION is the FT field and the SOURCE is
                     * FS - the opposite of every arithmetic op above
                     * (FD/FS/FT) - matching real disassembly convention
                     * "vabs.xyzw FT, FS"; fd (bits 6-10) is unused here,
                     * per real hardware. Guarded by ft==0 (writes to
                     * VF00 discarded), same rule as every other VF
                     * write. Plain integer load/mask/store (lwz/rlwinm/
                     * stw, not lfs/fabs/stfs) since this is a raw
                     * bitwise operation on the pattern, not IEEE
                     * arithmetic - matching vu0_vf_read_lane/write_lane's
                     * own uint32_t-typed interface exactly. */
                    if (ft != 0) {
                        for (int lane = 0; lane < 4; lane++) {
                            if (!(destmask & (0x8u >> lane)))
                                continue;
                            emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, VU0_VF_OFF(fs, (uint32_t)lane)));
                            emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, 0, 1, 31)); /* clear bit0 (sign) */
                            emit(ctx, enc_stw(SCRATCH_A, CTX_REG, VU0_VF_OFF(ft, (uint32_t)lane)));
                        }
                    }
                    return 0;
                }
                if (idx == 31) {
                    /* VCLIP: judges |FS.x|,|FS.y|,|FS.z| against |FT.w|
                     * via SIGNED-INTEGER comparisons (not float, per
                     * ee_core.c lines 8911-8952 / PCSX2's real _vuCLIP),
                     * no Fsf/Ftf lane selector - xyz vs w is hardwired.
                     * `value` is derived from FT.w with a denormal-
                     * substitution rule, then 6 signed compares against
                     * `value` shift 6 new judgment bits into the CLIP
                     * flag register (this project's cop2_ctrl[18],
                     * masked to its low 24 bits after each call - 4
                     * calls' worth of judgment history, matching real
                     * hardware). CLIP's index (18) is a fixed part of
                     * this opcode's own real semantics, never a field
                     * of the instruction encoding, so it needs no
                     * reg==0 guard (18 is a compile-time constant,
                     * never 0).
                     *
                     * Each `(int32_t)(fsc ^ mask) > value` signed
                     * compare is turned into an UNSIGNED compare via
                     * the standard sign-bit-flip trick (XOR bit31 on
                     * both sides maps signed ordering onto unsigned
                     * ordering) - and because mask is only ever 0 or
                     * 0x80000000 here, XORing that trick's own
                     * 0x80000000 into `mask` collapses algebraically:
                     * the mask==0 compare needs (fsc^0x80000000) vs
                     * value', and the mask==0x80000000 compare needs
                     * fsc (UNCHANGED) vs value' - so only the raw lane
                     * value and its sign-flipped twin are ever needed,
                     * both against one precomputed value'=value^
                     * 0x80000000. Each unsigned (a>b)?1:0 uses the same
                     * subfc/subfe carry-to-mask idiom already
                     * established for SLT/SLTU (Round 886) and COP1's
                     * branch-condition masks, just on a single 32-bit
                     * word (no hi-word propagation needed - these
                     * aren't 64-bit R5900 GPRs). SCRATCH_A-H are all
                     * pure scratch within this one opcode's codegen. */
                    emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, VU0_VF_OFF(ft, 3))); /* SCRATCH_A = raw FT.w */
                    /* value = (ftw & 0x7f800000) ? (ftw & 0x7fffffff) : 0x007fffff -
                     * blended branchlessly via mask-and-OR (no real PPC
                     * control flow), matching e.g. the DIV.S divzero_mask
                     * idiom above (~line 2504) this file already
                     * established: materialize a real zero into a
                     * scratch register first via "li" (addi rD,0,imm -
                     * PPC special-cases rA==0 in ADDI/ADDIS to mean a
                     * literal 0, NOT a read of real register r0, which
                     * has no such guarantee outside that one encoding),
                     * then use THAT register as subfc/subfe's operand -
                     * passing a bare immediate 0 into subfc/subfe
                     * directly would silently read real r0's live
                     * content instead, since subf-family instructions
                     * don't get ADDI's rA==0 special case. */
                    emit(ctx, enc_rlwinm(SCRATCH_C, SCRATCH_A, 0, 1, 31)); /* SCRATCH_C = ftw & 0x7fffffff (candidate value if exponent nonzero) */
                    emit(ctx, enc_rlwinm(SCRATCH_B, SCRATCH_A, 0, 1, 8));  /* SCRATCH_B = ftw & 0x7f800000 (exponent field) */
                    emit_load_const32(ctx, SCRATCH_D, 0x007FFFFFu);        /* SCRATCH_D = 0x007FFFFF (denormal-substitution constant) */
                    emit(ctx, enc_addi(SCRATCH_F, 0, 0));                  /* SCRATCH_F = 0 (materialized zero, real "li") */
                    emit(ctx, enc_subfc(SCRATCH_E, SCRATCH_B, SCRATCH_F)); /* SCRATCH_E = 0 - SCRATCH_B; CA=1 iff SCRATCH_B==0 */
                    emit(ctx, enc_subfe(SCRATCH_E, SCRATCH_E, SCRATCH_E)); /* SCRATCH_E = CA-1: B==0 -> 0, B!=0 -> allOnes */
                    emit(ctx, enc_and(SCRATCH_C, SCRATCH_C, SCRATCH_E));   /* SCRATCH_C = (B!=0) ? (ftw&0x7fffffff) : 0 */
                    emit(ctx, enc_nor(SCRATCH_G, SCRATCH_E, SCRATCH_E));   /* SCRATCH_G = ~SCRATCH_E */
                    emit(ctx, enc_and(SCRATCH_D, SCRATCH_D, SCRATCH_G));   /* SCRATCH_D = (B==0) ? 0x7FFFFF : 0 */
                    emit(ctx, enc_or(SCRATCH_C, SCRATCH_C, SCRATCH_D));    /* SCRATCH_C = value (final) */
                    emit(ctx, enc_xoris(SCRATCH_C, SCRATCH_C, 0x8000));    /* SCRATCH_C = value' = value ^ 0x80000000 */

                    /* Fold 6 signed compares into 6 unsigned compares
                     * against value' (SCRATCH_C), building `clip` in
                     * SCRATCH_H, then shift the whole VI[18] history
                     * left by 6 and OR the new bits in, masked to 24
                     * bits, exactly matching ee_core.c's own
                     * clip=(clip<<6|bits)&0xFFFFFF. */
                    emit(ctx, enc_lwz(SCRATCH_H, CTX_REG, COP2_CTRL_OFF(18))); /* SCRATCH_H = old clip (VI[18]) */
                    /* (old_clip << 6) & 0xFFFFFF: rotate left 6, then mask
                     * to PPC bits 8-25 (normal bits 6-23) - this excludes
                     * BOTH the bottom 6 bits (where the rotate would have
                     * wrapped clip's own top 6 bits back in, which a true
                     * shift-left must zero-fill instead) AND the top 8
                     * bits (bits 24-31, cleared by the real &0xFFFFFF),
                     * leaving exactly clip's original bits 0-17 re-
                     * positioned at bits 6-23 - bit-for-bit what
                     * ee_core.c's "(clip<<6)&0xFFFFFF" computes, verified
                     * by hand against the same rlwinm-as-slwi calibration
                     * this file's own BC1 code (SH=8,MB=0,ME=23 == slwi
                     * by 8) already established: extending that pattern,
                     * a plain slwi-by-6 alone would be MB=0,ME=25; the
                     * extra &0xFFFFFF narrows MB from 0 to 8. */
                    emit(ctx, enc_rlwinm(SCRATCH_H, SCRATCH_H, 6, 8, 25));

                    for (int comp = 0; comp < 3; comp++) {
                        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, VU0_VF_OFF(fs, (uint32_t)comp))); /* SCRATCH_A = raw fsc (comp: 0=x,1=y,2=z) */
                        emit(ctx, enc_xoris(SCRATCH_B, SCRATCH_A, 0x8000)); /* SCRATCH_B = fsc ^ 0x80000000 */
                        /* bit_lo (mask==0 test, i.e. fsc>value): unsigned compare (SCRATCH_B >u SCRATCH_C) */
                        emit(ctx, enc_subfc(SCRATCH_D, SCRATCH_B, SCRATCH_C));
                        emit(ctx, enc_subfe(SCRATCH_D, SCRATCH_D, SCRATCH_D));
                        emit(ctx, enc_andi_dot(SCRATCH_D, SCRATCH_D, 1)); /* SCRATCH_D = (fsc>value)?1:0 */
                        /* bit_hi (mask==0x80000000 test, i.e. (fsc^0x80000000)>value): unsigned compare (SCRATCH_A >u SCRATCH_C) */
                        emit(ctx, enc_subfc(SCRATCH_E, SCRATCH_A, SCRATCH_C));
                        emit(ctx, enc_subfe(SCRATCH_E, SCRATCH_E, SCRATCH_E));
                        emit(ctx, enc_andi_dot(SCRATCH_E, SCRATCH_E, 1)); /* SCRATCH_E = ((fsc^0x80000000)>value)?1:0 */
                        /* Place this component's two result bits at their
                         * real clip-register slot: x->bits0-1, y->bits2-3,
                         * z->bits4-5 (matching ee_core.c's 0x01/0x02,
                         * 0x04/0x08, 0x10/0x20 literals exactly). Both D
                         * and E hold a clean 0 or 1 (single bit at normal
                         * position 0) going in, so rotating left by a
                         * small constant with an identity mask (MB=0,
                         * ME=31) just relocates that one bit - there is no
                         * other set bit to wrap into contamination. */
                        emit(ctx, enc_rlwinm(SCRATCH_D, SCRATCH_D, (uint32_t)(comp * 2), 0, 31));     /* bit_lo -> bit(2*comp) */
                        emit(ctx, enc_rlwinm(SCRATCH_E, SCRATCH_E, (uint32_t)(comp * 2 + 1), 0, 31)); /* bit_hi -> bit(2*comp+1) */
                        emit(ctx, enc_or(SCRATCH_H, SCRATCH_H, SCRATCH_D));
                        emit(ctx, enc_or(SCRATCH_H, SCRATCH_H, SCRATCH_E));
                    }
                    emit(ctx, enc_stw(SCRATCH_H, CTX_REG, COP2_CTRL_OFF(18))); /* VI[18] = new clip */
                    return 0;
                }
                if (idx == 56 || idx == 58) {
                    /* Round 909 (task #894): VDIV(56)/VRSQRT(58) - the
                     * division/reciprocal-sqrt family that writes the Q
                     * register (cop2_ctrl[22], already this project's
                     * single source of truth for Q since the VMULq/
                     * VADDq broadcast-row opcodes). Re-verified against
                     * ee_core.c's real case body (~lines 8813-8851):
                     * Fsf/Ftf are compile-time-constant 2-bit lane
                     * selectors baked into destmask (destmask&3=Fsf,
                     * (destmask>>2)&3=Ftf - the same split VCLIP's own
                     * comment above documents), so which VF lane feeds
                     * FS/FT is fixed at JIT-translate time, not a
                     * runtime choice - unlike the float VALUES read from
                     * those lanes, which are of course runtime data.
                     * VU0 floats are never run through COP1's
                     * fpu_double()/fpu_clamp32 machinery anywhere in
                     * ee_core.c's real VU0 case bodies (confirmed absent
                     * again here, matching every other VU0 arithmetic
                     * opcode this file has JIT'd since Round 907) - so
                     * this is simpler than COP1's DIV.S/RSQRT.S in that
                     * regard, but the divide-by-zero test itself is
                     * DIFFERENT from COP1's: `ftv == 0.0f` is a genuine
                     * IEEE float equality check here (true only for the
                     * exact 0x00000000/0x80000000 bit patterns), not
                     * COP1's exponent-field/denormal-counts-as-zero
                     * test - implemented as a sign-stripped magnitude
                     * compare on the raw bits (rlwinm mb=1,me=31 then
                     * the same subfc/subfe/nor allOnes-iff-zero idiom
                     * this file already uses throughout). On a zero
                     * divisor, VDIV always produces a signed FLT_MAX
                     * whose sign is the XOR of both raw operands' sign
                     * bits (0/0 and x/0 share this one formula - the
                     * real distinction only affects an unmodeled status
                     * flag), same "xor sign bits, OR with 0x7F7FFFFF"
                     * blend DIV.S's own divide-by-zero path established
                     * (~line 2512). VRSQRT's zero-divisor case branches
                     * ONE level further on fsv (confirmed against lines
                     * 8834-8841): fsv!=0 clamps to that identical
                     * signed-FLT_MAX result, but fsv==0 (a genuine
                     * 0/sqrt(0)) clamps to signed zero instead - and
                     * since sign_diff is already exactly 0 or
                     * 0x80000000 (nothing else), "signed zero with that
                     * sign" is just sign_diff itself, needing no extra
                     * OR. VRSQRT's normal path calls this project's real
                     * sqrtf() trampoline (same ADDR_EE_SQRTF convention
                     * Round 905's SQRT.S/RSQRT.S established, since real
                     * PPC750/Gekko can't safely run fsqrts) BEFORE the
                     * final blend, so the zero_mask/special_result must
                     * be spilled to the stack across that call (r3-r12,
                     * i.e. every SCRATCH_A-H register, are EABI volatile
                     * and sqrtf() is free to clobber any of them - the
                     * same reason RSQRT.S's own codegen above reloads
                     * K1M1/K2M1 after its call). */
                    uint32_t fsf_lane = destmask & 0x3u;
                    uint32_t ftf_lane = (destmask >> 2) & 0x3u;
                    if (idx == 56) {
                        emit(ctx, enc_addi(1, 1, -16)); /* push 16-byte scratch frame */

                        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, VU0_VF_OFF(ft, ftf_lane))); /* raw FT[ftf_lane] (divisor) */
                        emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, VU0_VF_OFF(fs, fsf_lane))); /* raw FS[fsf_lane] (dividend) */

                        /* zero_mask = allOnes iff (divisor & 0x7FFFFFFF) == 0 */
                        emit(ctx, enc_rlwinm(SCRATCH_E, SCRATCH_C, 0, 1, 31));
                        emit(ctx, enc_addi(SCRATCH_F, 0, 0)); /* real zero, "li" */
                        emit(ctx, enc_subfc(SCRATCH_G, SCRATCH_E, SCRATCH_F));
                        emit(ctx, enc_subfe(SCRATCH_G, SCRATCH_G, SCRATCH_G));
                        emit(ctx, enc_nor(SCRATCH_G, SCRATCH_G, SCRATCH_G)); /* SCRATCH_G = zero_mask */

                        /* special_result = ((divisor ^ dividend) & 0x80000000) | 0x7F7FFFFF */
                        emit(ctx, enc_xor(SCRATCH_H, SCRATCH_C, SCRATCH_D));
                        emit(ctx, enc_rlwinm(SCRATCH_H, SCRATCH_H, 0, 0, 0)); /* sign bit only */
                        emit_load_const32(ctx, SCRATCH_A, 0x7F7FFFFFu);
                        emit(ctx, enc_or(SCRATCH_H, SCRATCH_H, SCRATCH_A)); /* special_result */

                        /* normal_result = fsv / ftv - raw fdivs, no clamp. */
                        emit(ctx, enc_stw(SCRATCH_D, 1, 0));
                        emit(ctx, enc_lfs(0, 1, 0)); /* f0 = fsv */
                        emit(ctx, enc_stw(SCRATCH_C, 1, 0));
                        emit(ctx, enc_lfs(1, 1, 0)); /* f1 = ftv */
                        emit(ctx, enc_fdivs(2, 0, 1));
                        emit(ctx, enc_stfs(2, 1, 0));
                        emit(ctx, enc_lwz(SCRATCH_B, 1, 0)); /* normal_result bits */

                        /* Blend: final = zero_mask ? special_result : normal_result */
                        emit(ctx, enc_nor(SCRATCH_F, SCRATCH_G, SCRATCH_G)); /* notmask */
                        emit(ctx, enc_and(SCRATCH_H, SCRATCH_H, SCRATCH_G));
                        emit(ctx, enc_and(SCRATCH_B, SCRATCH_B, SCRATCH_F));
                        emit(ctx, enc_or(SCRATCH_B, SCRATCH_B, SCRATCH_H));
                        emit(ctx, enc_stw(SCRATCH_B, CTX_REG, COP2_CTRL_OFF(22))); /* Q = result */

                        emit(ctx, enc_addi(1, 1, 16)); /* pop scratch frame */
                    } else {
                        emit(ctx, enc_addi(1, 1, -32)); /* push 32-byte scratch frame */

                        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, VU0_VF_OFF(ft, ftf_lane))); /* raw uft */
                        emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, VU0_VF_OFF(fs, fsf_lane))); /* raw ufs */
                        emit(ctx, enc_stw(SCRATCH_D, 1, 24)); /* stash ufs across the sqrtf call */

                        /* zero_mask = allOnes iff (uft & 0x7FFFFFFF) == 0 */
                        emit(ctx, enc_rlwinm(SCRATCH_E, SCRATCH_C, 0, 1, 31));
                        emit(ctx, enc_addi(SCRATCH_F, 0, 0));
                        emit(ctx, enc_subfc(SCRATCH_G, SCRATCH_E, SCRATCH_F));
                        emit(ctx, enc_subfe(SCRATCH_G, SCRATCH_G, SCRATCH_G));
                        emit(ctx, enc_nor(SCRATCH_G, SCRATCH_G, SCRATCH_G)); /* zero_mask */
                        emit(ctx, enc_stw(SCRATCH_G, 1, 4));

                        /* sign_diff = (uft ^ ufs) & 0x80000000 */
                        emit(ctx, enc_xor(SCRATCH_H, SCRATCH_C, SCRATCH_D));
                        emit(ctx, enc_rlwinm(SCRATCH_H, SCRATCH_H, 0, 0, 0)); /* sign_diff */

                        /* fsv_zero_mask = allOnes iff (ufs & 0x7FFFFFFF) == 0 */
                        emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_D, 0, 1, 31));
                        emit(ctx, enc_addi(SCRATCH_B, 0, 0));
                        emit(ctx, enc_subfc(SCRATCH_E, SCRATCH_A, SCRATCH_B));
                        emit(ctx, enc_subfe(SCRATCH_E, SCRATCH_E, SCRATCH_E));
                        emit(ctx, enc_nor(SCRATCH_E, SCRATCH_E, SCRATCH_E)); /* fsv_zero_mask */

                        /* special_within_zero = fsv_zero_mask ? sign_diff
                         *                        : (sign_diff | 0x7F7FFFFF) */
                        emit_load_const32(ctx, SCRATCH_A, 0x7F7FFFFFu);
                        emit(ctx, enc_or(SCRATCH_A, SCRATCH_H, SCRATCH_A)); /* special_a (fsv!=0) */
                        emit(ctx, enc_nor(SCRATCH_B, SCRATCH_E, SCRATCH_E)); /* notmask of fsv_zero_mask */
                        emit(ctx, enc_and(SCRATCH_F, SCRATCH_H, SCRATCH_E)); /* special_b(=sign_diff) & fsv_zero_mask */
                        emit(ctx, enc_and(SCRATCH_A, SCRATCH_A, SCRATCH_B)); /* special_a & notmask */
                        emit(ctx, enc_or(SCRATCH_A, SCRATCH_A, SCRATCH_F));  /* special_within_zero */
                        emit(ctx, enc_stw(SCRATCH_A, 1, 8));

                        /* temp = sqrtf(fabsf(ftv)) via trampoline */
                        emit(ctx, enc_rlwinm(SCRATCH_C, SCRATCH_C, 0, 1, 31)); /* fabsf(uft) */
                        emit(ctx, enc_stw(SCRATCH_C, 1, 12));
                        emit(ctx, enc_lfs(1, 1, 12)); /* f1 = |ftv| - EABI float arg reg */

                        emit(ctx, enc_stw(14, 1, 16)); /* save caller's r14 */
                        emit(ctx, enc_stw(15, 1, 20)); /* save caller's r15 */
                        emit(ctx, enc_or(15, CTX_REG, CTX_REG)); /* r15 = ctx */
                        emit(ctx, enc_mflr(14));
                        emit_load_const32(ctx, 12, ADDR_EE_SQRTF);
                        emit(ctx, enc_mtctr(12));
                        emit(ctx, enc_bctrl());                  /* f1 = sqrtf(f1) = temp */
                        emit(ctx, enc_mtlr(14));
                        emit(ctx, enc_or(CTX_REG, 15, 15));      /* restore ctx into r3 */
                        emit(ctx, enc_lwz(14, 1, 16));
                        emit(ctx, enc_lwz(15, 1, 20));

                        /* normal_result = fsv / temp (f1 still holds sqrtf's result) */
                        emit(ctx, enc_lwz(SCRATCH_C, 1, 24)); /* reload raw ufs */
                        emit(ctx, enc_stw(SCRATCH_C, 1, 12));
                        emit(ctx, enc_lfs(0, 1, 12)); /* f0 = fsv */
                        emit(ctx, enc_fdivs(2, 0, 1));
                        emit(ctx, enc_stfs(2, 1, 12));
                        emit(ctx, enc_lwz(SCRATCH_B, 1, 12)); /* normal_result bits */

                        /* Blend: final = zero_mask ? special_within_zero : normal_result */
                        emit(ctx, enc_lwz(SCRATCH_D, 1, 4));  /* zero_mask */
                        emit(ctx, enc_lwz(SCRATCH_E, 1, 8));  /* special_within_zero */
                        emit(ctx, enc_nor(SCRATCH_F, SCRATCH_D, SCRATCH_D));
                        emit(ctx, enc_and(SCRATCH_E, SCRATCH_E, SCRATCH_D));
                        emit(ctx, enc_and(SCRATCH_B, SCRATCH_B, SCRATCH_F));
                        emit(ctx, enc_or(SCRATCH_B, SCRATCH_B, SCRATCH_E));
                        emit(ctx, enc_stw(SCRATCH_B, CTX_REG, COP2_CTRL_OFF(22))); /* Q = result */

                        emit(ctx, enc_addi(1, 1, 32)); /* pop scratch frame */
                    }
                    return 0;
                }
                if (idx == 57) {
                    /* Round 909 (task #894): VSQRT - Q = sqrtf(|FT[ftf_
                     * lane]|), no FS operand and no destmask (matches
                     * VABS's own real "no full destmask consulted"
                     * convention, though for a different reason here:
                     * this project's own comment above already noted
                     * that in the disassembler, VSQRT prints only FT -
                     * confirmed directly in ee_core.c's real case body,
                     * ~lines 8818-8821). Unlike VDIV/VRSQRT above, VSQRT
                     * has NO divide-by-zero-style special case at all -
                     * sqrtf(|0|)=0 is already exactly the right answer,
                     * nothing to substitute a fake infinity for - so
                     * this is just an unconditional sqrtf() trampoline
                     * call, the simplest of the three Round 909
                     * opcodes. Same real sqrtf() trampoline convention
                     * as VDIV/VRSQRT above and Round 905's SQRT.S/
                     * RSQRT.S (real PPC750/Gekko can't safely run
                     * fsqrts - see ADDR_EE_SQRTF's own comment). */
                    uint32_t ftf_lane = (destmask >> 2) & 0x3u;
                    emit(ctx, enc_addi(1, 1, -16)); /* push 16-byte scratch frame */

                    emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, VU0_VF_OFF(ft, ftf_lane))); /* raw FT[ftf_lane] */
                    emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, 0, 1, 31)); /* fabsf: clear sign bit */
                    emit(ctx, enc_stw(SCRATCH_A, 1, 0));
                    emit(ctx, enc_lfs(1, 1, 0)); /* f1 = |FT[ftf_lane]| - EABI float arg reg */

                    emit(ctx, enc_stw(14, 1, 8));  /* save caller's r14 */
                    emit(ctx, enc_stw(15, 1, 12)); /* save caller's r15 */
                    emit(ctx, enc_or(15, CTX_REG, CTX_REG)); /* r15 = ctx */
                    emit(ctx, enc_mflr(14));
                    emit_load_const32(ctx, 12, ADDR_EE_SQRTF);
                    emit(ctx, enc_mtctr(12));
                    emit(ctx, enc_bctrl());                  /* f1 = sqrtf(f1) */
                    emit(ctx, enc_mtlr(14));
                    emit(ctx, enc_or(CTX_REG, 15, 15));      /* restore ctx into r3 */
                    emit(ctx, enc_lwz(14, 1, 8));
                    emit(ctx, enc_lwz(15, 1, 12));

                    emit(ctx, enc_stfs(1, 1, 0));
                    emit(ctx, enc_lwz(SCRATCH_A, 1, 0));
                    emit(ctx, enc_stw(SCRATCH_A, CTX_REG, COP2_CTRL_OFF(22))); /* Q = result */

                    emit(ctx, enc_addi(1, 1, 16)); /* pop scratch frame */
                    return 0;
                }
                if (idx == 48) {
                    /* Round 910 (task #895): VMOVE - FT[lane] = FS[lane],
                     * plain per-lane copy, confirmed against ee_core.c
                     * lines ~8557-8563. Same "dest=FT, src=FS, fd field
                     * unused" convention as VABS/VCLIP (idx 29/31,
                     * Round 908) and the whole idx=16-23/48/49 unary/
                     * data-movement cluster this project's own comment
                     * documents. Guarded by ft==0 (writes to VF00
                     * discarded), matching that shared cluster guard
                     * exactly. Plain integer copy (lwz/stw, not lfs/
                     * stfs) since this is a raw bit-pattern move, not
                     * IEEE arithmetic - no computation needed at all. */
                    if (ft != 0) {
                        for (int lane = 0; lane < 4; lane++) {
                            if (!(destmask & (0x8u >> lane)))
                                continue;
                            emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, VU0_VF_OFF(fs, (uint32_t)lane)));
                            emit(ctx, enc_stw(SCRATCH_A, CTX_REG, VU0_VF_OFF(ft, (uint32_t)lane)));
                        }
                    }
                    return 0;
                }
                if (idx == 49) {
                    /* Round 910 (task #895): VMR32 - 32-bit lane rotate:
                     * FT.x=FS.y, FT.y=FS.z, FT.z=FS.w, FT.w=FS.x.
                     * Confirmed against ee_core.c lines ~8564-8579
                     * (itself ported from PCSX2's _vuMR32). All four
                     * source lanes are read into scratch registers FIRST
                     * before any destination write, exactly matching
                     * ee_core.c's own temp-variable ordering - this is
                     * NOT a style choice, it's required correctness: a
                     * VMR32-to-self (ft==fs) must still rotate correctly,
                     * which an in-place lane-by-lane read/write would
                     * corrupt (e.g. writing FT.x=FS.y before FS.y itself
                     * has been read for the FT.y=FS.z step, if ft==fs).
                     * ft==0 guard shared with VMOVE above. */
                    if (ft != 0) {
                        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, VU0_VF_OFF(fs, 0))); /* tx = FS.x */
                        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, VU0_VF_OFF(fs, 1))); /* ty = FS.y */
                        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, VU0_VF_OFF(fs, 2))); /* tz = FS.z */
                        emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, VU0_VF_OFF(fs, 3))); /* tw = FS.w */
                        if (destmask & 0x8u) emit(ctx, enc_stw(SCRATCH_B, CTX_REG, VU0_VF_OFF(ft, 0))); /* FT.x = ty */
                        if (destmask & 0x4u) emit(ctx, enc_stw(SCRATCH_C, CTX_REG, VU0_VF_OFF(ft, 1))); /* FT.y = tz */
                        if (destmask & 0x2u) emit(ctx, enc_stw(SCRATCH_D, CTX_REG, VU0_VF_OFF(ft, 2))); /* FT.z = tw */
                        if (destmask & 0x1u) emit(ctx, enc_stw(SCRATCH_A, CTX_REG, VU0_VF_OFF(ft, 3))); /* FT.w = tx */
                    }
                    return 0;
                }
                if (idx >= 16 && idx <= 23) {
                    /* Round 911 (task #896): VITOF0/4/12/15(idx16-19)/
                     * VFTOI0/4/12/15(idx20-23) - fixed-point<->float
                     * conversion, the last members of the unary/data-
                     * movement cluster (idx 16-23/29/48/49) this file's
                     * own comment documents; VABS(29)/VMOVE(48)/VMR32(49)
                     * already JIT'd above (Rounds 908/910). Re-verified
                     * against ee_core.c's real case body (~lines 8580-
                     * 8628, itself ported bit-exact from PCSX2's
                     * VUops.cpp intToFloat<Offset>/floatToInt<Offset>
                     * templates): offset_n selects a power-of-two scale
                     * factor baked directly into a float's raw exponent
                     * bits (0x3F800000 -/+ (offset_n<<23) for VITOF/
                     * VFTOI respectively), applied AFTER the int->float
                     * conversion for VITOF but BEFORE the float->int
                     * conversion for VFTOI. dest=FT, src=FS, fd unused,
                     * guarded by ft==0 - same convention as the rest of
                     * this cluster. destmask still selects which of the
                     * 4 lanes participate (confirmed against ee_core.c's
                     * own per-lane destmask loop at line 8596-8597,
                     * unlike VABS/VCLIP's few special cases above).
                     *
                     * VITOF's core int->float step has no real PPC750/
                     * Gekko FPU instruction at all (see ADDR_EE_CVT_S_W's
                     * own comment, ~line 740) - reuses that EXACT SAME
                     * real ee_jit_cvt_s_w_helper() trampoline CVT.S.W
                     * (Round 906b) established, since it's a trivial
                     * `(float)(int32_t)x` cast identical to what VITOF's
                     * un-scaled core conversion needs; the offset scale
                     * (if any) is then applied afterward with a plain
                     * fmuls on the trampoline's f1 result - no value
                     * needs to survive the call except what's already
                     * naturally in f1 when it returns, so (unlike
                     * VRSQRT's trampoline above) nothing needs to be
                     * spilled to the stack across this particular call.
                     *
                     * VFTOI's core float->int step reuses fctiwz (PPC
                     * Book I base ISA, present on Gekko - see CVT.W.S's
                     * own comment, ~line 3031) plus an exponent-threshold
                     * saturation blend in the SAME shape CVT.W.S (idx==24
                     * funct dispatch, ~line 3022) already established
                     * (same subfc/subfe borrow-to-mask idiom turning a
                     * signed comparison into an unsigned one, same
                     * clamp_val=0x7fffffff^signmask derivation) - but
                     * NOT byte-for-byte identical, since VFTOI's real
                     * threshold test is `>=0x4F000000` (ee_core.c line
                     * 8619), a DIFFERENT constant AND a DIFFERENT
                     * comparison operator than CVT.W.S's `>0x4E800000`
                     * (confirmed by direct re-read of both real case
                     * bodies - not assumed identical just because both
                     * are "a float->int saturation gap"). To get a
                     * `>=` test out of the same subfc/subfe idiom,
                     * subfc(D,threshold,mag_exp) computes mag_exp-
                     * threshold with CA=1 iff mag_exp>=threshold (the
                     * exact polarity wanted), then subfe(E,D,D) turns
                     * that CA into 0(CA=1)/allOnes(CA=0) - the OPPOSITE
                     * of what's wanted (out-of-range should map to
                     * allOnes) - so an explicit nor() flips it, unlike
                     * CVT.W.S's subfc(D,mag_exp,threshold) ordering
                     * which already comes out with the right polarity
                     * for its own `>` test without an extra flip. */
                    if (ft != 0) {
                        int is_ftoi = (idx >= 20);
                        uint32_t offset_n = (idx & 0x3u) == 0u ? 0u : (idx & 0x3u) == 1u ? 4u : (idx & 0x3u) == 2u ? 12u : 15u;
                        for (int lane = 0; lane < 4; lane++) {
                            if (!(destmask & (0x8u >> lane)))
                                continue;
                            emit(ctx, enc_addi(1, 1, -16)); /* push 16-byte scratch frame */
                            if (!is_ftoi) {
                                /* VITOF: int->float via trampoline, then optional scale */
                                emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, VU0_VF_OFF(fs, (uint32_t)lane))); /* raw int32 */
                                emit(ctx, enc_stw(SCRATCH_A, 1, 8));

                                emit(ctx, enc_stw(14, 1, 0)); /* save caller's r14 */
                                emit(ctx, enc_stw(15, 1, 4)); /* save caller's r15 */
                                emit(ctx, enc_or(15, CTX_REG, CTX_REG)); /* r15 = ctx */
                                emit(ctx, enc_mflr(14));
                                emit(ctx, enc_lwz(3, 1, 8)); /* r3 = int32 argument */
                                emit_load_const32(ctx, 12, ADDR_EE_CVT_S_W);
                                emit(ctx, enc_mtctr(12));
                                emit(ctx, enc_bctrl());                  /* f1 = (float)ival */
                                emit(ctx, enc_mtlr(14));
                                emit(ctx, enc_or(CTX_REG, 15, 15));      /* restore ctx into r3 */
                                emit(ctx, enc_lwz(14, 1, 0));
                                emit(ctx, enc_lwz(15, 1, 4));

                                if (offset_n) {
                                    uint32_t scale_bits = 0x3F800000u - (offset_n << 23);
                                    emit_load_const32(ctx, SCRATCH_A, scale_bits);
                                    emit(ctx, enc_stw(SCRATCH_A, 1, 8));
                                    emit(ctx, enc_lfs(2, 1, 8));   /* f2 = scale */
                                    emit(ctx, enc_fmuls(1, 1, 2)); /* f1 *= scale */
                                }

                                emit(ctx, enc_stfs(1, 1, 8));
                                emit(ctx, enc_lwz(SCRATCH_A, 1, 8));
                                emit(ctx, enc_stw(SCRATCH_A, CTX_REG, VU0_VF_OFF(ft, (uint32_t)lane)));
                            } else {
                                /* VFTOI: optional scale, then float->int via fctiwz + exponent-threshold saturation blend */
                                emit(ctx, enc_lfs(0, CTX_REG, VU0_VF_OFF(fs, (uint32_t)lane))); /* f0 = fval */
                                if (offset_n) {
                                    uint32_t scale_bits = 0x3F800000u + (offset_n << 23);
                                    emit_load_const32(ctx, SCRATCH_A, scale_bits);
                                    emit(ctx, enc_stw(SCRATCH_A, 1, 8));
                                    emit(ctx, enc_lfs(2, 1, 8));
                                    emit(ctx, enc_fmuls(0, 0, 2)); /* f0 *= scale */
                                }
                                emit(ctx, enc_stfs(0, 1, 8));
                                emit(ctx, enc_lwz(SCRATCH_A, 1, 8)); /* fbits (post-scale) */

                                /* gt_mask(SCRATCH_E) = allOnes iff mag_exp >= 0x4F000000 (out of range) */
                                emit(ctx, enc_rlwinm(SCRATCH_B, SCRATCH_A, 0, 1, 8)); /* mag_exp */
                                emit_load_const32(ctx, SCRATCH_C, 0x4F000000u);
                                emit(ctx, enc_subfc(SCRATCH_D, SCRATCH_C, SCRATCH_B)); /* D = mag_exp - threshold; CA=1 iff mag_exp>=threshold */
                                emit(ctx, enc_subfe(SCRATCH_E, SCRATCH_D, SCRATCH_D)); /* CA=1->E=0; CA=0->E=allOnes */
                                emit(ctx, enc_nor(SCRATCH_E, SCRATCH_E, SCRATCH_E));   /* flip: E = gt_mask */

                                /* clamp_val = (fbits<0) ? 0x80000000 : 0x7fffffff */
                                emit(ctx, enc_srawi(SCRATCH_F, SCRATCH_A, 31));
                                emit_load_const32(ctx, SCRATCH_G, 0x7fffffffu);
                                emit(ctx, enc_xor(SCRATCH_G, SCRATCH_G, SCRATCH_F));

                                /* normal path: fctiwz on the (already scaled) value */
                                emit(ctx, enc_lfs(0, 1, 8));         /* reload scaled/unscaled float */
                                emit(ctx, enc_fctiwz(1, 0));
                                emit(ctx, enc_stfd(1, 1, 0));
                                emit(ctx, enc_lwz(SCRATCH_H, 1, 4));  /* normal_val (low word) */

                                /* blend: final = gt_mask ? clamp_val : normal_val */
                                emit(ctx, enc_nor(SCRATCH_D, SCRATCH_E, SCRATCH_E)); /* notmask (in-range) */
                                emit(ctx, enc_and(SCRATCH_H, SCRATCH_H, SCRATCH_D));
                                emit(ctx, enc_and(SCRATCH_G, SCRATCH_G, SCRATCH_E));
                                emit(ctx, enc_or(SCRATCH_H, SCRATCH_H, SCRATCH_G));
                                emit(ctx, enc_stw(SCRATCH_H, CTX_REG, VU0_VF_OFF(ft, (uint32_t)lane)));
                            }
                            emit(ctx, enc_addi(1, 1, 16)); /* pop scratch frame */
                        }
                    }
                    return 0;
                }
                return -1; /* every other SPECIAL2 sub-opcode: not yet
                             * JIT-compiled, fall back to the interpreter. */
            }
            if (funct == 0x30u || funct == 0x31u || funct == 0x34u || funct == 0x35u) {
                /* Round 910 (task #895): VIADD/VISUB/VIAND/VIOR - plain
                 * integer ALU on VI registers, VI[fd] = VI[fs] op
                 * VI[ft]. Confirmed against ee_core.c lines ~8982-9009
                 * (its own comment there notes these were found in a
                 * real BIOS "clear every VU0 register" init routine).
                 * Unlike every other CO-format op this file JITs,
                 * destmask/lane looping is irrelevant here - this is a
                 * plain SCALAR VI-register op, no VF/lane involvement at
                 * all. VI registers live in the same cop2_ctrl array
                 * VDIV/VCLIP's Q/CLIP already use (COP2_CTRL_OFF), and
                 * VI0 is hardwired to 0 exactly like VF00 - real
                 * vu0_vi_write() silently discards writes to reg 0
                 * ("if (reg==0) return"), so this dynarec's direct store
                 * needs the same `if (fd != 0)` guard every VF-writing
                 * op here already carries (the Round 908 bugfix's own
                 * lesson, applied correctly from the start this round).
                 * Real VI registers are 16-bit, so VIADD/VISUB results
                 * are masked to 16 bits (rlwinm mb=16,me=31, the
                 * standard "clear top 16 bits" idiom - SH=0 so it's a
                 * pure AND, no rotation); VIAND/VIOR need no extra mask
                 * since AND/OR of two already-16-bit-clean values stays
                 * 16-bit-clean, matching ee_core.c's own comment on this
                 * exact point. */
                emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, COP2_CTRL_OFF(fs))); /* a = VI[fs] */
                emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, COP2_CTRL_OFF(ft))); /* b = VI[ft] */
                if (funct == 0x30u) {
                    emit(ctx, enc_add(SCRATCH_C, SCRATCH_A, SCRATCH_B));    /* VIADD: a + b */
                    emit(ctx, enc_rlwinm(SCRATCH_C, SCRATCH_C, 0, 16, 31)); /* & 0xFFFF */
                } else if (funct == 0x31u) {
                    emit(ctx, enc_subfc(SCRATCH_C, SCRATCH_B, SCRATCH_A));  /* VISUB: a - b (subfc computes rB-rA) */
                    emit(ctx, enc_rlwinm(SCRATCH_C, SCRATCH_C, 0, 16, 31)); /* & 0xFFFF */
                } else if (funct == 0x34u) {
                    emit(ctx, enc_and(SCRATCH_C, SCRATCH_A, SCRATCH_B));   /* VIAND: a & b, already 16-bit clean */
                } else {
                    emit(ctx, enc_or(SCRATCH_C, SCRATCH_A, SCRATCH_B));    /* VIOR: a | b, already 16-bit clean */
                }
                if (fd != 0) /* writes to VI0 are discarded on real hardware */
                    emit(ctx, enc_stw(SCRATCH_C, CTX_REG, COP2_CTRL_OFF(fd)));
                return 0;
            }
            if (funct == 0x29u || funct == 0x2Du) {
                /* Round 913 (task #898): VMADD(0x29)/VMSUB(0x2D) -
                 * FD[lane] = ACC[lane] +- FS[lane]*FT[lane], per
                 * destmask lane. Re-verified against ee_core.c's real
                 * case body (lines 8376-8408): same SPECIAL1 row as
                 * VADD/VMUL/VMAX/VSUB/VMINI (Round 907/908) and
                 * VOPMSUB (Round 908), but reads a third operand from
                 * the fixed VU0 macro-mode accumulator (st->vu0_acc[4],
                 * lane order x=0/y=1/z=2/w=3 - VOPMSUB's own
                 * VU0_ACC_OFF already established this exact offset
                 * macro, no register-index concept applies to ACC so
                 * no reg==0 guard is needed on the read side, unlike
                 * FS/FT). Writes FD only - does NOT write back into
                 * ACC (that's the separate VMADDA/VMSUBA accumulator-
                 * dest family, a distinct scoped-out gap per
                 * ee_core.c's own comment, not implemented in the
                 * interpreter either). Computed as two separate float
                 * ops (fmuls then fadds/fsubs), matching the plain C
                 * `acc + a*b` / `acc - a*b` expression shape the
                 * interpreter itself evaluates (no fused multiply-add
                 * contraction assumed, same as VOPMSUB's own two-step
                 * pattern). */
                for (int lane = 0; lane < 4; lane++) {
                    if (!(destmask & (0x8u >> lane))) continue;
                    emit(ctx, enc_lfs(0, CTX_REG, VU0_ACC_OFF((uint32_t)lane))); /* f0 = ACC[lane] */
                    emit(ctx, enc_lfs(1, CTX_REG, VU0_VF_OFF(fs, (uint32_t)lane))); /* f1 = FS[lane] */
                    emit(ctx, enc_lfs(2, CTX_REG, VU0_VF_OFF(ft, (uint32_t)lane))); /* f2 = FT[lane] */
                    emit(ctx, enc_fmuls(3, 1, 2));                    /* f3 = FS[lane] * FT[lane] */
                    if (funct == 0x29u)
                        emit(ctx, enc_fadds(4, 0, 3));                /* VMADD: f4 = ACC + FS*FT */
                    else
                        emit(ctx, enc_fsubs(4, 0, 3));                /* VMSUB: f4 = ACC - FS*FT */
                    if (fd != 0) /* writes to VF00 are discarded on real hardware */
                        emit(ctx, enc_stfs(4, CTX_REG, VU0_VF_OFF(fd, (uint32_t)lane)));
                }
                return 0;
            }
            if (funct == 0x32u) {
                /* Round 913 (task #898): VIADDI - VI[ft] = VI[fs] +
                 * sign_extend(imm), where imm is the raw 5-bit field
                 * at the SAME bit position (6-10) as FD in every other
                 * CO-format arithmetic op this file handles, reused
                 * here as an immediate rather than a register index.
                 * Re-verified against ee_core.c's real case body
                 * (lines 8957-8981): unlike VIADD/VISUB/VIAND/VIOR
                 * (dest=FD), VIADDI's real operand order is dest=FT,
                 * src=FS, imm=SA (confirmed there against PCSX2's own
                 * DisR5900asm.cpp P_VIADDI disassembly format). The
                 * sign-extension is a real-hardware quirk ported
                 * verbatim from PCSX2's VUops.cpp _vuIADDI: imm5=fd,
                 * imm = (imm5&0x10 ? 0xFFF0 : 0) | (imm5&0xF) -
                 * effectively a signed 4-bit magnitude with a separate
                 * sign bit, not a plain 5-bit two's-complement
                 * sign-extend. imm5/fd is a compile-time-constant
                 * field of this instruction's own encoding, so imm is
                 * fully resolved at JIT-compile time; its two possible
                 * 32-bit value ranges (0x0-0xF or 0xFFF0-0xFFFF) are
                 * exactly valid int16_t bit patterns, so a plain
                 * `addi` with imm cast to int16_t reproduces the same
                 * 32-bit sum the interpreter's unsigned `a + imm`
                 * computes, with no separate load-immediate step
                 * needed. Result masked to 16 bits (real VI registers
                 * are 16-bit, same convention VIADD/VISUB already
                 * use), guarded by the usual compile-time
                 * `if (ft != 0)` (vu0_vi_write's own discard-on-VI0). */
                uint32_t imm5 = fd;
                uint32_t imm = ((imm5 & 0x10u) ? 0xFFF0u : 0u) | (imm5 & 0xFu);
                if (ft != 0) {
                    emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, COP2_CTRL_OFF(fs))); /* a = VI[fs] */
                    emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, (int16_t)imm));   /* a + sign_extend(imm) */
                    emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, 0, 16, 31));    /* & 0xFFFF */
                    emit(ctx, enc_stw(SCRATCH_A, CTX_REG, COP2_CTRL_OFF(ft)));
                }
                return 0;
            }
            return -1; /* broadcast row (funct 0x00-0x1F): not yet
                         * JIT-compiled, fall back to the interpreter. */
        }
        /* Round 912 (task #897): the rs<0x10 scalar transfer family -
         * MFC2(0x00)/QMFC2(0x01)/CFC2(0x02)/MTC2(0x04)/QMTC2(0x05)/
         * CTC2(0x06) - re-verified against ee_core.c's real case body
         * (lines 8221-8258). rt sits at bits 20-16, rd at bits 15-11 -
         * the same field positions the CO-format's ft/fs already use,
         * just under different names since this is the scalar-transfer
         * form, not the vector-arithmetic form. Two exact interpreter-
         * body duplications drive this codegen's own structure:
         * MFC2(0x00) and CFC2(0x02) are BYTE-FOR-BYTE identical
         * (`if (rt) GPR(rt) = sext32(vu0_vi_read(st, rd));`), and so are
         * MTC2(0x04) and CTC2(0x06) (`vu0_vi_write(st, rd, rt32);`) -
         * CTC2's real FBRST/control-register semantics (VU0/VU1 force-
         * break and reset bits) are commented in ee_core.c but NOT
         * modeled beyond plain storage there, so the JIT correctly
         * mirrors that same "plain storage, nothing more" behavior
         * rather than inventing FBRST side effects the interpreter
         * itself doesn't have.
         *
         * MFC2/CFC2: vu0_vi_read(rd) is `(rd==0) ? 0 : cop2_ctrl[rd]`
         * (ee_core.c line 3034-3037) - rd is a compile-time-constant
         * field of this instruction's own encoding, so the rd==0 case
         * is resolved at JIT-compile time (li 0) rather than costing a
         * runtime branch; sext32() then fills the full 64-bit GPR via
         * the same srawi-by-31 fill-word idiom LW/ADDIU/ADDU/SLL all
         * already use elsewhere in this file (REG_HI=sign word,
         * REG_LO=value word).
         *
         * MTC2/CTC2: vu0_vi_write(rd, rt32) discards writes to VI0
         * (ee_core.c line 3039-3043, `if (reg==0) return;`) - same
         * "compile-time-constant guard, no runtime branch" treatment
         * every other VI/VF write in this file already uses (the Round
         * 908 bugfix's own lesson). rt32 is read from REG_LO(rt)
         * un-guarded - SW's own comment (this file, op==0x2B) already
         * established that reading GPR0 as a value-to-store needs no
         * guard, since the GPR0 slot is always kept zero.
         *
         * QMFC2/QMTC2: 128-bit raw bit copy between GPR(rt) and VF[rd],
         * NO float conversion (ee_core.c lines 8224-8245, the comment there
         * is explicit about this). GPR(rt).ud0 = VF.x | (VF.y<<32),
         * GPR(rt).ud1 = VF.z | (VF.w<<32) - so VF.x/VF.z are the LOW
         * 32-bit halves and VF.y/VF.w are the HIGH 32-bit halves of
         * ud0/ud1 respectively, which maps directly onto this file's
         * existing REG_LO/REG_HI (ud0's low/high halves) and
         * REG_LO1/REG_HI1 (ud1's low/high halves, established by
         * Round 900's LQ/SQ). QMFC2 reads VU0_VF_OFF(rd,lane) directly
         * with NO rd==0 special-casing needed - unlike every VF WRITE
         * in this file, VF00's array slot itself is kept correctly
         * hardwired ((0,0,0,1.0), see ee_core.c line 3802's reset-time
         * `st->vu0_vf[0][3] = 0x3F800000u`) precisely because writes to
         * it are always discarded, so a direct read is always safe -
         * the same assumption every VADD/VSUB/VMUL/etc read in this
         * file already relies on. QMTC2 writes VU0_VF_OFF(rd,lane) and
         * DOES need the compile-time rd==0 guard (vu0_vf_write_lane's
         * own discard-on-reg-0), applied here exactly like every prior
         * VF-writing round since Round 908. */
        {
            uint32_t rt = (mips_instr >> 16) & 0x1Fu;
            uint32_t rd = (mips_instr >> 11) & 0x1Fu;
            if (rs == 0x00u || rs == 0x02u) { /* MFC2 / CFC2 */
                if (rt != 0) {
                    if (rd == 0)
                        emit_load_const32(ctx, SCRATCH_A, 0);
                    else
                        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, COP2_CTRL_OFF(rd)));
                    emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rt)));
                    emit(ctx, enc_srawi(SCRATCH_B, SCRATCH_A, 31)); /* sign fill */
                    emit(ctx, enc_stw(SCRATCH_B, CTX_REG, REG_HI(rt)));
                }
                return 0;
            }
            if (rs == 0x04u || rs == 0x06u) { /* MTC2 / CTC2 */
                if (rd != 0) { /* writes to VI0 are discarded on real hardware */
                    emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rt))); /* rt32 */
                    emit(ctx, enc_stw(SCRATCH_A, CTX_REG, COP2_CTRL_OFF(rd)));
                }
                return 0;
            }
            if (rs == 0x01u) { /* QMFC2 */
                if (rt != 0) {
                    emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, VU0_VF_OFF(rd, 0))); /* VF.x -> ud0 lo */
                    emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(rt)));
                    emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, VU0_VF_OFF(rd, 1))); /* VF.y -> ud0 hi */
                    emit(ctx, enc_stw(SCRATCH_B, CTX_REG, REG_HI(rt)));
                    emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, VU0_VF_OFF(rd, 2))); /* VF.z -> ud1 lo */
                    emit(ctx, enc_stw(SCRATCH_C, CTX_REG, REG_LO1(rt)));
                    emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, VU0_VF_OFF(rd, 3))); /* VF.w -> ud1 hi */
                    emit(ctx, enc_stw(SCRATCH_D, CTX_REG, REG_HI1(rt)));
                }
                return 0;
            }
            if (rs == 0x05u) { /* QMTC2 */
                if (rd != 0) { /* writes to VF00 are discarded on real hardware */
                    emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rt))); /* ud0 lo -> VF.x */
                    emit(ctx, enc_stw(SCRATCH_A, CTX_REG, VU0_VF_OFF(rd, 0)));
                    emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_HI(rt))); /* ud0 hi -> VF.y */
                    emit(ctx, enc_stw(SCRATCH_B, CTX_REG, VU0_VF_OFF(rd, 1)));
                    emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_LO1(rt))); /* ud1 lo -> VF.z */
                    emit(ctx, enc_stw(SCRATCH_C, CTX_REG, VU0_VF_OFF(rd, 2)));
                    emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, REG_HI1(rt))); /* ud1 hi -> VF.w */
                    emit(ctx, enc_stw(SCRATCH_D, CTX_REG, VU0_VF_OFF(rd, 3)));
                }
                return 0;
            }
            return -1; /* rs==0x03 / rs==0x07-0x0F: no real sub-opcode -
                         * matches ee_core.c's own halt() default case
                         * exactly. */
        }
    }

    if (op == 0x02) {
        /* MIPS: j target -> pc = (this_pc & 0xF0000000) |
         * ((instr & 0x03FFFFFF) << 2); always taken, delay slot always
         * executes, no link register write (that's JAL below). The
         * top-4-bits-of-this_pc part is read from CONTEXT at runtime
         * (see EXC_THIS_PC_OFFSET's comment for why that's required,
         * not just a style choice, under this dynarec's encoding-keyed
         * cache) while the low 26 bits of the target are a plain
         * compile-time constant baked in via emit_load_const32 - they
         * really are part of this instruction's own encoding, unlike
         * this_pc itself. */
        uint32_t target_low = (mips_instr & 0x03FFFFFFu) << 2;
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, EXC_THIS_PC_OFFSET));
        emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, 0, 0, 3)); /* &= 0xF0000000 */
        emit_load_const32(ctx, SCRATCH_B, target_low);
        emit(ctx, enc_or(SCRATCH_A, SCRATCH_A, SCRATCH_B));
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, NEXT_PC_OFFSET));
        emit(ctx, enc_addi(SCRATCH_B, 0, 1)); /* li SCRATCH_B, 1 */
        emit(ctx, enc_stb(SCRATCH_B, CTX_REG, BRANCH_PENDING_OFFSET));
        return 0;
    }

    if (op == 0x03) {
        /* MIPS: jal target -> gpr[31] = this_pc + 8; pc = (this_pc &
         * 0xF0000000) | ((instr & 0x03FFFFFF) << 2) - same target
         * computation as J above, plus the LINK(31) write. IMPORTANT:
         * ee_core.c's own LINK() macro is `GPR(reg) = this_pc + 8;`
         * where this_pc is a plain uint32_t and GPR(reg) is the 64-bit
         * `ud0` field - `this_pc + 8` is computed as a uint32_t (usual
         * arithmetic conversions: the int literal 8 converts to
         * unsigned), and assigning a uint32_t into a uint64_t lvalue is
         * a ZERO-extension in C, not a sign-extension. This is
         * DIFFERENT from every prior 32-bit-result opcode this dynarec
         * has handled (ADDIU/ADDU/LUI/etc.), which all sign-extend
         * their 32-bit result via srawi - LINK's result must instead
         * get a plain 0 high word, matching the interpreter's own
         * (zero-extending, not sign-extending) real behavior exactly.
         * The link write is emitted FIRST (its own independent read of
         * EXC_THIS_PC_OFFSET into SCRATCH_A, immediately consumed and
         * stored before SCRATCH_A gets reused) so it can't be disturbed
         * by whatever SCRATCH_A holds afterward for the target calc -
         * simpler than trying to preserve a value across the two
         * computations. */
        uint32_t target_low = (mips_instr & 0x03FFFFFFu) << 2;
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, EXC_THIS_PC_OFFSET));
        emit(ctx, enc_addi(SCRATCH_A, SCRATCH_A, 8));
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, REG_LO(31)));
        emit(ctx, enc_addi(SCRATCH_B, 0, 0)); /* li SCRATCH_B, 0 (zero-extend, NOT sign-extend - see comment above) */
        emit(ctx, enc_stw(SCRATCH_B, CTX_REG, REG_HI(31)));
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, EXC_THIS_PC_OFFSET));
        emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, 0, 0, 3)); /* &= 0xF0000000 */
        emit_load_const32(ctx, SCRATCH_B, target_low);
        emit(ctx, enc_or(SCRATCH_A, SCRATCH_A, SCRATCH_B));
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, NEXT_PC_OFFSET));
        emit(ctx, enc_addi(SCRATCH_B, 0, 1)); /* li SCRATCH_B, 1 */
        emit(ctx, enc_stb(SCRATCH_B, CTX_REG, BRANCH_PENDING_OFFSET));
        return 0;
    }

    if (op == 0x00 && funct == 0x08) {
        /* MIPS: jr rs -> pc = (uint32_t)gpr[rs] - truncated to the low
         * 32 bits, matching ee_core.c's own `(uint32_t)GPR(rs)` cast
         * (real EE code addresses are always 32-bit even though GPRs
         * are 64-bit). No link register write, always taken. Unlike
         * J/JAL, the target here is a plain register value (REG_LO(rs))
         * with no this_pc dependency at all - same shape as LW/SW's
         * base-register read, nothing new needed. */
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, NEXT_PC_OFFSET));
        emit(ctx, enc_addi(SCRATCH_B, 0, 1)); /* li SCRATCH_B, 1 */
        emit(ctx, enc_stb(SCRATCH_B, CTX_REG, BRANCH_PENDING_OFFSET));
        return 0;
    }

    if (op == 0x00 && funct == 0x09) {
        /* MIPS: jalr rd, rs -> { uint32_t tgt = (uint32_t)gpr[rs]; if
         * (rd) gpr[rd] = this_pc + 8; pc = tgt; } - ee_core.c's own
         * case body captures tgt into a local BEFORE conditionally
         * writing rd, specifically so "jalr rd, rs" with rd==rs still
         * uses the OLD rs value as the jump target (not the just-
         * written link address). This block preserves that ordering
         * the same way: REG_LO(rs) is read into SCRATCH_A first and
         * never touched again, so it's safe regardless of whether
         * rd==rs. Real hardware discards writes to $zero (rd==0), same
         * as every other dest-register op in this dynarec. Like JAL
         * above, the link write is ZERO-extended (plain 0 high word),
         * NOT sign-extended via srawi - see JAL's comment for why
         * ee_core.c's own LINK() macro (`GPR(reg) = this_pc + 8;`,
         * uint32_t assigned into a uint64_t) is a zero-extension, not
         * a sign-extension, unlike every other 32-bit-result opcode
         * this dynarec has handled so far. */
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs))); /* tgt, captured before any rd write */
        if (rd != 0) {
            emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, EXC_THIS_PC_OFFSET));
            emit(ctx, enc_addi(SCRATCH_B, SCRATCH_B, 8));
            emit(ctx, enc_stw(SCRATCH_B, CTX_REG, REG_LO(rd)));
            emit(ctx, enc_addi(SCRATCH_C, 0, 0)); /* li SCRATCH_C, 0 (zero-extend, NOT sign-extend) */
            emit(ctx, enc_stw(SCRATCH_C, CTX_REG, REG_HI(rd)));
        }
        emit(ctx, enc_stw(SCRATCH_A, CTX_REG, NEXT_PC_OFFSET));
        emit(ctx, enc_addi(SCRATCH_B, 0, 1)); /* li SCRATCH_B, 1 */
        emit(ctx, enc_stb(SCRATCH_B, CTX_REG, BRANCH_PENDING_OFFSET));
        return 0;
    }

    if (op == 0x04 || op == 0x05) {
        /* MIPS: beq/bne rs, rt, offset -> if (GPR(rs) ==/!= GPR(rt))
         * BRANCH_TO(this_pc + 4 + (sext16(offset) << 2)); delay slot
         * always executes regardless of whether the branch is taken
         * (ee_step()'s own dispatch handles that the same way it does
         * for J/JAL above - this dynarec only needs to get THIS
         * instruction's next_pc/branch_pending right).
         *
         * This is this dynarec's FIRST conditional opcode, and it's
         * done WITHOUT any real PPC branch instruction: compute an
         * all-0s/all-1s "taken" mask using the exact same technique
         * MOVZ/MOVN already use above for conditional register writes
         * (see that block's rtOr-is-zero check), then hand the mask to
         * emit_branch_blend() to conditionally overwrite next_pc/
         * branch_pending. Every generated block stays a single
         * straight-line run with no internal control flow, matching
         * this whole file's existing style. */
        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_HI(rs)));
        emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, REG_HI(rt)));
        emit(ctx, enc_xor(SCRATCH_C, SCRATCH_C, SCRATCH_D)); /* hi diff */
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(rt)));
        emit(ctx, enc_xor(SCRATCH_A, SCRATCH_A, SCRATCH_B)); /* lo diff */
        emit(ctx, enc_or(SCRATCH_A, SCRATCH_A, SCRATCH_C));  /* combined diff, 0 iff equal */
        emit(ctx, enc_addi(SCRATCH_B, 0, 1));                       /* li SCRATCH_B, 1 */
        emit(ctx, enc_subfc(SCRATCH_D, SCRATCH_B, SCRATCH_A));      /* D=throwaway, CA=(diff!=0) */
        emit(ctx, enc_subfe(SCRATCH_E, SCRATCH_B, SCRATCH_B));      /* SCRATCH_E = eq_mask: all-ones iff diff==0 */
        if (op == 0x05) /* BNE wants the opposite condition */
            emit(ctx, enc_nor(SCRATCH_E, SCRATCH_E, SCRATCH_E));
        /* imm is already a sign-extended int32_t; left-shifting a
         * possibly-negative signed value is undefined behavior in C
         * (caught by UBSan during this round's own verification run),
         * so the *4 is done via multiplication instead - well-defined
         * for any 16-bit-derived imm, no overflow risk. */
        emit_branch_blend(ctx, 4 + (imm * 4));
        return 0;
    }

    if (op == 0x06 || op == 0x07) {
        /* MIPS: blez/bgtz rs, offset -> if ((int64_t)GPR(rs) </>= 0)
         * BRANCH_TO(this_pc + 4 + (sext16(offset) << 2)). rt is a
         * reserved field (encoded as 0) and ignored here, matching real
         * hardware and this project's own interpreter.
         *
         * A 64-bit two's-complement value's sign is exactly its hi
         * word's own sign bit (bit63 of the value == bit31 of hi), so
         * "srawi rA, hi, 31" replicates that sign bit across all 32
         * bits in ONE instruction - a direct "value < 0" mask with no
         * subfc/subfe compare needed at all (unlike BEQ/BNE above).
         * <=0 additionally needs the exactly-zero case ORed in (whose
         * sign bit is 0, so it's not already covered by the sign-bit
         * mask) via the same OR-then-subfc/subfe "is-zero" idiom used
         * by BEQ/BNE and MOVZ/MOVN above. */
        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_HI(rs)));
        emit(ctx, enc_srawi(SCRATCH_E, SCRATCH_C, 31)); /* sign mask: all-ones iff rs<0 */
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_or(SCRATCH_A, SCRATCH_A, SCRATCH_C));   /* hi|lo, nonzero iff rs!=0 */
        emit(ctx, enc_addi(SCRATCH_B, 0, 1));                 /* li SCRATCH_B, 1 */
        emit(ctx, enc_subfc(SCRATCH_D, SCRATCH_B, SCRATCH_A));
        emit(ctx, enc_subfe(SCRATCH_D, SCRATCH_B, SCRATCH_B)); /* SCRATCH_D = is_zero_mask */
        emit(ctx, enc_or(SCRATCH_E, SCRATCH_E, SCRATCH_D));    /* SCRATCH_E = ble_mask (rs<=0) */
        if (op == 0x07) /* BGTZ wants the opposite condition (rs>0) */
            emit(ctx, enc_nor(SCRATCH_E, SCRATCH_E, SCRATCH_E));
        /* imm is already a sign-extended int32_t; left-shifting a
         * possibly-negative signed value is undefined behavior in C
         * (caught by UBSan during this round's own verification run),
         * so the *4 is done via multiplication instead - well-defined
         * for any 16-bit-derived imm, no overflow risk. */
        emit_branch_blend(ctx, 4 + (imm * 4));
        return 0;
    }

    if (op == 0x01) {
        /* MIPS REGIMM (op=0x01): the rt field selects the sub-opcode.
         * Only BLTZ(0x00)/BGEZ(0x01)/BLTZL(0x02)/BGEZL(0x03) are handled
         * this round; other REGIMM sub-opcodes (BLTZAL/BGEZAL/-ALL,
         * TGEI/TLTI/etc. traps) fall through to the Unsupported return
         * below untouched. */
        if (rt == 0x00 || rt == 0x01) {
            /* MIPS: bltz/bgez rs, offset -> if ((int64_t)GPR(rs) </>= 0)
             * BRANCH_TO(...). Trivial extension of BLEZ/BGTZ's srawi
             * trick above, but simpler: BLTZ tests ONLY the sign bit (no
             * exactly-zero special case needed, since 0 is not < 0). */
            emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_HI(rs)));
            emit(ctx, enc_srawi(SCRATCH_E, SCRATCH_C, 31)); /* <0 mask */
            if (rt == 0x01) /* BGEZ wants the opposite condition */
                emit(ctx, enc_nor(SCRATCH_E, SCRATCH_E, SCRATCH_E));
            emit_branch_blend(ctx, 4 + (imm * 4));
            return 0;
        }
        if (rt == 0x02 || rt == 0x03) {
            /* MIPS: bltzl/bgezl rs, offset - REGIMM's Likely pair. Same
             * sign-bit mask as BLTZ/BGEZ, but routed through
             * emit_branch_blend_likely() for delay-slot annulment. */
            emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_HI(rs)));
            emit(ctx, enc_srawi(SCRATCH_E, SCRATCH_C, 31));
            if (rt == 0x03) /* BGEZL wants the opposite condition */
                emit(ctx, enc_nor(SCRATCH_E, SCRATCH_E, SCRATCH_E));
            emit_branch_blend_likely(ctx, 4 + (imm * 4));
            return 0;
        }
        /* Other REGIMM sub-opcodes: fall through to Unsupported. */
    }

    if (op == 0x14 || op == 0x15) {
        /* MIPS: beql/bnel rs, rt, offset - the Likely pair of BEQ/BNE
         * above. Identical mask computation, routed through
         * emit_branch_blend_likely() instead of emit_branch_blend() for
         * delay-slot annulment on the not-taken path (see that helper's
         * own comment for the exact ee_core.c semantics it reproduces). */
        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_HI(rs)));
        emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, REG_HI(rt)));
        emit(ctx, enc_xor(SCRATCH_C, SCRATCH_C, SCRATCH_D));
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, REG_LO(rt)));
        emit(ctx, enc_xor(SCRATCH_A, SCRATCH_A, SCRATCH_B));
        emit(ctx, enc_or(SCRATCH_A, SCRATCH_A, SCRATCH_C));
        emit(ctx, enc_addi(SCRATCH_B, 0, 1));
        emit(ctx, enc_subfc(SCRATCH_D, SCRATCH_B, SCRATCH_A));
        emit(ctx, enc_subfe(SCRATCH_E, SCRATCH_B, SCRATCH_B));
        if (op == 0x15) /* BNEL wants the opposite condition */
            emit(ctx, enc_nor(SCRATCH_E, SCRATCH_E, SCRATCH_E));
        emit_branch_blend_likely(ctx, 4 + (imm * 4));
        return 0;
    }

    if (op == 0x16 || op == 0x17) {
        /* MIPS: blezl/bgtzl rs, offset - the Likely pair of BLEZ/BGTZ
         * above. Identical mask computation, routed through
         * emit_branch_blend_likely(). */
        emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, REG_HI(rs)));
        emit(ctx, enc_srawi(SCRATCH_E, SCRATCH_C, 31));
        emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, REG_LO(rs)));
        emit(ctx, enc_or(SCRATCH_A, SCRATCH_A, SCRATCH_C));
        emit(ctx, enc_addi(SCRATCH_B, 0, 1));
        emit(ctx, enc_subfc(SCRATCH_D, SCRATCH_B, SCRATCH_A));
        emit(ctx, enc_subfe(SCRATCH_D, SCRATCH_B, SCRATCH_B));
        emit(ctx, enc_or(SCRATCH_E, SCRATCH_E, SCRATCH_D));
        if (op == 0x17) /* BGTZL wants the opposite condition */
            emit(ctx, enc_nor(SCRATCH_E, SCRATCH_E, SCRATCH_E));
        emit_branch_blend_likely(ctx, 4 + (imm * 4));
        return 0;
    }

    if (op == 0x1Cu && funct == 0x08u) {
        /* Round 914 (task #899): MMI0 group (op=0x1C/MMI, funct=0x08 is
         * itself a meta-opcode whose OWN sub-dispatch key is the `sa`
         * field, bits 10-6 - the "shift amount" position in a normal
         * R-type MIPS instruction, repurposed here, matching ee_core.c's
         * own `case 0x08: switch (sa) { ... }` nesting at ~line 9276).
         * This round covers the 3 add/sub SIMD pairs: PADDW/PSUBW (4x
         * 32-bit lanes, sa=0x00/0x01), PADDH/PSUBH (8x 16-bit lanes,
         * sa=0x04/0x05), PADDB/PSUBB (16x 8-bit lanes, sa=0x08/0x09) -
         * verified against ee_core.c's real case bodies at lines
         * 9278-9283. Every real case body is byte-for-byte `set_lane_X(
         * &gpr[rd], n, lane_X(gpr[rs],n) +/- lane_X(gpr[rt],n))` for
         * n across the lane count, guarded by `if (rd)` - real hardware
         * discards writes to $zero, same compile-time-resolved guard
         * this whole file uses everywhere else. No saturation, no
         * overflow detection - plain wraparound add/sub, matching the
         * interpreter's plain C `+`/`-` on the unsigned lane types
         * exactly (uint32_t/uint16_t/uint8_t, so C's usual arithmetic
         * already wraps the same way real hardness does).
         *
         * Byte-lane addressing is the interesting part here (see
         * mmi_w_off/mmi_h_off/mmi_b_off's own comment for the full
         * derivation) - once those helpers give the right compile-time
         * offset per lane, the codegen itself is a plain load-add/sub-
         * store loop with no new arithmetic idioms. lhz/sth (opcodes
         * 40/44) are the only new PPC750 instruction forms this round
         * introduces - lwz/stw/lbz/stb/add/subf were all already
         * established. subf's `rT = rB - rA` calling convention (see
         * SUBU's own comment, Round 887) computes rs-rt via
         * enc_subf(SCRATCH_A, SCRATCH_B, SCRATCH_A): SCRATCH_A already
         * holds rs (rB, minuend) when this executes, SCRATCH_B holds rt
         * (rA, subtrahend). lhz/lbz zero-extend on load and sth/stb
         * truncate on store, so no separate masking instruction is
         * needed to reproduce set_lane_h/set_lane_b's own truncating
         * cast - the store width does that for free. */
        if (sa == 0x00u || sa == 0x01u) { /* PADDW / PSUBW: 4x 32-bit lanes */
            for (int lane = 0; lane < 4; lane++) {
                emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, mmi_w_off((int)rs, lane)));
                emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, mmi_w_off((int)rt, lane)));
                if (sa == 0x00u)
                    emit(ctx, enc_add(SCRATCH_A, SCRATCH_A, SCRATCH_B));
                else
                    emit(ctx, enc_subf(SCRATCH_A, SCRATCH_B, SCRATCH_A)); /* rs - rt */
                if (rd != 0)
                    emit(ctx, enc_stw(SCRATCH_A, CTX_REG, mmi_w_off((int)rd, lane)));
            }
            return 0;
        }
        if (sa == 0x04u || sa == 0x05u) { /* PADDH / PSUBH: 8x 16-bit lanes */
            for (int lane = 0; lane < 8; lane++) {
                emit(ctx, enc_lhz(SCRATCH_A, CTX_REG, mmi_h_off((int)rs, lane)));
                emit(ctx, enc_lhz(SCRATCH_B, CTX_REG, mmi_h_off((int)rt, lane)));
                if (sa == 0x04u)
                    emit(ctx, enc_add(SCRATCH_A, SCRATCH_A, SCRATCH_B));
                else
                    emit(ctx, enc_subf(SCRATCH_A, SCRATCH_B, SCRATCH_A));
                if (rd != 0)
                    emit(ctx, enc_sth(SCRATCH_A, CTX_REG, mmi_h_off((int)rd, lane)));
            }
            return 0;
        }
        if (sa == 0x08u || sa == 0x09u) { /* PADDB / PSUBB: 16x 8-bit lanes */
            for (int lane = 0; lane < 16; lane++) {
                emit(ctx, enc_lbz(SCRATCH_A, CTX_REG, mmi_b_off((int)rs, lane)));
                emit(ctx, enc_lbz(SCRATCH_B, CTX_REG, mmi_b_off((int)rt, lane)));
                if (sa == 0x08u)
                    emit(ctx, enc_add(SCRATCH_A, SCRATCH_A, SCRATCH_B));
                else
                    emit(ctx, enc_subf(SCRATCH_A, SCRATCH_B, SCRATCH_A));
                if (rd != 0)
                    emit(ctx, enc_stb(SCRATCH_A, CTX_REG, mmi_b_off((int)rd, lane)));
            }
            return 0;
        }
        /* Round 918 (task #903): MMI0's pack family - PPACW (sa=0x13,
         * 4x 32-bit lanes), PPACH (sa=0x17, 8x 16-bit lanes), PPACB
         * (sa=0x1B, 16x 8-bit lanes) - grep-confirmed at ee_core.c
         * lines 9402/9420/9440. Each takes alternating (even-indexed)
         * lanes from Rt into the low half of the result and from Rs
         * into the high half - a pure lane-reorder/subselect, no
         * arithmetic. mmi_w_off/mmi_h_off/mmi_b_off (established Round
         * 914) already give the correct compile-time byte offset for
         * ANY lane of ANY register, including the non-contiguous even-
         * lane subsets these ops read, so the codegen is plain lwz/
         * lhz/lbz + stw/sth/stb - no bit-packing or byte-deinterleave
         * needed, unlike an earlier draft of this round's PPACB
         * codegen assumed before re-reading mmi_h_off/mmi_b_off's own
         * definitions and realizing they already solve exactly this.
         *
         * The one real hazard: real hardware reads BOTH full source
         * registers before writing anything to the destination (see
         * ee_core.c's own `Rs = gpr[rs]; Rt = gpr[rt]; ... gpr[rd] =
         * out;` local-copy pattern) - so if rd aliases rs or rt, a
         * naive interleaved read/write could read an already-
         * overwritten lane. PPACW (4 lanes) and PPACH (8 lanes) each
         * fit entirely within this dynarec's 8 scratch GPRs, so they
         * simply read every needed source lane into scratch registers
         * FIRST, then write rd's lanes only after all reads are done -
         * always alias-safe, no special-casing needed. PPACB needs 16
         * independent source lanes, more than the 8 available scratch
         * registers, so it stages through a small stack scratch buffer
         * instead (push 16 bytes, read+stage all 16 source bytes, then
         * read the buffer back and write rd, pop the 16 bytes) - the
         * same "temporarily borrow stack space for a single compiled
         * block" pattern the C-function trampolines already use
         * (Round 891+), just holding data instead of a saved register. */
        if (sa == 0x13u) { /* PPACW */
            emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, mmi_w_off((int)rt, 0)));
            emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, mmi_w_off((int)rt, 2)));
            emit(ctx, enc_lwz(SCRATCH_C, CTX_REG, mmi_w_off((int)rs, 0)));
            emit(ctx, enc_lwz(SCRATCH_D, CTX_REG, mmi_w_off((int)rs, 2)));
            if (rd != 0) {
                emit(ctx, enc_stw(SCRATCH_A, CTX_REG, mmi_w_off((int)rd, 0)));
                emit(ctx, enc_stw(SCRATCH_B, CTX_REG, mmi_w_off((int)rd, 1)));
                emit(ctx, enc_stw(SCRATCH_C, CTX_REG, mmi_w_off((int)rd, 2)));
                emit(ctx, enc_stw(SCRATCH_D, CTX_REG, mmi_w_off((int)rd, 3)));
            }
            return 0;
        }
        if (sa == 0x17u) { /* PPACH */
            static const int src_lane4[4] = { 0, 2, 4, 6 };
            int scratch8[8] = { SCRATCH_A, SCRATCH_B, SCRATCH_C, SCRATCH_D,
                                 SCRATCH_E, SCRATCH_F, SCRATCH_G, SCRATCH_H };
            for (int k = 0; k < 4; k++)
                emit(ctx, enc_lhz(scratch8[k], CTX_REG, mmi_h_off((int)rt, src_lane4[k])));
            for (int k = 0; k < 4; k++)
                emit(ctx, enc_lhz(scratch8[4 + k], CTX_REG, mmi_h_off((int)rs, src_lane4[k])));
            if (rd != 0) {
                for (int k = 0; k < 8; k++)
                    emit(ctx, enc_sth(scratch8[k], CTX_REG, mmi_h_off((int)rd, k)));
            }
            return 0;
        }
        if (sa == 0x1Bu) { /* PPACB */
            static const int src_lane8[8] = { 0, 2, 4, 6, 8, 10, 12, 14 };
            emit(ctx, enc_addi(1, 1, -16)); /* 16-byte stack scratch buffer */
            for (int k = 0; k < 8; k++) {
                emit(ctx, enc_lbz(SCRATCH_A, CTX_REG, mmi_b_off((int)rt, src_lane8[k])));
                emit(ctx, enc_stb(SCRATCH_A, 1, (int16_t)k));
            }
            for (int k = 0; k < 8; k++) {
                emit(ctx, enc_lbz(SCRATCH_A, CTX_REG, mmi_b_off((int)rs, src_lane8[k])));
                emit(ctx, enc_stb(SCRATCH_A, 1, (int16_t)(8 + k)));
            }
            if (rd != 0) {
                for (int k = 0; k < 16; k++) {
                    emit(ctx, enc_lbz(SCRATCH_A, 1, (int16_t)k));
                    emit(ctx, enc_stb(SCRATCH_A, CTX_REG, mmi_b_off((int)rd, k)));
                }
            }
            emit(ctx, enc_addi(1, 1, 16));
            return 0;
        }
        return -1; /* other MMI0 sub-opcodes (PCGTW/PMAXW/PEXTLW/... etc): not yet JIT-compiled */
    }

    if (op == 0x1Cu && funct == 0x09u) {
        /* Round 915 (task #900): MMI2 multiply/divide family - PMULTW
         * (sa=0x0C), PDIVW (sa=0x0D), PMULTH (sa=0x1C), PDIVBW
         * (sa=0x1D). IMPORTANT naming note: this project's own
         * ee_core.c interpreter labels its `case 0x09:` block "MMI2"
         * in its comment (matching real R5900 EE Core hardware - the
         * funct 0x09 sub-group really is MMI2, and funct 0x28 is
         * really MMI1; a prior session's summary had these two swapped,
         * corrected here after re-reading the actual interpreter source
         * directly rather than trusting the stale note).
         *
         * All four opcodes involve substantial 64-bit HI:LO-pipe-pair
         * arithmetic with real-hardware edge-case handling (INT32_MIN/
         * -1 overflow guards, MIPS sign-of-dividend div-by-zero
         * convention, PDIVBW's broadcast-one-halfword-divisor-across-
         * four-lanes quirk) that would take many dozens of individual
         * PPC750 instructions to hand-translate bit-exactly, at real
         * risk of silently drifting from the interpreter's own
         * behavior. Rather than do that (as Round 914's simpler add/
         * sub-only MMI0 family did), this round uses the established
         * C-function-call trampoline pattern instead (first used for
         * LW/SW in Round 891, extended through SQRT.S/CVT.S.W in
         * Rounds 905/906b): emit a call into a small dedicated C helper
         * per opcode (ee_jit_helper_pmultw/pdivw/pmulth/pdivbw, defined
         * in ee_core.c - see that file's own Round 915 comment), each a
         * byte-for-byte port of the real interpreter case body, so the
         * JIT and interpreter can never silently disagree here. rs/rt/
         * rd are compile-time-constant instruction fields (this JIT
         * compiles exactly one MIPS instruction per block), so they're
         * passed as plain li-loaded integer arguments in r4/r5/r6 -
         * SCRATCH_A/B/C's register numbers exactly match the EABI's
         * arg2/arg3/arg4 slots, so no extra register-shuffling is
         * needed beyond the li itself. No result flows back into any
         * PPC register after the call (the helper writes gpr[rd]/hi/lo
         * directly through the ctx pointer) - same "SW-style" simpler
         * frame as the plain-write trampolines above, no r15 (saved
         * ctx) needed, only r14 (saved LR) crosses the call. */
        /* Round 916 (task #901): PAND (sa=0x12) / PXOR (sa=0x13) - full
         * 128-bit bitwise AND/XOR, checked first (before the Round 915
         * muldiv sentinel selection below) since these are plain
         * inline codegen, not trampoline calls. Real ee_core.c bodies:
         * `if (rd) { gpr[rd].ud0 = gpr[rs].ud0 OP gpr[rt].ud0;
         * gpr[rd].ud1 = gpr[rs].ud1 OP gpr[rt].ud1; }` - a per-bit op
         * with no cross-word carry, so it decomposes cleanly into four
         * independent 32-bit word operations (REG_HI/REG_LO for ud0,
         * REG_HI1/REG_LO1 for ud1), same word-slot addressing Round
         * 914's MMI0 family already established (just without that
         * round's lane-permutation indirection - AND/XOR/OR/NOR don't
         * reorder bits across word boundaries, so the four words can
         * be processed in any order and REG_HI(r)/REG_LO(r)/REG_HI1(r)/
         * REG_LO1(r) can be used directly). Always computed, store
         * skipped when rd==0 (same convention as Round 914). */
        if (sa == 0x12u || sa == 0x13u) {
            int16_t offs[4] = { REG_HI(rs), REG_LO(rs), REG_HI1(rs), REG_LO1(rs) };
            int16_t offt[4] = { REG_HI(rt), REG_LO(rt), REG_HI1(rt), REG_LO1(rt) };
            int16_t offd[4] = { REG_HI(rd), REG_LO(rd), REG_HI1(rd), REG_LO1(rd) };
            for (int w = 0; w < 4; w++) {
                emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, offs[w]));
                emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, offt[w]));
                if (sa == 0x12u)
                    emit(ctx, enc_and(SCRATCH_A, SCRATCH_A, SCRATCH_B));
                else
                    emit(ctx, enc_xor(SCRATCH_A, SCRATCH_A, SCRATCH_B));
                if (rd != 0)
                    emit(ctx, enc_stw(SCRATCH_A, CTX_REG, offd[w]));
            }
            return 0;
        }

        uint32_t helper_addr;
        if (sa == 0x0Cu)      helper_addr = ADDR_EE_JIT_PMULTW;
        else if (sa == 0x0Du) helper_addr = ADDR_EE_JIT_PDIVW;
        else if (sa == 0x1Cu) helper_addr = ADDR_EE_JIT_PMULTH;
        else if (sa == 0x1Du) helper_addr = ADDR_EE_JIT_PDIVBW;
        else return -1; /* other MMI2 sub-opcodes (PMFHI/PMADDW/... etc): not yet JIT-compiled */

        emit(ctx, enc_addi(1, 1, -32));
        emit(ctx, enc_stw(14, 1, 8));
        emit(ctx, enc_mflr(14));
        emit(ctx, enc_addi(SCRATCH_A, 0, (int16_t)rs)); /* r4 = rs (arg2); r3=ctx already arg1 */
        emit(ctx, enc_addi(SCRATCH_B, 0, (int16_t)rt)); /* r5 = rt (arg3) */
        emit(ctx, enc_addi(SCRATCH_C, 0, (int16_t)rd)); /* r6 = rd (arg4) */
        emit_load_const32(ctx, 12, helper_addr);
        emit(ctx, enc_mtctr(12));
        emit(ctx, enc_bctrl());                       /* ee_jit_helper_pXXXX(ctx, rs, rt, rd) */
        emit(ctx, enc_mtlr(14));
        emit(ctx, enc_lwz(14, 1, 8));
        emit(ctx, enc_addi(1, 1, 32));
        return 0;
    }

    if (op == 0x1Cu && funct == 0x29u) {
        /* Round 916 (task #901): MMI3 - POR (sa=0x12) / PNOR (sa=0x13).
         * Same per-word bitwise decomposition as PAND/PXOR just above;
         * PNOR is `~(a|b)` on each of ud0/ud1, so it's an enc_nor per
         * word rather than enc_or followed by a separate complement -
         * PPC750 has a native nor instruction, matching this exactly.
         * Real ee_core.c bodies confirmed at ee_core.c's `case 0x29:
         * MMI3` block, sa 0x12/0x13 (grep-verified this round; distinct
         * from MMI2's own sa=0x12/0x13, which are PAND/PXOR - same sa
         * values, different funct/meta-group, real hardware quirk). */
        if (sa == 0x12u || sa == 0x13u) {
            int16_t offs[4] = { REG_HI(rs), REG_LO(rs), REG_HI1(rs), REG_LO1(rs) };
            int16_t offt[4] = { REG_HI(rt), REG_LO(rt), REG_HI1(rt), REG_LO1(rt) };
            int16_t offd[4] = { REG_HI(rd), REG_LO(rd), REG_HI1(rd), REG_LO1(rd) };
            for (int w = 0; w < 4; w++) {
                emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, offs[w]));
                emit(ctx, enc_lwz(SCRATCH_B, CTX_REG, offt[w]));
                if (sa == 0x12u)
                    emit(ctx, enc_or(SCRATCH_A, SCRATCH_A, SCRATCH_B));
                else
                    emit(ctx, enc_nor(SCRATCH_A, SCRATCH_A, SCRATCH_B));
                if (rd != 0)
                    emit(ctx, enc_stw(SCRATCH_A, CTX_REG, offd[w]));
            }
            return 0;
        }
        return -1; /* other MMI3 sub-opcodes (PMTHI/PMTLO/PCPYUD/... etc): not yet JIT-compiled */
    }

    if (op == 0x1Cu && (funct == 0x34u || funct == 0x36u || funct == 0x37u ||
                         funct == 0x3Cu || funct == 0x3Eu || funct == 0x3Fu)) {
        /* Round 917 (task #902): MMI shift family - PSLLH (funct=0x34),
         * PSRLH (0x36), PSRAH (0x37), PSLLW (0x3C), PSRLW (0x3E),
         * PSRAW (0x3F). Unlike MMI0-3, these are TOP-LEVEL direct-funct
         * MMI opcodes - ee_core.c's `case 0x1C: switch(funct)` dispatches
         * straight to them (grep-confirmed at ee_core.c lines 9237-9242),
         * no intermediate sa-based sub-group the way funct 0x08/0x09/0x29
         * work. All six shift rt ONLY (never rs) by the compile-time-
         * constant sa field, write to rd (skipped when rd==0, matching
         * every prior MMI round's convention). Real semantics:
         * PSLLH/PSRLH/PSRAH operate on 8x 16-bit lanes with the shift
         * masked to sa&0xF (0-15); PSLLW/PSRLW/PSRAW operate on 4x
         * 32-bit lanes with the full 5-bit sa (0-31), per-lane, via
         * set_lane_h/set_lane_w in the interpreter. */
        if (funct == 0x3Cu || funct == 0x3Eu || funct == 0x3Fu) {
            /* W-family: each of the 4 memory words IS one lane already -
             * native PPC750 shift-by-immediate applies directly, no
             * lane-packing concerns (unlike H below, where two lanes
             * share one 32-bit word). slwi/srwi are the standard
             * rlwinm-based idioms (rA,rS,n,0,31-n and rA,rS,(32-n)&31,
             * n,31 respectively); PSRAW's arithmetic shift is a real
             * srawi, PPC750's native instruction for it. */
            int shamt = (int)(sa & 0x1Fu);
            int16_t offt[4] = { REG_HI(rt), REG_LO(rt), REG_HI1(rt), REG_LO1(rt) };
            int16_t offd[4] = { REG_HI(rd), REG_LO(rd), REG_HI1(rd), REG_LO1(rd) };
            for (int w = 0; w < 4; w++) {
                emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, offt[w]));
                if (funct == 0x3Cu)      /* PSLLW: slwi rA,rS,n */
                    emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, shamt, 0, 31 - shamt));
                else if (funct == 0x3Eu) /* PSRLW: srwi rA,rS,n */
                    emit(ctx, enc_rlwinm(SCRATCH_A, SCRATCH_A, (32 - shamt) & 31, shamt, 31));
                else                      /* PSRAW: srawi rA,rS,n */
                    emit(ctx, enc_srawi(SCRATCH_A, SCRATCH_A, shamt));
                if (rd != 0)
                    emit(ctx, enc_stw(SCRATCH_A, CTX_REG, offd[w]));
            }
            return 0;
        }

        /* H-family: PSLLH/PSRLH/PSRAH. Each 32-bit memory word packs
         * TWO independent 16-bit lanes that must not bleed into each
         * other - shifting the whole 32-bit word directly (as the
         * W-family does) would let bits cross the halfword boundary,
         * which real hardware never does. Per word: extract each
         * 16-bit half right-aligned into its own scratch register
         * (rlwinm(x,x,0,16,31) for the low half, rlwinm(x,x,16,16,31)
         * for the high half - the standard "extract low/high halfword"
         * idiom, hand-verified bit-exactly against concrete examples
         * before use here), apply the shift to each half independently
         * (PSLLH/PSRLH reuse the exact same slwi/srwi-style rlwinm
         * formulas as the W-family above - still exact for a 16-bit
         * field, since the extraction step already zeroed the other 16
         * bits: shifting a zero-extended value left and re-masking to
         * 16 bits is bit-exact truncation, and shifting right is
         * bit-exact zero-fill since there's nothing above bit15 to leak
         * in), then reassemble via shift-left-16 + or. PSRAH needs real
         * 16-bit sign extension (not the zero-extension the isolation
         * step leaves behind) before its arithmetic shift: first shift
         * the isolated half up into the TOP of the register (via
         * rlwinm(x,x,16,0,15), so its bit15 becomes the register's true
         * sign bit), then a real srawi by 16+s - the low 16 bits of
         * that result are exactly (int16_t)H >> s two's-complement
         * floor-shift, verified by hand against a negative test value
         * (0x8001 >> 1 -> 0xC000) before use here - and the final
         * rlwinm(x,x,0,16,31) discards the extra sign-extension bits
         * above bit15, matching the interpreter's (uint16_t) cast. */
        int s = (int)(sa & 0xFu);
        int16_t offt[4] = { REG_HI(rt), REG_LO(rt), REG_HI1(rt), REG_LO1(rt) };
        int16_t offd[4] = { REG_HI(rd), REG_LO(rd), REG_HI1(rd), REG_LO1(rd) };
        for (int w = 0; w < 4; w++) {
            emit(ctx, enc_lwz(SCRATCH_A, CTX_REG, offt[w]));          /* A = whole word */
            emit(ctx, enc_rlwinm(SCRATCH_B, SCRATCH_A, 0, 16, 31));   /* B = lo16 (right-aligned) */
            emit(ctx, enc_rlwinm(SCRATCH_C, SCRATCH_A, 16, 16, 31));  /* C = hi16 (right-aligned) */
            if (funct == 0x34u) { /* PSLLH */
                emit(ctx, enc_rlwinm(SCRATCH_B, SCRATCH_B, s, 16, 31)); /* (lo16<<s)&0xFFFF */
                emit(ctx, enc_rlwinm(SCRATCH_C, SCRATCH_C, s, 16, 31)); /* (hi16<<s)&0xFFFF */
            } else if (funct == 0x36u) { /* PSRLH */
                emit(ctx, enc_rlwinm(SCRATCH_B, SCRATCH_B, (32 - s) & 31, s, 31));
                emit(ctx, enc_rlwinm(SCRATCH_C, SCRATCH_C, (32 - s) & 31, s, 31));
            } else { /* PSRAH */
                emit(ctx, enc_rlwinm(SCRATCH_B, SCRATCH_B, 16, 0, 15));  /* B <<= 16 (sign->bit31) */
                emit(ctx, enc_srawi(SCRATCH_B, SCRATCH_B, 16 + s));
                emit(ctx, enc_rlwinm(SCRATCH_B, SCRATCH_B, 0, 16, 31));  /* truncate to 16 bits */
                emit(ctx, enc_rlwinm(SCRATCH_C, SCRATCH_C, 16, 0, 15));
                emit(ctx, enc_srawi(SCRATCH_C, SCRATCH_C, 16 + s));
                emit(ctx, enc_rlwinm(SCRATCH_C, SCRATCH_C, 0, 16, 31));
            }
            emit(ctx, enc_rlwinm(SCRATCH_C, SCRATCH_C, 16, 0, 15));   /* hi16' << 16 into position */
            emit(ctx, enc_or(SCRATCH_A, SCRATCH_C, SCRATCH_B));       /* reassemble */
            if (rd != 0)
                emit(ctx, enc_stw(SCRATCH_A, CTX_REG, offd[w]));
        }
        return 0;
    }

    /* Unsupported: remaining REGIMM sub-opcodes (BLTZAL/BGEZAL/-ALL,
     * trap instructions), the rest of MMI, COP1/2, everything else.
     * Every base conditional/unconditional MIPS branch/jump opcode this
     * project's boot traces are known to exercise is now handled:
     * J/JAL/JR/JALR, BEQ/BNE/BLEZ/BGTZ/BLTZ/BGEZ, and their six
     * "likely" counterparts (BEQL/BNEL/BLEZL/BGTZL/BLTZL/BGEZL). */
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
