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

/* Byte offset of MIPS register `r`'s ppc_dynarec_gpr128_t slot within
 * the context array (16 bytes/slot: 8-byte ud0 + 8-byte ud1 - see the
 * header's endianness note before touching these). REG_HI/REG_LO give
 * the offsets of ud0's two 32-bit halves; REG_HI is the semantically
 * "high 32 bits" word regardless of host byte order because PPC750/
 * Broadway is big-endian, so it sits at the LOWER address. */
#define REG_SLOT(r)  ((int16_t)((r) * 16))
#define REG_HI(r)    ((int16_t)(REG_SLOT(r) + 0))
#define REG_LO(r)    ((int16_t)(REG_SLOT(r) + 4))

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
     * 32-instruction ceiling, so again no change to `words` was needed. */
    size_t words = max_instructions * 32 + 1;
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
    if (ctx->used_words + 32 > ctx->capacity_words) /* Round 890: was 20, DIV/DIVU's 32-word worst case */
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

    /* Unsupported: branches, loads/stores other than LB/LBU/LH/LHU/
     * LW/LWU/SB/SH/SW above (LD/SD still unsupported - their 64-bit
     * value calling convention needs register-pair handling this file
     * hasn't built yet), MMI, COP1/2, everything else. Real coverage
     * would require this switch to be the size of ee_core's
     * interpreter (or larger, with scheduling). */
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
