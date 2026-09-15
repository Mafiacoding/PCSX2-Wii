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
 *
 * Round 906 (task #890) update: ADDA.S/SUBA.S/MULA.S (funct
 * 0x18/0x19/0x1A, write ACC not fpr[fd]), MADDA.S/MSUBA.S (funct
 * 0x1E/0x1F, ACC +/= fs*ft with NO second clamp pass on the product),
 * MADD.S/MSUB.S (funct 0x1C/0x1D, fd = ACC +/- fs*ft WITH a second
 * clamp pass on the product - a real hardware/PCSX2 quirk verified
 * directly from ee_core.c and deliberately NOT made consistent with
 * the MADDA/MSUBA variants), and C.EQ.S/C.LT.S/C.LE.S (funct
 * 0x32/0x34/0x36, write fcr31 bit 0x00800000 only) are now
 * JIT-accelerated. The comparison family uses this dynarec's first
 * CR-based instructions - real PPC750 hardware fcmpu + mfcr, chosen
 * over a hand-rolled integer bit-pattern ordering trick specifically
 * because real hardware gets the -0.0==+0.0 edge case right for free
 * (a naive sign-flip-then-unsigned-compare trick would get it wrong -
 * see docs/STATUS.md's Round 906 section for the worked example and a
 * dedicated regression test proving it). The fcr31 bit-clear sequence
 * is this project's first generated use of a WRAPPING rlwinm mask
 * (mb>me) - which also exposed and fixed a latent bug in the verify
 * harness's OWN rlwinm simulator (it only implemented the non-wrapping
 * mb<=me case; real generated code was always correct, only the test
 * tool's decode was incomplete). 93 opcodes now JIT-accelerated,
 * 21/21 checks passed. Next: CVT.W.S/CVT.S.W and the BC1/BC1L
 * branch-on-FP-condition family (Round 906b), closing out task #884.
 *
 * Round 906b (task #891) update: CVT.W.S (funct 0x24, float->int32,
 * uses real hardware fctiwz - confirmed present on Gekko unlike fsqrt -
 * spilled via a new stfd encoder since fctiwz has no direct FPR->GPR
 * move), CVT.S.W (funct 0x20 under a new rs=0x14 sibling block,
 * int32->float via the same call-trampoline pattern SQRT.S established,
 * extended for the first time to a mixed int-arg/float-return
 * signature since Gekko predates fcfid), and BC1F/BC1T/BC1FL/BC1TL (a
 * new rs=0x08 sibling block, routed through the existing
 * emit_branch_blend()/emit_branch_blend_likely() helpers unmodified)
 * are now JIT-accelerated, closing out task #884 (Rounds 902-906b, COP1
 * FPU JIT arc) entirely. The verify harness's first run (17/21) caught
 * a REAL bug in CVT.W.S's blend polarity: the subfc/subfe "borrow-to-
 * mask" idiom produces the OUT-of-range mask (E=0 when in-range, E=
 * allOnes when out-of-range), but the original blend had normal_val
 * masked by E and clamp_val by ~E - backwards, so every IN-range
 * conversion returned the out-of-range clamp value instead. Fixed by
 * swapping which mask each AND uses (2-line change, subfc/subfe
 * computation itself untouched) - re-ran 21/21 clean. See docs/
 * STATUS.md's Round 906b section for the full hex-trace derivation and
 * worked example. 96 opcodes now JIT-accelerated. Task #884 CLOSED.
 * Next: task #885, Round 907 - JIT VU0 macro-mode VADD/VSUB/VMUL.
 *
 * Round 907 (task #892) update: this dynarec's first VU0/COP2 opcodes
 * (op=0x12). New CO-format vector dispatch block handles VADD
 * (funct=0x28), VMUL (funct=0x2A), and VSUB (funct=0x2C): rs=0x10|
 * destmask selects the active lanes (bit3=X..bit0=W), FD[lane]=
 * FS[lane] OP FT[lane] with NO clamping (unlike every COP1.S op).
 * Because destmask is a compile-time-constant field of the
 * instruction's own encoding (unlike BC1's runtime fcr31 condition),
 * the emitter simply unrolls active lanes in a plain C loop at
 * translate time - no runtime branchless-masking machinery needed at
 * all, simpler than any COP1.S opcode. New VU0_VF_OFF(reg,lane) macro
 * addresses ee_state_t's vu0_vf[32][4] array (offset 1728, confirmed
 * via host-side offsetof(), pinned by a new _Static_assert in
 * ee_jit.c). Scalar MFC2-family transfers (rs<0x10), the broadcast
 * row, VMAX/VMINI, and VMADD/VMSUB (ACC-operand) are deliberately
 * left to Round 908 (task #893) onward. 31/31 checks passed under
 * -fsanitize=address,undefined, 0 leaks.
 *
 * Round 908 (task #893) update: fixed a real bug in Round 907's own
 * shipped code (found during this round's semantics research, not by
 * a test) - the VADD/VSUB/VMUL loop stored to VU0_VF_OFF(fd,lane)
 * unconditionally, never discarding writes to VF00 (fd==0) the way
 * vu0_vf_write_lane() and every other VF write in this file must.
 * Fixed with a compile-time `if (fd != 0)` guard (fd is known at
 * translate time, zero runtime cost). That same combined per-lane
 * loop now also handles VMAX(funct=0x2B)/VMINI(funct=0x2F) - a PLAIN
 * ternary per ee_core.c's real combined case body, NOT COP1 MAX.S/
 * MIN.S's sign-magnitude trick - via a new `enc_fsel` encoder (opcode
 * 63, xo=23, verified bit-for-bit against real devkitPPC output):
 * diff=fsubs(a,b), then fsel(diff,pick_a,pick_b). New VOPMSUB
 * (funct=0x2E) block: fixed xyz-only (no destmask field on real
 * hardware at all), reads the VU0 accumulator via a new
 * VU0_ACC_OFF(lane) macro (offset 10464, distinct from COP1's
 * ACC_OFFSET=1596). New SPECIAL2 sub-dispatch
 * ((funct&0x3C)==0x3C, idx=(instr&0x3)|((instr>>4)&0x7C)) handles
 * VABS(idx=29: dest=FT not FD, plain lwz/rlwinm/stw bitwise-AND,
 * ft==0 guard) and VCLIP(idx=31: 6 signed comparisons folded into 6
 * unsigned subfc/subfe compares via the sign-bit-flip trick, updating
 * the CLIP flag register at a new COP2_CTRL_OFF(18) - the most
 * involved opcode this dynarec has JIT'd to date). 8 of the ~15-20
 * real VU0 macro-mode opcodes now JIT-accelerated. 28/28 checks
 * passed under -fsanitize=address,undefined, 0 leaks.
 *
 * Round 909 (task #894) update: JIT VU0 VDIV(SPECIAL2 idx56)/
 * VSQRT(idx57)/VRSQRT(idx58) - the division/reciprocal-sqrt family
 * that writes the Q register (cop2_ctrl[22], new-this-round
 * COP2_CTRL_OFF(22) alias of the offset VMULq/VADDq already read).
 * Fsf/Ftf are compile-time-constant 2-bit lane selectors packed into
 * destmask (destmask&3=Fsf, (destmask>>2)&3=Ftf), so - unlike the
 * float VALUES these ops operate on - which VF lane feeds FS/FT is
 * baked in at JIT-translate time, same as every other CO-format op
 * this file handles. VU0's divide-by-zero test is a genuine IEEE
 * `ftv==0.0f` equality (true only for the exact 0x00000000/
 * 0x80000000 bit patterns), NOT COP1's exponent-field/denormal-
 * counts-as-zero test - confirmed a denormal divisor takes VDIV's
 * NORMAL path (real fdivs), unlike COP1's DIV.S. On a zero divisor,
 * VDIV produces a signed FLT_MAX whose sign is the XOR of both raw
 * operand sign bits (0/0 and x/0 share this one formula), the same
 * "xor sign bits, OR with FMAX" blend DIV.S established, but with NO
 * clamp anywhere (confirmed absent, matching every VU0 arithmetic
 * opcode JIT'd since Round 907 - VU0 floats never go through COP1's
 * fpu_double()/fpu_clamp32 machinery). VRSQRT's zero-divisor case
 * branches one level further on fsv: fsv!=0 clamps to that same
 * signed-FLT_MAX result, but fsv==0 (a genuine 0/sqrt(0)) clamps to
 * signed zero instead - and since sign_diff is already exactly 0 or
 * 0x80000000, "signed zero with that sign" is just sign_diff itself,
 * no extra OR needed. VSQRT has NO special case at all (sqrtf(|0|)=0
 * is already correct) - the simplest of the three. VRSQRT/VSQRT call
 * the real sqrtf() trampoline Round 905's SQRT.S/RSQRT.S established
 * (ADDR_EE_SQRTF - real PPC750/Gekko can't safely run fsqrts); VDIV
 * needs no such call. New host-native harness
 * r909_vu0_div_sqrt_rsqrt_verify.c extended the ppcsim simulator with
 * LR/CTR pseudo-registers and mfspr/mtspr/bcctrl decode - the first
 * harness in this project needing to intercept a real sqrtf() call
 * from JIT'd VU0 code (SQRT.S/RSQRT.S's own Round 905 harness was
 * deleted before this project's per-round cleanup convention was
 * applied as consistently as it is now). 17/17 checks passed under
 * -fsanitize=address,undefined, 0 leaks.
 *
 * Round 910 (task #895) update: JIT VU0 VIADD/VISUB/VIAND/VIOR (funct
 * 0x30/0x31/0x34/0x35 - plain scalar VI[fd]=VI[fs] op VI[ft] integer
 * ALU, no VF/lane/destmask involvement at all, unlike every CO-format
 * op above) + VMOVE(SPECIAL2 idx48)/VMR32(idx49) (VF lane copy/
 * rotate, joining the idx=16-23/29 unary/data-movement cluster VABS
 * already established in Round 908 - dest=FT, src=FS, fd field
 * unused). VI registers live in the same cop2_ctrl array VDIV/VCLIP's
 * Q/CLIP already use (COP2_CTRL_OFF); VI0 is hardwired to 0 exactly
 * like VF00, so VIADD/VISUB/VIAND/VIOR's direct store needs the same
 * `if (fd != 0)` guard every VF-writing op here carries - applied
 * correctly from the start this round, learning Round 908's own
 * lesson rather than repeating its bug. VIADD/VISUB mask their result
 * to 16 bits (real VI registers are 16-bit); VIAND/VIOR need no mask
 * since AND/OR of already-16-bit-clean operands stays clean. VMR32
 * reads all 4 source lanes into scratch registers before any
 * destination write - required correctness for a self-rotate
 * (ft==fs), not just style, verified with a dedicated test case.
 * 15/15 checks passed under -fsanitize=address,undefined, 0 leaks.
 *
 * Round 911 (task #896) update: JIT VU0 VITOF0/4/12/15 (SPECIAL2
 * idx16-19)/VFTOI0/4/12/15 (idx20-23) - the fixed-point<->float
 * conversion members of the unary/data-movement cluster (idx 16-23/29/
 * 48/49), closing it out (VABS/VMOVE/VMR32 already JIT'd in Rounds
 * 908/910). dest=FT, src=FS, fd unused, guarded by ft==0 - same
 * convention as the rest of the cluster; destmask still selects which
 * lanes participate (confirmed against ee_core.c's real per-lane loop,
 * unlike VABS/VCLIP's special-cased few). offset_n (0/4/12/15,
 * selected by the low 2 bits of funct) bakes a power-of-two scale
 * directly into a float's raw exponent bits, applied AFTER the
 * int->float conversion for VITOF but BEFORE the float->int conversion
 * for VFTOI - ported bit-exact from ee_core.c's real intToFloat<Offset>/
 * floatToInt<Offset> templates. VITOF's int->float step reuses the
 * EXACT SAME ee_jit_cvt_s_w_helper() trampoline CVT.S.W (Round 906b)
 * established (PPC750/Gekko has no int->float FPU instruction at all -
 * see ADDR_EE_CVT_S_W's own comment), needing no stack-spill across the
 * call since nothing but the trampoline's own f1 result is needed
 * afterward. VFTOI's float->int step reuses fctiwz (Round 906b's
 * CVT.W.S) plus an exponent-threshold saturation blend in the same
 * shape - but NOT byte-for-byte identical: VFTOI's real threshold test
 * is `>=0x4F000000` (confirmed by direct re-read of ee_core.c's actual
 * case body), a different constant AND a different comparison operator
 * than CVT.W.S's `>0x4E800000`, requiring an extra nor() flip on the
 * subfc/subfe borrow-to-mask idiom to get the right polarity for a
 * `>=` test instead of CVT.W.S's `>`.
 *
 * task #896's VCALLMS/VCALLMSR half was investigated and found
 * out-of-scope this round: ee_core.c's own vu0_exec_micro()/
 * vu0_exec_micro_continue() (VU0 micro-mode execution) exist as
 * infrastructure but are never called from anywhere in the real COP2
 * opcode dispatch (confirmed by grep - zero call sites outside their
 * own definitions and each other's comments) - VCALLMS/VCALLMSR use a
 * COP2 encoding form this project's interpreter has never wired up at
 * all (distinct from both the rs<0x10 MFC2/CFC2/etc. dispatch and the
 * rs>=0x10 CO-format vector dispatch this file already JITs). Per this
 * project's own discipline (JIT mirrors a real, already-verified
 * interpreter case body - never invents behavior the interpreter
 * itself doesn't have), there is nothing correct to JIT here yet;
 * wiring VCALLMS/VCALLMSR would require interpreter-side work first,
 * out of scope for a JIT-only round. Deferred off the JIT closure path
 * for task #885 entirely (not just to a later round) until the
 * interpreter gains real VU0 micro-mode dispatch.
 *
 * New host-native harness r911_vu0_vitof_vftoi_verify.c extends
 * r904's ppcsim base with fctiwz/stfd (opcode63/54, same forms Round
 * 906b's now-deleted harness established) and mflr/mtlr/mtctr/a
 * literal bctrl match (0x4E800421, checked directly alongside the
 * existing blr literal - avoiding Round 909's opcode-19-dispatch
 * collision entirely by not using a range dispatch for bctrl at all).
 * 13/13 checks passed under -fsanitize=address,undefined, 0 leaks.
 *
 * Round 912 (task #897) update: JIT the COP2 rs<0x10 scalar transfer
 * family - MFC2(0x00)/QMFC2(0x01)/CFC2(0x02)/MTC2(0x04)/QMTC2(0x05)/
 * CTC2(0x06) - re-verified against ee_core.c's real case body (lines
 * 8221-8258). MFC2 and CFC2 are byte-for-byte identical interpreter
 * bodies (`if (rt) GPR(rt) = sext32(vu0_vi_read(st, rd));`); so are
 * MTC2 and CTC2 (`vu0_vi_write(st, rd, rt32);` - CTC2's real FBRST
 * bit semantics are commented in ee_core.c but not modeled beyond
 * plain storage there, so the JIT mirrors exactly that, nothing more).
 * MFC2/CFC2 read COP2_CTRL_OFF(rd) (or emit a literal 0 when rd==0,
 * resolved at JIT-compile time since rd is a constant field of the
 * instruction) and sign-extend into the full 64-bit destination via
 * the same srawi-by-31 fill-word idiom LW/ADDIU/ADDU/SLL already use.
 * MTC2/CTC2 store REG_LO(rt) into COP2_CTRL_OFF(rd), guarded by the
 * usual compile-time `if (rd != 0)` (vu0_vi_write's own discard-on-
 * VI0). QMFC2/QMTC2 are 128-bit RAW BIT COPIES between GPR(rt) and
 * VF[rd] - explicitly no float conversion, per ee_core.c's own
 * comment - mapping VF.x/VF.z (low 32 halves) onto REG_LO/REG_LO1 and
 * VF.y/VF.w (high 32 halves) onto REG_HI/REG_HI1 (the ud0/ud1 split
 * Round 900's LQ/SQ established). QMFC2 needs no rd==0 special-case
 * on the read side - VF00's array slot is itself kept correctly
 * hardwired to (0,0,0,1.0) since writes to it are always discarded
 * (ee_core.c line 3802's reset-time store), so a direct read is
 * always safe, same as every other VF-reading op in this file. QMTC2
 * does need the usual compile-time `if (rd != 0)` write guard.
 *
 * No new PPC750 instruction forms were needed this round - lwz/stw/
 * li/srawi (all already established by LW/ADDIU/DIV.S and friends)
 * cover the entire scalar-transfer family. New host-native harness
 * r912_vu0_cfc2_ctc2_qmfc2_qmtc2_verify.c reuses r904's ppcsim base
 * completely unmodified (zero new opcode decode added) - 15/15 checks
 * passed under -fsanitize=address,undefined, 0 leaks.
 * Next: task #898, Round 913 - JIT the remaining VU0 opcodes to close
 * out task #885 (COP2/VU0 umbrella).
 *
 * Round 913 (task #898) update: JIT VMADD(funct=0x29)/VMSUB(funct=
 * 0x2D) and VIADDI(funct=0x32), re-verified against ee_core.c's real
 * case bodies (lines 8376-8408 for VMADD/VMSUB, lines 8957-8981 for
 * VIADDI). VMADD/VMSUB share the same SPECIAL1 row as VADD/VMUL/VMAX/
 * VSUB/VMINI/VOPMSUB (Rounds 907-908) but read a third operand from
 * the fixed VU0 macro-mode accumulator (VU0_ACC_OFF, established by
 * VOPMSUB - no register-index/reg==0 concept applies to ACC), writing
 * FD only (never back into ACC - that's the separate, still-
 * unimplemented VMADDA/VMSUBA family, matching ee_core.c's own scoped
 * gap). Computed as two separate float ops (fmuls then fadds/fsubs)
 * rather than a fused multiply-add, matching the plain C `acc +- a*b`
 * expression shape the interpreter evaluates. VIADDI is the odd one
 * out among the CO-format integer ops: dest=FT/src=FS/imm=FD-field
 * (reversed from VIADD/VISUB/VIAND/VIOR's dest=FD), with a real-
 * hardware sign-extension quirk ported verbatim from PCSX2's VUops.cpp
 * _vuIADDI (imm5&0x10 ? 0xFFF0 : 0 | imm5&0xF - a signed 4-bit
 * magnitude with a separate sign bit, not plain 5-bit two's-
 * complement) - resolved entirely at JIT-compile time since imm5/FD
 * is a constant instruction field, so a single `addi` with the
 * precomputed imm cast to int16_t reproduces the interpreter's exact
 * 32-bit sum with no extra load-immediate step. No new PPC750
 * instruction forms were needed - lfs/stfs/fmuls/fadds/fsubs/lwz/stw/
 * addi/rlwinm were all already established.
 *
 * New host-native harness r913_vu0_vmadd_vmsub_viaddi_verify.c reuses
 * r904's ppcsim base with zero new opcode decode added. 8/8 checks
 * passed under -fsanitize=address,undefined, 0 leaks.
 *
 * task #885 (COP2/VU0 umbrella) remains open after this round: the
 * broadcast row (funct 0x00-0x1F - the Q/I-scalar-broadcast forms of
 * VADD/VSUB/VMADD/VMSUB/VMAX/VMINI/VMUL, plus VMADDA/VMSUBA/VMULA's
 * own broadcast siblings under the SPECIAL2 idx-based dispatch) is a
 * substantially larger, more complex feature (8 op_kinds x 4
 * broadcast-source selectors) deliberately deferred to its own round
 * rather than folded in here, consistent with this project's pattern
 * of splitting large features across rounds instead of rushing scope.
 * Next: task #898 continues in a follow-up round scoping the
 * broadcast row specifically.
 *
 * Round 913b (task #898 continuation) update: the full broadcast row
 * (funct 0x00-0x1F) is now JIT'd - re-verified against ee_core.c's
 * real case body (lines 8308-8375), which is itself cross-checked
 * against PCSX2's own R5900OpcodeTables.cpp SPECIAL1 table's first 4
 * rows. `funct` alone determines bc_lane=funct&3 (which FT lane, or
 * which control register, is broadcast) and base_op=(funct>>2)&7
 * (0=ADD,1=SUB,2=MADD,3=MSUB,4=MAX,5=MINI,6=MUL,7=Q/I-row-with-op-
 * selected-by-bc_lane-instead: bc_lane 0=VMULq/Q, 1=VMAXi/I, 2=VMULi/I,
 * 3=VMINIi/I). Because funct is a compile-time-constant field of this
 * instruction's own encoding, op_kind and the broadcast source
 * (VF[ft][bc_lane] vs cop2_ctrl[21 or 22]) are BOTH resolved entirely
 * at JIT-compile time - unlike the interpreter's runtime op_kind
 * switch, this codegen emits only the exact instruction sequence the
 * resolved op_kind needs, zero runtime branching, consistent with
 * every other CO-format op in this file. The broadcast scalar is
 * loaded once into f1 before the per-lane loop since it's lane-
 * invariant, matching the interpreter's own single `b` computation
 * outside its loop. VMADD/VMSUB read the same fixed VU0_ACC_OFF
 * accumulator VOPMSUB/Round-913's non-broadcast VMADD/VMSUB already
 * established (no reg==0 concept for ACC, write to FD only). VMAX/
 * VMINI (both the x/y/z/w and Q/I-row VMAXi/VMINIi forms) reuse the
 * exact fsubs+fsel idiom Round 908's non-broadcast VMAX/VMINI already
 * established (real hardware ternary comparison, not the sign-
 * magnitude bit trick COP1.S's MAX.S/MIN.S uses). The Q/I control
 * registers (cop2_ctrl[22]/cop2_ctrl[21]) already store raw float bit
 * patterns (established by VDIV/VRSQRT, Round 909), so a direct `lfs`
 * from COP2_CTRL_OFF needs no int->float conversion. No new PPC750
 * instruction forms were needed beyond fsel, which Round 908 already
 * introduced and this round's harness (unlike Round 912/913's, which
 * didn't need it) had to add to its own ppcsim decode table.
 *
 * New host-native harness r913b_vu0_broadcast_row_verify.c extends
 * r904's ppcsim base with exactly one new decode addition (fsel,
 * opcode 63, xo5=23). 24/24 checks passed under
 * -fsanitize=address,undefined, 0 leaks - covering all 7 non-Q/I
 * base_ops, all 4 Q/I-row forms, partial destmask, fd==0 write-
 * discard, and an 11-case cross-check against an independent
 * reference model transcribed directly from ee_core.c.
 *
 * Status: task #898 (Rounds 913+913b) CLOSED. task #885 (COP2/VU0
 * umbrella) is now effectively closed too: every VU0 macro-mode
 * opcode with real, evidenced interpreter semantics is JIT'd. The one
 * remaining documented gap, VCALLMS/VCALLMSR, was correctly
 * classified by Round 911 as an interpreter-side micro-mode-dispatch
 * gap (not a JIT gap) and stays deferred pending that interpreter
 * work - noted here so the gap isn't lost, not treated as blocking.
 * Next: task #886 (Round 914+: JIT the MMI opcode family).
 *
 * Round 914 (task #899, task #886 start) update: JIT'd MMI0's add/sub
 * SIMD family - PADDW/PSUBW (4x32-bit lanes), PADDH/PSUBH (8x16-bit
 * lanes), PADDB/PSUBB (16x8-bit lanes). MMI is op=0x1C with funct=0x08
 * itself a meta-opcode ("MMI0") whose real sub-dispatch key is the `sa`
 * field (bits 10-6, the shift-amount position in a normal R-type MIPS
 * instruction) - verified against ee_core.c's real nested `case 0x08:
 * switch (sa)` dispatch (~line 9276-9283) before writing any codegen.
 * Every case body is byte-for-byte `set_lane_X(&gpr[rd], n, lane_X(
 * gpr[rs],n) +/- lane_X(gpr[rt],n))`, guarded by the usual compile-
 * time-resolved `if (rd)`, with plain unsigned wraparound (no
 * saturation) - ee_core.c's own lane_w/lane_h/lane_b/set_lane_*
 * helpers already do this via C's ordinary unsigned arithmetic on
 * uint32_t/uint16_t/uint8_t.
 *
 * The real work this round was byte-offset derivation, not arithmetic:
 * ee_core.c's lane accessors do value-level bit-shifts on the plain
 * uint64_t ud0/ud1 fields, which is host-endianness-independent AT THE
 * VALUE LEVEL, but this dynarec needs the exact BYTE ADDRESS a real
 * big-endian PPC750/Broadway memory access at that offset would hit -
 * and the mapping is NOT simply "lane order == address order" (see
 * the new mmi_w_off()/mmi_h_off()/mmi_b_off() helpers' own long
 * derivation comment for the full worked-out byte offsets). Every
 * offset was hand-derived from REG_HI/REG_LO/REG_HI1/REG_LO1's already-
 * established meaning, not guessed from a pattern.
 *
 * Two new PPC750 instruction forms: enc_lhz/enc_sth (opcodes 40/44,
 * the halfword members of the existing lwz/lbz/stw/stb D-form load/
 * store family) - lhz zero-extends on load and sth truncates on store,
 * so set_lane_h's own `(uint16_t)(...)` truncating cast needs no
 * separate masking instruction. lwz/stw/lbz/stb/add/subf were all
 * already established; subf's `rT=rB-rA` calling convention (from
 * SUBU, Round 887) computes rs-rt the same way SUBU's own codegen
 * already does.
 *
 * New host-native harness r914_mmi0_paddsub_verify.c extends r904's
 * ppcsim base with THREE new decode additions (lhz/sth, plus opcode
 * 31's plain `add`/xo266 and `subf`/xo40 forms - the first VU0/MMI-arc
 * harness needing integer add/subf at all, since every prior round in
 * this arc only ever needed float ops or bitwise/shift ops). 8/8
 * checks passed under -fsanitize=address,undefined, 0 leaks: PADDW/
 * PSUBW/PADDH/PSUBH/PADDB/PSUBB each cross-checked against an
 * independent reference model using ee_core.c's own lane_w/lane_h/
 * lane_b/set_lane_* bit-shift formulas (not this dynarec's own offset
 * logic), plus rd==0 write-discard and an rs==rt self-add case
 * specifically to catch any lane-aliasing bug in the offset
 * derivation.
 *
 * Status: task #899 (Round 914) CLOSED - MMI0's add/sub SIMD family is
 * JIT'd. task #886 (COP2/VU0's MMI sibling umbrella) is now open and
 * in progress: MMI0 still has PCGTW/PMAXW/PCGTH/PMAXH/PCGTB/PEXTLW/
 * PPACW/etc. beyond this round's 6 opcodes, and MMI1/MMI2/MMI3 (the
 * other three funct=0x09/0x28/0x29 meta-groups) plus the top-level
 * MADD/MADDU/PLZCW/MFHI1/MTHI1/MFLO1/MTLO1/MULT1/MULTU1/etc. opcodes
 * remain entirely unaddressed. Next: task #900 (Round 915: JIT MMI
 * multiply-divide family).
 *
 * Round 915 (task #900) update: JIT'd MMI2's multiply/divide family -
 * PMULTW (sa=0x0C), PDIVW (sa=0x0D), PMULTH (sa=0x1C), PDIVBW
 * (sa=0x1D) - opcode 0x1C funct 0x09. Naming correction found this
 * round (superseding a stale prior-session note): per real R5900 EE
 * Core hardware AND this project's own ee_core.c interpreter source
 * (re-read directly, not from memory), funct 0x09 is MMI2 and funct
 * 0x28 is MMI1 - the reverse of what an earlier session summary
 * claimed. Round 914's block comments already correctly said "MMI0"
 * for funct 0x08 (unaffected), but any future MMI1-family round
 * should target funct 0x28, not 0x09.
 *
 * Unlike every opcode JIT'd so far (including Round 914's inline
 * lwz/add/stb MMI0 codegen), these four opcodes are dispatched via a
 * "whole-operation" C-function-call trampoline: the real
 * ee_jit_helper_pmultw/pdivw/pmulth/pdivbw() functions (new this
 * round, defined in ee_core.c right before ee_step(), where
 * ee_state_t/lane_w/lane_h/set_lane_w/sext32 are already visible) do
 * ALL the arithmetic themselves - each is a byte-for-byte port of
 * ee_core.c's own interpreter case body for the same opcode. The
 * JIT-generated PPC code only does li r4=rs/r5=rt/r6=rd (compile-time
 * constants - single-instruction-granularity JIT) then bctrl through
 * a new ADDR_EE_JIT_PMULTW/PDIVW/PMULTH/PDIVBW sentinel quartet
 * (0x10B-0x10E on host builds, real function addresses under GEKKO),
 * following the SW-style simple frame (only r14/LR saved across the
 * call - no r15/saved-ctx needed, since no result flows back into any
 * PPC register: the helper writes gpr[rd]/HI/LO directly through the
 * ctx pointer it's given). This trampoline choice was deliberate, not
 * a shortcut: PMULTW/PDIVW/PMULTH/PDIVBW's real 64-bit HI:LO-pipe-pair
 * arithmetic (including MIPS's div-by-zero sign-of-dividend
 * convention, the INT32_MIN/-1 overflow special case, and PDIVBW's
 * single-halfword-divisor-broadcast-across-four-lanes quirk) would
 * take many dozens of individual PPC750 instructions to hand-translate
 * bit-exactly, at real risk of silently drifting from the
 * interpreter's own behavior - the trampoline call guarantees
 * byte-for-byte agreement instead, since JIT and interpreter now
 * literally share the same C arithmetic. New host-native harness
 * r915_mmi2_muldiv_verify.c extends r893's bctrl-dispatch-simulation
 * base with the four new sentinel targets; its own test doubles are
 * an INDEPENDENTLY transcribed port of the same real case bodies (not
 * a call into the real ee_jit_helper_* functions), preserving this
 * project's cross-check-independence discipline. 35/35 checks passed
 * on the first run; full 10-harness regression suite unchanged; clean
 * devkitPPC Wii cross-build (0 warnings), elf 3,310,500/dol 551,648
 * (+13,304/+1,664 over Round 914). Status: task #900 (Round 915)
 * CLOSED. Next: task #901 (Round 916: JIT MMI2's logical family -
 * PAND/POR/PXOR/PNOR).
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
