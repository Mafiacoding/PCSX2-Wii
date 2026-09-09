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
 *
 * Round 891 (task #875) update: LW/SW are this PoC's first opcodes
 * that need to call a REAL C function (ee_mem_read32/ee_mem_write32)
 * instead of only moving bits between the context array and PPC
 * registers - real memory access has side effects (MMIO, TLB, EE
 * exceptions) this PoC has no intention of reimplementing a second
 * time in generated code. ppc_dynarec.c now knows how to emit a small
 * stack frame (to spill/restore the two non-volatile GPRs it borrows
 * as scratch across the call, per the PowerPC EABI) plus the
 * lis+ori/mtctr/bctrl call sequence itself; see that file's own
 * ADDR_EE_MEM_READ32/WRITE32 comment for exactly how the callee's
 * absolute address is obtained (differently on GEKKO vs. host-native
 * verification builds) and its LW/SW dispatch blocks' comments for the
 * full register-preservation walkthrough.
 *
 * Round 892 (task #876) update: LB/LBU/LH/LHU/LWU/SB/SH extend Round
 * 891's call-emission mechanism to the rest of the base-ISA byte/
 * halfword/unsigned-word loads and stores - same stack frame and call
 * sequence, just different callees (ee_mem_read8/16, ee_mem_write8/16)
 * and, for the loads, an extra sign/zero-extend step (extsb/extsh/
 * andi.) before the usual store-back. LD/SD are NOT included this
 * round - their 64-bit value calling convention needs register-pair
 * argument/return handling this file hasn't built yet, left for a
 * future round.
 *
 * Round 893 (task #877) update: LD/SD complete the full base-ISA
 * integer load/store family, using the PowerPC 32-bit EABI's register-
 * PAIR convention for a genuine 64-bit callee value (r3:r4 hi:lo for
 * ee_mem_read64's return, r5:r6 hi:lo for ee_mem_write64's argument) -
 * see ppc_dynarec.c's ADDR_EE_MEM_READ64/WRITE64 comment.
 *
 * Round 894 (task #878) update: J/JAL/JR/JALR - the full set of
 * unconditional control-transfer opcodes. These are this dynarec's
 * FIRST opcodes that read/write ee_state_t fields other than the flat
 * gpr[32]+hi+lo register array: `exc_this_pc` (to compute J/JAL's
 * absolute jump target), and `next_pc`/`branch_pending` (to hand
 * control flow back to ee_step() exactly the way its own BRANCH_TO()
 * macro does). This works with zero new addressing mechanism because
 * this file's context pointer (CTX_REG/r3) is `&st->gpr[0]`, which is
 * ALSO byte offset 0 of the whole ee_state_t struct (gpr is its first
 * field) - so any ee_state_t field is reachable as a plain lwz/stw/stb
 * at its real offset; see ppc_dynarec.c's EXC_THIS_PC_OFFSET/
 * NEXT_PC_OFFSET/BRANCH_PENDING_OFFSET comment for the full rationale,
 * including why reading this_pc from CONTEXT at every execution (not
 * baking it into the generated code) is what keeps J/JAL correct under
 * this dynarec's instruction-encoding-keyed cache even though their
 * absolute target genuinely depends on WHERE the instruction sits in
 * memory. Conditional branches (BEQ/BNE/BLEZ/BGTZ/BLTZ/BGEZ/...) are
 * NOT included this round - they need a real 64-bit signed/equality
 * compare emitted as PPC condition-register logic, a codegen
 * capability this file doesn't have yet, left for a
 * future round.
 *
 * Round 895 (task #879) update: BEQ/BNE/BLEZ/BGTZ - four of the six
 * non-REGIMM conditional branches. Contrary to Round 894's own
 * prediction above, these do NOT need real PPC condition-register
 * branch logic - they reuse the all-0s/all-1s "taken" mask technique
 * MOVZ/MOVN (Round 889) and SLT/SLTU (Round 886) already established
 * for conditional writes, applied here to next_pc/branch_pending via a
 * new shared helper, emit_branch_blend(). Every generated block stays a
 * single straight-line run with zero internal control flow, exactly
 * like every opcode before it. BLTZ/BGEZ (REGIMM) and the "likely"
 * variants of every conditional branch (which need delay-slot
 * annulment - a genuinely new capability) are left for a future round.
 *
 * Round 896 (task #880) update: BLTZ/BGEZ (REGIMM) and all six "likely"
 * branches (BLTZL/BGEZL/BEQL/BNEL/BLEZL/BGTZL) complete the branches/
 * jumps arc Round 894 opened. BLTZ/BGEZ were the trivial srawi-sign-bit
 * extension Round 895 predicted; the "likely" variants needed the
 * genuinely new capability Round 895 flagged - delay-slot annulment -
 * implemented as a new emit_branch_blend_likely() helper that blends
 * pc/next_pc/branch_pending together (this dynarec's first opcodes to
 * write ee_state_t.pc directly) using the exact not-taken semantics
 * grepped from this project's own ee_core.c interpreter: pc jumps
 * straight to this_pc+8, skipping the delay slot entirely, rather than
 * executing it. Every generated block is still a single straight-line
 * PPC sequence with zero real branch instructions, unchanged since
 * Round 895. This is the last round in the branches/jumps arc - every
 * base MIPS conditional/unconditional control-transfer opcode this
 * project's boot traces exercise is now JIT-compiled.
 *
 * Round 897 (task #881) update: ANDI/ORI/XORI/ADDI complete the base-ISA
 * ALU-immediate family (SLTI/SLTIU/LUI were already covered earlier).
 * ADDI simply joins ADDIU's existing dispatch condition unchanged - real
 * MIPS ADDI traps on signed overflow where ADDIU doesn't, but this
 * project's own interpreter deliberately never implements that trap
 * (the same simplification DADDI/DADDIU make one level down), so the two
 * opcodes are byte-for-byte identical here too. ANDI/ORI/XORI exploit a
 * simplification unique to logical (not arithmetic) immediates: the
 * 16-bit immediate is ZERO-extended, never sign-extended, which makes
 * the high-word combine collapse to a constant instead of a real op -
 * ANDI's high result is always exactly 0 (anything AND an implicit-zero
 * upper half), ORI/XORI's high result is always the source's high word
 * UNCHANGED (anything OR/XOR 0 is a no-op). A new enc_xori() D-form
 * encoder (opcode 26) was added and verified bit-for-bit against real
 * devkitPPC output; andi. (opcode 28, used internally since Round 886)
 * and ori (opcode 24, since Round 891) already existed. See
 * docs/STATUS.md's Round 897 section for the full verification writeup
 * (19/19 checks, two test-harness-only bugs caught before shipping) and
 * for what's left: the 64-bit-native DADD/DSUB/DSLL/DSRL/DSRA family
 * (task #882, Round 898-899), which has no 32-bit-then-sign-extend
 * shortcut available.
 *
 * Round 898 (task #882) update: DADD/DADDU/DSUB/DSUBU (funct 0x2C-0x2F)
 * and DSLL/DSRL/DSRA (funct 0x38/0x3A/0x3B) are this dynarec's FIRST
 * genuinely 64-bit-native register-register/shift opcodes - every prior
 * ALU opcode got away with a 32-bit-compute-then-sign-extend shortcut
 * that doesn't apply here. DADD/DSUB synthesize a real 64-bit add/
 * subtract from two 32-bit PPC750 halves via the standard add-with-
 * carry/subtract-with-borrow chain (new addc/adde encoders for the add
 * side, verified against real devkitPPC; DSUB reuses Round 886's
 * existing subfc/subfe unchanged). DSLL/DSRL/DSRA shift each half by sa
 * and OR in the bits that cross the hi/lo boundary from the other half
 * (shifted by the complementary 32-sa amount) - reusing Round 888's
 * existing slw/srw/sraw encoders with no new ones needed; the sa==0
 * edge case needs no special-casing at all because real PPC750 hardware
 * already treats a >=32 shift count as "result is zero/all-sign-bits",
 * which is exactly what a zero-amount boundary-crossing term needs. See
 * docs/STATUS.md's Round 898 section for the full derivation and
 * verification writeup (27/27 checks, first attempt, no bugs). Next:
 * the EE-specific unaligned/128-bit loads - LWL/LWR/SWL/SWR/LQ/SQ (task
 * #883, Round 900-901).
 *
 * Round 900 (task #883) update: LWL/LWR/SWL/SWR (opcodes 0x22/0x26/
 * 0x2A/0x2E) and LQ/SQ (opcodes 0x1E/0x1F) are now JIT-accelerated.
 * ee_core.c's own LWL_MASK/LWL_SHIFT/LWR_MASK/LWR_SHIFT/SWL_MASK/
 * SWR_MASK 4-entry lookup tables (indexed by the runtime 2-bit
 * `shift = addr&3`) are NOT reproduced as tables in generated code -
 * every entry collapses to a formula of `shift`, computed with plain
 * register arithmetic instead (see the inline comments at each
 * dispatch block in ppc_dynarec.c for the exact per-opcode formulas).
 * The shift==3 (LWL/SWL) and shift==0 (LWR mask, SWR shift/mask) edge
 * cases divide by "shift 32", which real PPC750 slw/srw's own
 * ">=32 -> zero" hardware rule (Round 898's DSLL/DSRL/DSRA reliance,
 * reused here for a RUNTIME rather than compile-time shift amount)
 * handles with no special-case branch. SWL/SWR/LQ/SQ are this
 * dynarec's first opcodes to call a real C function TWICE in one
 * compiled block (SWL/SWR: read-merge-write via ee_mem_read32 then
 * ee_mem_write32; LQ/SQ: two ee_mem_read64/ee_mem_write64 calls, one
 * per 64-bit half of the EE's 128-bit register) - this requires never
 * trusting a volatile scratch register (r4-r11, caller-saved per the
 * PowerPC EABI) to survive a `bctrl` call; every value needed after a
 * call is instead recomputed from context via r15 (the non-volatile
 * saved ctx pointer) once the call returns. New REG_HI1()/REG_LO1()
 * macros address the EE 128-bit register's upper 64 bits (`ud1`,
 * bytes 8-15 of each 16-byte slot) for LQ/SQ, alongside the existing
 * REG_HI()/REG_LO() pair for `ud0`. LQ matches ee_core.c's real
 * behavior of skipping the read ENTIRELY when rt==$0 (unlike every
 * other load in this dynarec, which still performs the read for its
 * memory side effects); SQ has no such guard and always writes both
 * halves, matching ee_core.c exactly. A host-native verification
 * harness caught two genuine bugs before this shipped: SWR's mask
 * formula was originally coded as `0xFFFFFFFF>>shift8` (copied
 * incorrectly from LWR_MASK's structurally similar but NOT identical
 * term) instead of the correct `0xFFFFFFFF>>(32-shift8)` - fixed by
 * computing `32-shift8` explicitly before the shift. See docs/
 * STATUS.md's Round 900 section for the full verification writeup
 * (25/25 checks, after fixing that mask bug plus a couple of test-
 * harness bugs of its own). Next: JIT COP1 FPU opcodes (task #884,
 * Round 902-906).
 *
 * Round 902 (task #884) update: MFC1/CFC1/MTC1/CTC1 (op 0x11, rs-
 * selected) and MOV.S/ABS.S/NEG.S (op 0x11, rs==0x10/COP1.S, funct-
 * selected) are now JIT-accelerated - the first slice of the FPU family,
 * deliberately scoped to exclude every opcode needing real floating-
 * point arithmetic (ADD.S/SUB.S/MUL.S/DIV.S/SQRT.S/etc, CVT.W.S/
 * CVT.S.W, the BC1 branch family), which need genuine PPC750 FPU
 * instructions and PCSX2's own overflow/underflow clamping ported
 * faithfully - saved for a later round in this same task's Round
 * 902-906 range. Every opcode here operates on FPR/GPR/FCR31 raw 32-bit
 * bit patterns with plain integer loads/stores/logical ops (MOV.S: copy;
 * ABS.S: rlwinm clearing bit 0; NEG.S: xoris flipping bit 31), matching
 * ee_core.c's own case bodies exactly - no float hardware touched at
 * all. New REG_FPR()/FCR31_OFFSET/ACC_OFFSET constants (fpr[0] at byte
 * offset 1464, fcr31 at 1592, acc at 1596 - confirmed via a real
 * offsetof() probe) reach ee_state_t's COP1 fields the same way every
 * other *_OFFSET constant does. CFC1/CTC1's `rd` field is a compile-time
 * constant, so both specialize to fixed-shape codegen per rd value
 * (CFC1: rd==31 real read / rd==0 fixed 0x2E00 / else always-0; CTC1:
 * rd==31 real write / else a true no-op, zero instructions emitted) with
 * no runtime branching. See docs/STATUS.md's Round 902 section for the
 * full verification writeup (20/20 checks, one harness bug caught and
 * fixed - a wrong xoris opcode number in the test harness itself, not in
 * the generated code). Next: the real FPU arithmetic family (Round
 * 903+), this dynarec's first genuine PPC750 FPU instructions.
 *
 * Round 903 (task #884) update: ADD.S/SUB.S/MUL.S (op 0x11, rs==0x10/
 * COP1.S, funct 0x00/0x01/0x02) are now JIT-accelerated - this dynarec's
 * first opcodes using REAL PPC750 floating-point instructions (lfs/
 * stfs/fadds/fsubs/fmuls, all new encoders this round). PPC has no
 * GPR<->FPR move instruction, so every bit-pattern handoff between the
 * integer and float register files goes through a small private stack
 * frame pushed/popped around the whole sequence (stw+lfs to go GPR->FPR,
 * stfs+lwz to come back).
 *
 * The real work is faithfully reproducing PCSX2's fpu_double()/
 * fpu_check_overflow()/fpu_check_underflow() semantics in PPC integer
 * code, since real PPC750 float hardware alone doesn't match them:
 * fpu_double() is applied to BOTH source operands before arithmetic
 * (denormal-or-zero magnitude -> signed zero, infinity/NaN magnitude
 * -> signed Fmax 0x7F7FFFFF), and fpu_check_overflow()+
 * fpu_check_underflow() are applied to the result after (the SAME
 * transform, just described as two separate ee_core.c helper functions
 * for a result rather than an operand). New emit_fpu_clamp32() helper
 * implements this ONE transform branchlessly (magnitude-range mask-
 * blend, reusing the exact subfc/subfe carry-to-mask idiom already
 * established for SLT/SLTU/branch-condition masks) and is called three
 * times per opcode: once for each input operand, once for the result -
 * no real PPC branch instructions, keeping this whole file's "every
 * compiled block is one straight-line run" invariant intact even
 * though the underlying logic is a three-way conditional.
 *
 * ADD.S/SUB.S/MUL.S each emit roughly 61 PPC750 instructions (2x
 * 15-instruction clamp calls for the operands + a 3rd for the result,
 * plus stack-frame/spill/fill plumbing) - by far the largest single-
 * opcode instruction count in this dynarec so far, prompting a bump of
 * the per-block buffer-capacity constant from 40 to 80 words (see
 * ppc_dynarec_init()'s own comment). See docs/STATUS.md's Round 903
 * section for the full verification writeup (12/12 checks - basic
 * arithmetic, negative operands, overflow-to-Fmax clamping in both
 * signs, underflow-to-signed-zero clamping, denormal/infinity/NaN
 * input clamping, and exact-zero passthrough - all passed on the first
 * run, clean under -fsanitize=address,undefined).
 *
 * Round 904 (task #884) update: DIV.S (funct 0x03) is now JIT-
 * accelerated, using the enc_fdivs encoder added-but-unused in Round
 * 903. Re-verified against ee_core.c's real DIV.S case body (not
 * trusted from memory) before implementing: the divide-by-zero special
 * case tests the RAW divisor's exponent field (denormal counts as zero
 * too) BEFORE any fpu_double() clamp, returning a signed Fmax whose
 * sign is the XOR of the RAW operand signs - entirely bypassing the
 * real division. This can't be reproduced as a shortcut ("let real
 * fdivs run on the clamped operands, feed its Infinity/NaN result
 * through the existing overflow clamp"): that shortcut agrees with the
 * XOR formula whenever the dividend is nonzero, but disagrees on the
 * 0/0-class case, since real PPC750 hardware's 0.0f/0.0f gives a
 * canonical NaN with an implementation-defined (effectively always-
 * positive) sign bit, not sign=XOR(dividend,divisor). Implemented as
 * an explicit branchless blend instead: a subfc/subfe "iszero" mask on
 * the divisor's exponent field selects between the explicit divide-by-
 * zero result and the normal clamp->fdivs->clamp path (which still
 * runs unconditionally either way, its result just discarded when the
 * mask fires) - preserving the straight-line-block invariant. Comes to
 * ~81 PPC750 instructions (new record, was ~61 for Round 903), needing
 * a 32-byte private stack frame (double Round 903's 16 bytes) to stash
 * the raw operands and the divide-by-zero mask/result. Bumped the
 * per-block buffer-capacity constant from 80 to 128 words, jumping
 * ahead of the immediate need since the MADD/MSUB ACC-register family
 * (Round 906) is expected to need a comparable amount. See docs/
 * STATUS.md's Round 904 section for the full verification writeup
 * (13/13 checks, including the critical -0.0/+0.0 case that
 * specifically distinguishes this implementation from the naive
 * hardware-NaN shortcut - all passed on the first run). Next: SQRT.S/
 * RSQRT.S/MAX.S/MIN.S, then the MADD-family/comparison-family
 * (Round 906), CVT.W.S/CVT.S.W, and the BC1 branch family (Round 906b).
 *
 * Round 905 (task #889) update: SQRT.S/RSQRT.S (funct 0x04/0x16) and
 * MAX.S/MIN.S (funct 0x28/0x29) are now JIT-accelerated. Before writing
 * any code, resolved a real hardware-safety question EMPIRICALLY (the
 * fetched IBM Gekko manual PDF's extracted text had zero hits for
 * "fsqrt", so documentation alone didn't answer it): does real PPC750/
 * Gekko silicon safely support fsqrts/fsqrt? devkitPPC's own GCC
 * (-mcpu=750 -mhard-float) compiles `sqrtf(x)` as a tail-call `b sqrtf`,
 * never an inlined hardware sqrt, and the real linked libm.a's
 * __ieee754_sqrtf is ~100+ instructions of pure software bit-twiddling
 * with zero use of any hardware sqrt opcode - decisive evidence this
 * dynarec must NOT emit fsqrts/fsqrt directly. Added ADDR_EE_SQRTF
 * (sentinel 0x109 on host, the real sqrtf symbol's address on GEKKO)
 * alongside the existing ADDR_EE_MEM_READ32-style dual-address macros,
 * and SQRT.S/RSQRT.S call the real linked sqrtf() through a
 * C-function-call trampoline - same LR-via-r14/ctx-via-r15 convention
 * as LW/SW's Round 891 trampoline, but with the argument/return in f1
 * (EABI float arg/return register) instead of r3/r4. Notable finding
 * specific to this trampoline style: SCRATCH_A/SCRATCH_B do NOT survive
 * the call (r3-r12 are all EABI volatile) and must be reloaded via
 * emit_load_const32 afterward if needed again - the first time a call
 * in this dynarec has silently invalidated scratch state that survived
 * every non-calling opcode's usage pattern. SQRT.S's source is `ft`
 * (fs unused, a genuine real-hardware/PCSX2 quirk), has a signed-zero
 * special case for zero-exponent ft and NO output clamp; a notable
 * real-hardware behavior specifically test-cased: sqrt of a NEGATIVE ft
 * returns sqrt(|ft|), not NaN. RSQRT.S shares SQRT.S's denominator
 * computation but its special-case sign comes from ft ALONE (no xor
 * with fs, unlike DIV.S) and it DOES apply the standard overflow/
 * underflow output clamp. MAX.S/MIN.S do a bit-level signed-int
 * max/min on raw register contents with NO clamping anywhere (verified
 * absent from the real case bodies) - implemented via the same signed-
 * compare subfc/subfe-borrow-to-mask idiom emit_slt_core already uses,
 * just on single 32-bit words instead of a 64-bit hi/lo pair. See
 * docs/STATUS.md's Round 905 section for the full verification writeup
 * (20/20 checks, all passed on the first run). Next: the MADD/MSUB/
 * ADDA/SUBA/MULA family and C.cond.S comparisons (Round 906), then
 * CVT.W.S/CVT.S.W and the BC1 branch family (Round 906b) to close out
 * task #884.
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
