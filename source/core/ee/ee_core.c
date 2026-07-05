/*
 * ee_core.c - R5900 (Emotion Engine) interpreter
 *
 * Instruction semantics (sign-extension rules, HI/LO handling, MMI
 * lane packing, etc.) are ported from PCSX2's own interpreter -
 * pcsx2/R5900OpcodeImpl.cpp and pcsx2/MMI.cpp - not reinvented from
 * the MIPS manual, so behavior matches real PCSX2 for the opcodes
 * covered here. Those files are GPL-3.0 (PCSX2 project), so this
 * file - and this project as a whole - is GPL-3.0. See /COPYING.GPLv3.
 *
 * Coverage, roughly in order of how much of a real BIOS boot they
 * unblock:
 *   - Full MIPS III integer core: ALU imm+reg, all shift variants
 *     incl. 64-bit D-forms, MULT/DIV, HI/LO moves, REGIMM branches,
 *     J/JAL/JR/JALR, byte/half/word/double load+store, and unaligned
 *     LWL/LWR/SWL/SWR.
 *   - COP0: MFC0/MTC0 (Status/Config/generic registers only), the
 *     "CO"-format instructions ERET, EI, DI (ported from PCSX2's
 *     COP0.cpp), RFE (the older MIPS I exception-return instruction -
 *     not in PCSX2's own EE opcode table since real EE/R5900 doesn't
 *     need it, but real BIOS PS1-backward-compatibility-mode boot code
 *     does execute it, so it's implemented here with RFE's standard
 *     MIPS I semantics), a real 48-entry TLB (TLBR/TLBWI/TLBWR/TLBP,
 *     ported from PCSX2's COP0.cpp, plus real KUSEG address
 *     translation wired into ee_mem_ptr() - see docs/STATUS.md's
 *     "round 6"), and real exception RAISING/delivery
 *     (ee_raise_exception()/ee_raise_tlb_exception(), ported from
 *     PCSX2's cpuException()/cpuTlbMiss() in R5900.cpp): Cause/EPC/
 *     Status.EXL are updated and pc/next_pc vectored to the correct
 *     BEV-dependent handler address on a KUSEG TLB miss, for both
 *     instruction fetch and load/store, with correct Cause.BD/EPC
 *     bookkeeping when the fault lands in a branch-delay slot. Still
 *     no BC0 branches, and the only ExcCodes actually raised anywhere
 *     are the two TLB-miss ones (TLBL/TLBS) - no general/interrupt/
 *     SYSCALL exception delivery through this same path yet (SYSCALL
 *     has its own hand-written trap elsewhere, see
 *     InstallExceptionHandlers).
 *   - CACHE, SYNC, PREF: accepted as no-ops (real BIOS init code issues
 *     these constantly for cache management we don't model).
 *   - A meaningful subset (~67 of ~90) of MMI (SIMD) opcodes: the
 *     add/sub/logic/copy/extend/pack family across byte/half/word
 *     lanes, the "pipeline 1" MULT1/DIV1/MFHI1/MFLO1 family, the
 *     compare/max/min/abs family (PCGTW/H/B, PMAXW/H, PCEQW/H/B,
 *     PMINW/H, PABSW/H, PADSBH), the saturated add/sub family
 *     (PADDSW/H/B, PSUBSW/H/B - clamp to the lane width's min/max
 *     instead of wrapping), PEXT5/PPAC5 (GS 5551-pixel-format
 *     unpack/pack, completing MMI0's sub-table entirely), the
 *     MMI2/MMI3 permute/interleave family (PINTH/PINTEH, PEXEH/PEXCH,
 *     PEXEW/PEXCW, PREVH, PCPYH, PROT3W), and PSLLVW/PSRLVW
 *     (variable logical shift of a word pair, each lane shifted by
 *     its own per-lane shift amount).
 *   - LQ/SQ (128-bit load/store, primary opcodes 0x1E/0x1F) - ported
 *     from PCSX2's R5900OpcodeImpl.cpp. Address masked to 16-byte
 *     alignment (real hardware silently ignores the low 4 bits rather
 *     than faulting); LQ skips the read entirely when rt==$0 (matches
 *     PCSX2's interpreter exactly - unlike LW/LH/etc elsewhere in
 *     this file, which still perform a discarded read for its memory
 *     side effects). Added after real-BIOS testing (see
 *     docs/STATUS.md) showed the EE now running 53M+ real instructions
 *     before halting on exactly this gap - the first real evidence
 *     that boot code needs 128-bit memory access, presumably for
 *     VU0 data staging.
 *
 * Still NOT implemented (halts cleanly, does not crash):
 *   - The other ~23 MMI opcodes (QFSRV - needs the SA hardware
 *     register and MTSA/MTSAB/MTSAH to set it, none of which exist
 *     yet; PMADDW/H, PMSUBW/H, PMULTW/H, PDIVW/PDIVBW, PMULTUW/
 *     PDIVUW/PMADDUW - the remaining MMI2/MMI3 HI/LO-touching
 *     arithmetic, some with documented real-hardware rounding quirks
 *     in PCSX2's own source worth extra care when ported; PMFHL/PMTHL
 *     clamping variants)
 *   - COP2 (VU0 macro mode)
 *   - COP1 (FPU): core arithmetic (ADD/SUB/MUL/DIV/ABS/MOV/NEG.S,
 *     SQRT.S/RSQRT.S, MAX.S/MIN.S, CVT.W.S/CVT.S.W, C.EQ/LT/LE.S,
 *     MFC1/MTC1/CFC1/CTC1), BC1F/BC1T (branch on FP condition flag),
 *     and the full ACC (accumulator) family - ADDA.S/SUBA.S/MULA.S/
 *     MADD.S/MSUB.S/MADDA.S/MSUBA.S - are implemented, ported from
 *     pcsx2/FPU.cpp including the PS2's non-IEEE denormal/infinity
 *     handling (fpuDouble/checkOverflow/checkUnderflow), the
 *     MAX.S/MIN.S bit-level signed-int comparison quirk (fp_max/
 *     fp_min), and a real hardware/PCSX2 asymmetry in the ACC family
 *     worth remembering: MADD.S/MSUB.S run their intermediate
 *     fs*ft product through fpuDouble() a SECOND time before adding/
 *     subtracting it from ACC, but MADDA.S/MSUBA.S do NOT - ported
 *     exactly as PCSX2 has it, not "cleaned up" to be consistent.
 *     Still NOT implemented: BC1FL/BC1TL ("likely" branches - this
 *     project has no likely-branch infrastructure for ANY branch yet,
 *     integer or FP), and the FPU exception-cause control-register
 *     flags (O/U/I/D/SO/SU/SI/SD) - only the condition flag (C,
 *     needed for BC1) is modeled; nothing in this project raises FPU
 *     exceptions from the others yet, a documented, consistent
 *     simplification across every FPU op in this file.
 *   - General/SYSCALL exception delivery through the real
 *     ee_raise_exception() path (KUSEG TLB misses AND the Timer/
 *     Compare interrupt - round 9, see docs/STATUS.md - both use it
 *     now; SYSCALL still doesn't); a real SYSCALL handler table (the
 *     existing InstallExceptionHandlers trap is a separate, hand-
 *     written mechanism, not real exception vectoring). Timer
 *     (Count==Compare) is the only interrupt SOURCE modeled so far -
 *     no INTC/DMAC-driven interrupts (VBlank, DMA-complete, etc.)
 *     raise Cause's other Interrupt Pending bits yet.
 *   - The IOP has its own core (iop_core.c) and BIOS syscall trap
 *     (core/hw/iop_hle_bios.c) now - see docs/ROADMAP.md for scope
 *
 * A real BIOS will still halt here - see docs/STATUS.md for what's
 * actually required to get to a rendered splash screen (short
 * version: the GS/GIF/DMA/IOP/SIF stack, which dwarfs the CPU work).
 */

#include "core/ee/ee_core.h"
#include "core/hw/dma.h"
#include "core/hw/gs.h"
#include "core/hw/gif.h"
#include "core/hw/sif.h"
#include "core/hw/mch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>

#define EE_RAM_SIZE (32 * 1024 * 1024)

static ee_state_t g_state;

ee_state_t *ee_core_get_state(void) { return &g_state; }

/* --- memory access --- */

/* KUSEG (0x00000000-0x7FFFFFFF) is a MAPPED segment on real MIPS/R5900
 * hardware - unlike kseg0/kseg1, a KUSEG address is NOT simply
 * "physical = virtual & 0x1FFFFFFF"; it requires a real TLB entry
 * (written by TLBWI/TLBWR, see the COP0 CO-format dispatch above) to
 * translate. Found empirically: once TLBWI actually stored real
 * entries (see docs/STATUS.md's "round 5" COP0-TLB work) and boot
 * progressed further, real BIOS code set up a kernel stack pointer in
 * KUSEG (observed: $sp=0x70003eb0) that this project's old flat
 * "phys = addr & 0x1FFFFFFF" shortcut mismapped to a physical address
 * far past the end of RAM (silently doing nothing on every access
 * through it) - explaining a further, more subtle case of the same
 * "boot wanders through silently-dead memory" failure mode already
 * documented for the JALR investigation. Ported from PCSX2's own
 * VPN2()/PFN0()/PFN1()/Mask() logic in R5900.h's tlbs struct. Returns
 * 1 and fills *out_phys on a hit; returns 0 (TLB miss) otherwise - the
 * caller (ee_mem_ptr()) turns a miss into a real TLB Refill exception
 * (see ee_raise_exception()/ee_raise_tlb_exception() and their call
 * sites in the ee_mem_read* / ee_mem_write* functions below), matching
 * real hardware instead of the old "just read as zero" placeholder. */
static inline int ee_tlb_translate(ee_state_t *st, uint32_t vaddr, uint32_t *out_phys)
{
    uint32_t want_vpn2 = (vaddr >> 13) & 0x7FFFFu;
    uint32_t want_asid = st->cop0[10] & 0xFFu; /* current ASID = current EntryHi's ASID field */
    for (uint32_t i = 0; i < 48; i++) {
        uint32_t mask = (st->tlb[i].page_mask >> 13) & 0xFFFu;
        uint32_t entry_vpn2 = (st->tlb[i].entry_hi >> 13) & 0x7FFFFu;
        if ((entry_vpn2 & ~mask) != (want_vpn2 & ~mask))
            continue;
        int is_global = (st->tlb[i].entry_lo0 & 1u) && (st->tlb[i].entry_lo1 & 1u);
        uint32_t entry_asid = st->tlb[i].entry_hi & 0xFFu;
        if (!is_global && entry_asid != want_asid)
            continue;
        /* Even/odd page select: the bit just below the masked-off
         * range picks EntryLo0 (even, bit clear) or EntryLo1 (odd,
         * bit set). Page size in bytes is (mask+1) << 13 (PageMask's
         * "Mask" field is in units of the VPN2 field's own bit
         * position, i.e. bit 13 upward). */
        /* Even/odd select bit position: bit 13 for the common 4KB-page
         * case (mask=0), moving one bit higher each time the page size
         * doubles. mask is always a run of low 1-bits (0, 1, 3, 7...)
         * per the MIPS PageMask spec, so (mask+1) is a power of two -
         * this matches PCSX2's own VPN2()/PFN0()/PFN1() shift-by-Mask()
         * logic in R5900.h, just computed as a bit position instead of
         * a bitmask. */
        uint32_t page_select_bit = 13u;
        {
            uint32_t doubling = mask + 1u; /* 1,2,4,8,... for 4KB,16KB,64KB,... pages */
            while (doubling > 1u) { page_select_bit++; doubling >>= 1; }
        }
        uint32_t lo = (vaddr & (1u << page_select_bit)) ? st->tlb[i].entry_lo1 : st->tlb[i].entry_lo0;
        uint32_t pfn = (lo >> 6) & 0xFFFFFu;
        uint32_t phys_page = (pfn & ~mask) << 12;
        uint32_t offset_mask = (1u << page_select_bit) - 1u;
        *out_phys = phys_page | (vaddr & offset_mask);
        return 1;
    }
    return 0;
}

/* MIPS/R5900 exception codes (Cause register ExcCode field, bits
 * 2-6) - only the ones this project actually raises so far. Ported
 * from PCSX2's R5900.h EXC_CODE_* table (EXC_CODE(n) = n << 2). */
#define EE_EXC_CODE_INT   (0u << 2) /* Interrupt - raised by ee_check_timer_interrupt() below (round 9) */
#define EE_EXC_CODE_TLBL  (2u << 2) /* TLB miss, load or instruction fetch */
#define EE_EXC_CODE_TLBS  (3u << 2) /* TLB miss, store */

/* Raises a real R5900 exception: updates Cause/EPC/Status and vectors
 * pc/next_pc to the correct handler address. Ported from PCSX2's own
 * cpuException() in R5900.cpp - not fabricated. Only the two TLB-miss
 * ExcCodes above are actually triggered anywhere in this project right
 * now (see ee_raise_tlb_exception() below and its call sites in the
 * ee_mem_read* / ee_mem_write* functions), but this function itself
 * handles the general case correctly (vector offsets, BEV, nested-
 * exception override) for whenever a future change raises a different
 * ExcCode (COP unusable, overflow, address error, etc. are all still
 * unraised - see the coverage notes at the top of this file). */
static void ee_raise_exception(ee_state_t *st, uint32_t exc_code, uint32_t this_pc, int in_delay_slot)
{
    uint32_t offset;
    if (exc_code == EE_EXC_CODE_TLBL || exc_code == EE_EXC_CODE_TLBS)
        offset = 0x000u; /* TLB Refill vector */
    else if (exc_code == EE_EXC_CODE_INT)
        offset = 0x200u; /* Interrupt vector */
    else
        offset = 0x180u; /* General exception vector */

    /* Cause: clear ExcCode (bits 2-6) and BD (bit 31), then set both -
     * other bits (e.g. the CE coprocessor-number field) are left
     * alone, matching real hardware/PCSX2. */
    st->cop0[13] = (st->cop0[13] & ~(0x7Cu | 0x80000000u)) | (exc_code & 0x7Cu);

    if (!(st->cop0[12] & 0x2u)) { /* Status.EXL == 0: not already inside a handler */
        st->cop0[12] |= 0x2u; /* Status.EXL = 1 */
        if (in_delay_slot) {
            st->cop0[14] = this_pc - 4u; /* EPC = the branch itself, not the delay slot */
            st->cop0[13] |= 0x80000000u; /* Cause.BD = 1 */
        } else {
            st->cop0[14] = this_pc;
        }
    } else {
        /* Nested exception (already mid-handler): real hardware forces
         * the general-exception vector regardless of this ExcCode, and
         * does NOT touch EPC again (the original handler's return
         * address must survive). Ported from PCSX2's cpuException(). */
        offset = 0x180u;
    }

    /* Status.BEV (bit 22): boot-time / pre-vector-install exceptions
     * vector into the BIOS ROM directly (uncached, 0xBFC00200+); once
     * the BIOS clears BEV (after installing its own RAM-resident
     * handlers - see the InstallExceptionHandlers work elsewhere in
     * this file), exceptions vector into RAM (cached, 0x80000000+)
     * instead. */
    uint32_t base = (st->cop0[12] & 0x00400000u) ? 0xBFC00200u : 0x80000000u;
    st->pc = base + offset;
    st->next_pc = st->pc + 4u;
}

/* TLB-miss-specific wrapper: also records BadVAddr/Context/EntryHi the
 * way real hardware does, so a BIOS TLB-refill handler could look up
 * (or install) the right entry - ported from PCSX2's cpuTlbMiss()/
 * cpuTlbMissR()/cpuTlbMissW() in R5900.cpp. Guarded by
 * exc_raised_this_step so a single instruction that touches the same
 * missing TLB entry twice (e.g. SWL/SWR's read-then-write of the same
 * address) only actually raises one exception - see the field's
 * comment in ee_core.h. */
static void ee_raise_tlb_exception(ee_state_t *st, int is_store, uint32_t vaddr, uint32_t this_pc, int in_delay_slot)
{
    if (st->exc_raised_this_step)
        return;
    st->exc_raised_this_step = 1;

    st->cop0[8]  = vaddr; /* BadVAddr */
    st->cop0[4]  = (st->cop0[4] & 0xFF80000Fu) | ((vaddr >> 9) & 0x007FFFF0u); /* Context */
    st->cop0[10] = (vaddr & 0xFFFFE000u) | (st->cop0[10] & 0x1FFFu); /* EntryHi */

    ee_raise_exception(st, is_store ? EE_EXC_CODE_TLBS : EE_EXC_CODE_TLBL, this_pc, in_delay_slot);
}

#define EE_CAUSE_IP7  0x00008000u /* Cause register: latched timer-interrupt pending bit */
#define EE_STATUS_IM7 0x00008000u /* Status register: per-line mask for the same IP7 line (same bit position, different register - real MIPS layout, not a typo) */

/* Latches Cause.IP7 the instant Count (cop0[9]) reaches Compare
 * (cop0[11]) - called unconditionally, every single instruction,
 * regardless of delay-slot/branch_pending state (see the big comment
 * on ee_check_timer_interrupt() below for why latching and actually
 * TAKING the interrupt have to be two separate steps). Ported from
 * real, documented MIPS Count/Compare semantics (not fabricated):
 * once posted, the pending bit is sticky - it stays set regardless of
 * Count's value afterward - and is only ever cleared by software
 * explicitly writing a new value to Compare (see the MTC0 case for
 * register 11), which is the real, standard way a MIPS interrupt
 * handler acknowledges/re-arms this specific interrupt for the next
 * tick. This project advances Count by exactly 1 per instruction (see
 * the increment in ee_step()'s epilogue just above the call site), so
 * the match is always hit on the nose for exactly one instruction -
 * no risk of stepping clean over it the way PCSX2's own coarser,
 * lazily-advanced Count has to guard against with its "Count<Compare+
 * 1000" window in _cpuTestTIMR(). */
static void ee_latch_timer_interrupt(ee_state_t *st)
{
    /* >= rather than == : verified necessary, not just defensive,
     * against a live real SCPH-10000 BIOS trace ("round 9" follow-up
     * verification, see docs/STATUS.md). The real early-boot code at
     * pc=0xBFC0081C does "MTC0 $0, Count" (resetting Count to 0), then
     * TWO instructions later at pc=0xBFC00824 does "MTC0 $26, Compare"
     * with Compare=1. Because this project's Count increments once
     * per instruction in ee_step()'s epilogue - including the epilogue
     * of the very instruction that just reset it to 0 - Count is
     * already 3 by the time the Compare=1 write itself completes (0->1
     * on the reset instruction's own epilogue, 1->2 on the next
     * instruction, 2->3 on the Compare-write instruction's own
     * epilogue) - the value 1 was already passed one full instruction
     * before Compare even became 1. An exact-equality check would
     * silently miss this real, common "reset Count then arm a small
     * Compare shortly after" pattern forever (Count only grows from
     * here, never revisiting 1 until a full 32-bit wraparound). Real
     * PCSX2 has this same fundamental issue even worse (its Count
     * advances in large, lazily-applied jumps), which is exactly why
     * its own _cpuTestTIMR() uses a Count>=Compare window rather than
     * exact equality too (see that function's comment for the "<
     * Compare+1000" upper bound it additionally needs and this project
     * doesn't - once latched here, IP7 stays latched regardless of
     * Count's value, so no upper bound is needed, only the lower
     * bound this check itself is). */
    if (st->cop0[9] >= st->cop0[11])
        st->cop0[13] |= EE_CAUSE_IP7;
}

/* Actually takes (vectors) the latched timer interrupt as a real MIPS
 * Interrupt exception (ExcCode 0/Int - the EE_EXC_CODE_INT vector
 * this file already had defined for completeness but never raised
 * until this round). Gating is ported directly from PCSX2's own two
 * checks combined (R5900.cpp):
 *   - cpuTestTIMRInts(): (Status.val & 0x10007) == 0x10001, i.e.
 *     IE=1, EIE=1, EXL=0, ERL=0.
 *   - _cpuTestTIMR(): Status.val & 0x8000 (Status.IM7, the per-line
 *     mask for this same IP7 interrupt line) must also be set.
 *
 * Real hardware won't (can't) service an interrupt in between a
 * branch/jump and its delay slot - there's no pipeline checkpoint
 * there. This project models the same thing by only calling this
 * function when st->branch_pending is 0 (see the call site in
 * ee_step()'s epilogue), i.e. only at instruction boundaries where
 * the NEXT instruction is not itself a delay slot - so this function
 * never needs to consider in_delay_slot/BD itself; EPC always simply
 * points at the next, not-yet-executed instruction (this_pc ==
 * st->pc at the call site). Crucially, this deferral is safe *only*
 * because ee_latch_timer_interrupt() above already ran unconditionally
 * on every instruction including ones inside a branch/delay-slot pair
 * - the pending bit survives however long it takes to reach a safe
 * boundary, instead of the match being missed if it happened to land
 * exactly on a branch instruction's own step. */
static void ee_check_timer_interrupt(ee_state_t *st, uint32_t this_pc)
{
    const uint32_t IE  = 0x00000001u;
    const uint32_t EXL = 0x00000002u;
    const uint32_t ERL = 0x00000004u;
    const uint32_t EIE = 0x00010000u;

    if (st->exc_raised_this_step)
        return; /* a memory/TLB exception already vectored pc this step */
    if (!(st->cop0[13] & EE_CAUSE_IP7))
        return; /* no timer interrupt latched/pending */
    if ((st->cop0[12] & (IE | EXL | ERL | EIE)) != (IE | EIE))
        return; /* IE=0 and/or EIE=0, or already inside a handler (EXL/ERL) */
    if (!(st->cop0[12] & EE_STATUS_IM7))
        return; /* this specific interrupt line (IM7) is masked */

    st->exc_raised_this_step = 1;
    ee_raise_exception(st, EE_EXC_CODE_INT, this_pc, 0);
}

static inline uint8_t *ee_mem_ptr(ee_state_t *st, uint32_t addr, uint32_t size)
{
    /* R5900 Scratchpad RAM (SPR): a real, dedicated 16KB on-chip buffer
     * hardwired to the fixed KUSEG range 0x70000000-0x70003FFF. Real
     * hardware bypasses the TLB *entirely* for this fixed window - it
     * is not ordinary mapped memory, regardless of what (if anything)
     * a software TLB entry says about it. Found via a live PCSX2 trace
     * plus confirmation in PCSX2's own source (pcsx2/Memory.cpp's
     * "0x70000000-0x70003fff scratch pad" comment, pcsx2/MemoryTypes.h's
     * 16KB Ps2MemSize::Scratch, pcsx2/COP0.cpp's isSPR()-gated direct-
     * buffer mapping in MapTLB()) - see docs/STATUS.md's "round 8".
     * Must be checked BEFORE the KUSEG TLB path below: the real BIOS's
     * kernel stack pointer lands in the upper half of this window
     * (0x70002000-0x70003FFF), which round 7's TLB implementation had
     * no way to resolve (no TLB entry covers it, and none should ever
     * be needed here), producing an unresolvable TLB Refill exception
     * loop. */
    if (addr >= 0x70000000u && addr < 0x70004000u) {
        uint32_t off = addr - 0x70000000u;
        if (off + size <= sizeof(st->scratch))
            return st->scratch + off;
        return NULL; /* out-of-bounds within the 16KB window - not a TLB matter */
    }

    uint32_t phys;
    if (addr < 0x80000000u) {
        /* KUSEG - needs real TLB translation, see ee_tlb_translate(). */
        if (!ee_tlb_translate(st, addr, &phys)) {
            st->mem_tlb_miss = 1; /* real TLB Refill exception territory - see callers below */
            return NULL;
        }
    } else {
        /* Mask to the physical address FIRST, then decide ROM-vs-RAM.
         * Real MIPS kseg0/kseg1 both decode to the same physical
         * address space directly (segment bits only affect caching,
         * not the physical target) - so the BIOS ROM (physical base
         * 0x1FC00000) is reachable via its kseg1 uncached mirror
         * (0xBFC00000-0xC0000000, the reset vector's own segment) AND
         * its kseg0 cached mirror (0x9FC00000-0xA0000000). This
         * project originally only special-cased the kseg1 form
         * (checking the raw virtual address >= 0xBFC00000 before
         * masking), which silently treated any kseg0 ROM-mirror access
         * as a RAM access with a physical offset far past the end of
         * RAM (returning NULL / a decoded-as-NOP 0) instead of the
         * real ROM byte. Found via the COP0 PRId fix (see
         * docs/STATUS.md's "round 5"): once boot took the correct
         * path, it jumped through pc=0x9FC4xxxx (kseg0 ROM) almost
         * immediately. */
        phys = addr & 0x1FFFFFFFu;
    }
    st->mem_tlb_miss = 0; /* reached past the TLB-miss check above (translated OK, or kseg0/1) -
                            * any NULL returned below is a bounds/backing-store miss, not a TLB
                            * fault, and should keep failing silently (reads-as-zero/no-op) as before */
    if (phys >= 0x1FC00000u) {
        uint32_t off = phys - 0x1FC00000u;
        if (st->bios && off + size <= st->bios->size)
            return st->bios->data + off;
        return NULL;
    }
    if (phys + size <= st->ram_size)
        return st->ram + phys;
    return NULL;
}

/* PS2 (EE + IOP) memory is little-endian; the Wii's PowerPC 750 is
 * big-endian. A plain memcpy() here would silently byte-swap every
 * instruction fetch and load/store, so all multi-byte accesses are
 * composed/decomposed byte-by-byte in explicit little-endian order,
 * independent of host/target endianness. */

/* Shared by every ee_mem_read* / ee_mem_write* below: if the just-failed
 * ee_mem_ptr() call was specifically a KUSEG TLB miss (not a harmless
 * kseg0/1 backing-store gap), raise the real exception instead of
 * silently reading-as-zero/no-oping. Uses the transient exc_this_pc/
 * exc_in_delay_slot context ee_step() sets once per instruction (see
 * ee_core.h) - so calling these functions OUTSIDE of an in-progress
 * ee_step() (e.g. test setup code poking memory directly) never raises
 * a spurious exception, since mem_tlb_miss will simply be 0 for any
 * kseg0/1 address, which is all such direct calls use in this project
 * (see tests/README.md's note on test_ee_unaligned.c). */
static inline void ee_mem_check_tlb_fault(ee_state_t *st, uint32_t addr, int is_store)
{
    if (st->mem_tlb_miss)
        ee_raise_tlb_exception(st, is_store, addr, st->exc_this_pc, st->exc_in_delay_slot);
}

/* Real hardware/game code always accesses the 0x10000000-0x1FFFFFFF
 * hardware-register window through its KSEG1 (uncached, 0xB0000000+
 * phys) or occasionally KSEG0 (cached, 0x90000000+phys) mirrors, never
 * through the bare physical value as a virtual address - exactly the
 * same segment-aliasing ee_mem_ptr() already applies for RAM/ROM
 * below. The dma_mmio_.../sif_mmio_.../mch_mmio_... dispatch checks below
 * used to compare against the raw, unmasked virtual address, which
 * only ever matched a KUSEG-style literal (as this project's own
 * pre-existing tests happen to construct, e.g. test_ee_dma_bus.c's
 * "LUI r2,0x1000" address) and NEVER matched real KSEG1/KSEG0 hardware
 * accesses - meaning this hardware-register wiring was silently dead
 * for any real BIOS/game-issued load or store. Found via round 11's
 * live-trace investigation (see docs/STATUS.md) while wiring up
 * MCH_RICM/MCH_DRD: the exact same bug applied to DMA/SIF too. Fixed
 * by masking KSEG0/1 addresses down to their physical form before the
 * mmio dispatch checks, same as ee_mem_ptr() does - KUSEG addresses
 * (< 0x80000000) are passed through unchanged, which keeps the
 * existing KUSEG-literal tests passing unmodified. */
static inline uint32_t ee_hw_mmio_addr(uint32_t addr)
{
    return (addr >= 0x80000000u) ? (addr & 0x1FFFFFFFu) : addr;
}

uint8_t ee_mem_read8(ee_state_t *st, uint32_t addr)
{
    uint8_t *p = ee_mem_ptr(st, addr, 1);
    if (p) return *p;
    ee_mem_check_tlb_fault(st, addr, 0);
    return 0;
}

uint16_t ee_mem_read16(ee_state_t *st, uint32_t addr)
{
    uint8_t *p = ee_mem_ptr(st, addr, 2);
    if (!p) { ee_mem_check_tlb_fault(st, addr, 0); return 0; }
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t ee_mem_read32(ee_state_t *st, uint32_t addr)
{
    /* Hardware register window (0x10000000-0x1000FFFF): DMA controller,
     * SIF mailbox registers, and friends. Other addresses in this
     * window (timers, INTC, SIO, GIF/VIF/IPU control regs) still fall
     * through to the silent-no-op RAM/BIOS path below, which returns
     * 0. See docs/ROADMAP.md. */
    uint32_t hw_val;
    uint32_t hw_addr = ee_hw_mmio_addr(addr);
    if (dma_mmio_read32(hw_addr, &hw_val))
        return hw_val;
    if (sif_mmio_read32(hw_addr, &hw_val))
        return hw_val;
    if (mch_mmio_read32(hw_addr, &hw_val))
        return hw_val;

    uint8_t *p = ee_mem_ptr(st, addr, 4);
    if (!p) { ee_mem_check_tlb_fault(st, addr, 0); return 0; }
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t ee_mem_read64(ee_state_t *st, uint32_t addr)
{
    uint64_t gs_val;
    if (gs_mmio_read64(addr, &gs_val))
        return gs_val;

    uint8_t *p = ee_mem_ptr(st, addr, 8);
    if (!p) { ee_mem_check_tlb_fault(st, addr, 0); return 0; }
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

void ee_mem_write8(ee_state_t *st, uint32_t addr, uint8_t val)
{
    uint8_t *p = ee_mem_ptr(st, addr, 1);
    if (p) { *p = val; return; }
    ee_mem_check_tlb_fault(st, addr, 1);
}

void ee_mem_write16(ee_state_t *st, uint32_t addr, uint16_t val)
{
    uint8_t *p = ee_mem_ptr(st, addr, 2);
    if (!p) { ee_mem_check_tlb_fault(st, addr, 1); return; }
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
}

void ee_mem_write32(ee_state_t *st, uint32_t addr, uint32_t val)
{
    uint32_t hw_addr_w = ee_hw_mmio_addr(addr);
    if (dma_mmio_write32(hw_addr_w, val))
        return;
    if (sif_mmio_write32(hw_addr_w, val))
        return;
    if (mch_mmio_write32(hw_addr_w, val))
        return;

    uint8_t *p = ee_mem_ptr(st, addr, 4);
    if (!p) { ee_mem_check_tlb_fault(st, addr, 1); return; }
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
    p[2] = (uint8_t)((val >> 16) & 0xFF);
    p[3] = (uint8_t)((val >> 24) & 0xFF);
}

void ee_mem_write64(ee_state_t *st, uint32_t addr, uint64_t val)
{
    if (gs_mmio_write64(addr, val))
        return;

    uint8_t *p = ee_mem_ptr(st, addr, 8);
    if (!p) { ee_mem_check_tlb_fault(st, addr, 1); return; }
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((val >> (8 * i)) & 0xFF);
}

int ee_core_init(const bios_image_t *bios)
{
    memset(&g_state, 0, sizeof(g_state));

    dma_init(); /* EE DMA controller register block - see core/hw/dma.h */
    gs_init();  /* GS privileged register block - see core/hw/gs.h */
    sif_init(); /* EE-side SIF/SBUS mailbox registers - see core/hw/sif.h */
    mch_init(); /* EE-side MCH_RICM/MCH_DRD RDRAM auto-init registers - see core/hw/mch.h */

    g_state.ram = memalign(32, EE_RAM_SIZE);
    if (!g_state.ram) {
        printf("[!] Could not allocate %u MB of EE RAM (out of memory)\n",
               EE_RAM_SIZE / (1024 * 1024));
        return -1;
    }
    memset(g_state.ram, 0, EE_RAM_SIZE);
    g_state.ram_size = EE_RAM_SIZE;

    dma_bind_ee_ram(g_state.ram, g_state.ram_size); /* chain-mode DMA reads tags/data from here */
    gif_init();
    dma_set_sink(DMA_CHANNEL_GIF, gif_process_quadwords); /* GIF DMA transfers now actually get parsed and drawn */

    g_state.bios = bios;
    g_state.pc = BIOS_RESET_VECTOR;
    g_state.next_pc = BIOS_RESET_VECTOR + 4;

    /* COP0 PRId (register 15, Processor Revision Identifier) - ported
     * directly from PCSX2's own R5900.cpp ("cpuRegs.CP0.n.PRid =
     * 0x00002e20"). Before this fix, cop0[15] was left at 0 by the
     * memset() above.
     *
     * This is not a cosmetic value: it is the very first thing the
     * real BIOS reads. Instruction #0 at the reset vector is
     * "MFC0 $k0, $15", followed immediately by "SLTI $at, $k0, 89" /
     * "BNE $at, $zero, ...": a CPU-revision check that picks between
     * two completely different early-boot code paths. With PRId left
     * at 0 (0 < 89), this project's interpreter took the WRONG branch
     * from the very first conditional in the whole boot sequence and
     * never reached the real vector-install copy loop the BIOS uses
     * to populate low RAM (confirmed missing via a live trace of real,
     * working PCSX2 - see docs/STATUS.md's "JALR investigation, round
     * 5" for the full trail). This one missing register write is the
     * root cause of the EE JALR-to-out-of-range halt investigated
     * across rounds 1-4. */
    g_state.cop0[15] = 0x00002e20u;

    /* COP0 Status (register 12) reset value - ported directly from
     * PCSX2's own R5900.cpp ("cpuRegs.CP0.n.Status.val = 0x70400004").
     * Before this fix, cop0[12] was left at 0 by the memset() above,
     * which is wrong in one specific, exception-relevant way: real
     * hardware resets with Status.BEV (bit 22) SET, meaning exceptions
     * vector into the BIOS ROM directly (0xBFC00200+) until the BIOS
     * itself clears BEV after installing its own RAM-resident handlers
     * (see the InstallExceptionHandlers work elsewhere in this file).
     * With BEV left at 0, this project's new exception-delivery path
     * (see ee_raise_exception()) would incorrectly vector early boot-
     * time exceptions into RAM instead of ROM. The other bits in this
     * reset value (ERL=1, KSU=0, IE=0, EXL=0) are also real, not
     * fabricated - decoded straight from the same constant PCSX2 uses. */
    g_state.cop0[12] = 0x70400004u;

    return 0;
}

static void halt(const char *reason)
{
    g_state.halted = 1;
    strncpy(g_state.halt_reason, reason, sizeof(g_state.halt_reason) - 1);
}

/* 32-bit ALU results are computed in 32 bits, then sign-extended to
 * fill the low 64-bit half of the register (matches PCSX2's
 * u64(s64(s32(...))) idiom). The upper 64 bits (ud1) are left
 * untouched by ordinary (non-MMI) instructions - this matches real
 * EE/PCSX2 behavior. */
static inline uint64_t sext32(uint32_t v) { return (uint64_t)(int64_t)(int32_t)v; }

/* --- COP1 (FPU) helpers, ported from PCSX2's pcsx2/FPU.cpp ---
 * PS2's FPU isn't strict IEEE-754: it treats denormal inputs as
 * signed zero and clamps infinite inputs to +/-Fmax *before* doing
 * arithmetic (fpuDouble), then separately clamps infinite/denormal
 * *results* after the operation (checkOverflow/checkUnderflow). Both
 * steps matter for matching real hardware/PCSX2 behavior. */
#define FPU_POS_INFINITY 0x7f800000u
#define FPU_POS_FMAX      0x7F7FFFFFu
#define FPU_NEG_FMAX      0xFF7FFFFFu

static inline float bits_to_float(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
static inline uint32_t float_to_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

static float fpu_double(uint32_t f)
{
    uint32_t exp = f & 0x7f800000u;
    if (exp == 0)               return bits_to_float(f & 0x80000000u);              /* denormal -> signed zero */
    if (exp == 0x7f800000u)     return bits_to_float((f & 0x80000000u) | FPU_POS_FMAX); /* infinity -> +/-Fmax */
    return bits_to_float(f);
}

static int fpu_check_overflow(uint32_t *reg)
{
    if ((*reg & ~0x80000000u) == FPU_POS_INFINITY) {
        *reg = (*reg & 0x80000000u) | FPU_POS_FMAX;
        return 1;
    }
    return 0;
}

static void fpu_check_underflow(uint32_t *reg)
{
    if ((*reg & 0x7F800000u) == 0 && (*reg & 0x007FFFFFu) != 0)
        *reg &= 0x80000000u; /* denormal result -> signed zero */
}

/* fp_max/fp_min - ported from PCSX2's FPU.cpp exactly (bit-level
 * comparison, not a float compare): when both operands are negative,
 * a plain signed-32-bit min/max on the raw bit patterns gives the
 * IEEE-754 max/min respectively (negative floats sort in REVERSED
 * order as signed integers), otherwise a plain signed-32-bit max/min
 * on the bit patterns already agrees with the float comparison. This
 * is why PCSX2 implements it this way rather than a naive
 * bits_to_float() compare - it's not a shortcut, it's the actual
 * hardware-matching algorithm. */
static uint32_t fp_max(uint32_t a, uint32_t b)
{
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (sa < 0 && sb < 0) return (uint32_t)((sa < sb) ? sa : sb);
    return (uint32_t)((sa > sb) ? sa : sb);
}
static uint32_t fp_min(uint32_t a, uint32_t b)
{
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (sa < 0 && sb < 0) return (uint32_t)((sa > sb) ? sa : sb);
    return (uint32_t)((sa < sb) ? sa : sb);
}


/* --- 128-bit lane accessors for MMI opcodes (lane 0 = least significant) --- */

static inline uint32_t lane_w(ee_reg128_t r, int n) {
    return (uint32_t)((n < 2 ? r.ud0 : r.ud1) >> ((n & 1) * 32));
}
static inline void set_lane_w(ee_reg128_t *r, int n, uint32_t val) {
    uint64_t *p = (n < 2) ? &r->ud0 : &r->ud1;
    int sh = (n & 1) * 32;
    uint64_t mask = ~((uint64_t)0xFFFFFFFFu << sh);
    *p = (*p & mask) | ((uint64_t)val << sh);
}
static inline uint16_t lane_h(ee_reg128_t r, int n) {
    return (uint16_t)((n < 4 ? r.ud0 : r.ud1) >> ((n & 3) * 16));
}
static inline void set_lane_h(ee_reg128_t *r, int n, uint16_t val) {
    uint64_t *p = (n < 4) ? &r->ud0 : &r->ud1;
    int sh = (n & 3) * 16;
    uint64_t mask = ~((uint64_t)0xFFFFu << sh);
    *p = (*p & mask) | ((uint64_t)val << sh);
}
static inline uint8_t lane_b(ee_reg128_t r, int n) {
    return (uint8_t)((n < 8 ? r.ud0 : r.ud1) >> ((n & 7) * 8));
}
static inline void set_lane_b(ee_reg128_t *r, int n, uint8_t val) {
    uint64_t *p = (n < 8) ? &r->ud0 : &r->ud1;
    int sh = (n & 7) * 8;
    uint64_t mask = ~((uint64_t)0xFFu << sh);
    *p = (*p & mask) | ((uint64_t)val << sh);
}

static int ee_step(void)
{
    ee_state_t *st = &g_state;
    uint32_t pc = st->pc;

    /* Capture + clear the delay-slot flag the PREVIOUS instruction may
     * have left for us (see branch_pending's comment in ee_core.h),
     * and publish this instruction'''s own transient exception context
     * BEFORE the fetch below - which can itself fault (a real TLB
     * Refill exception on a KUSEG instruction fetch) - so
     * ee_raise_tlb_exception() always has the right EPC/BD to use, no
     * matter how deep in this instruction'''s execution the fault
     * actually happens. */
    int in_delay_slot = st->branch_pending;
    st->branch_pending = 0;
    st->exc_this_pc = pc;
    st->exc_in_delay_slot = (uint8_t)in_delay_slot;
    st->exc_raised_this_step = 0;

    uint32_t instr = ee_mem_read32(st, pc);
    if (st->mem_tlb_miss) {
        /* Instruction-fetch TLB Refill: ee_mem_read32() -> ee_mem_ptr()
         * already raised the exception and pointed st->pc/next_pc at
         * the vector. Bail out now, before the unconditional
         * st->pc = fallthrough_pc below would clobber that vector with
         * this (never-fetched) instruction's own stale fallthrough
         * address. */
        return 0;
    }

    uint32_t op    = (instr >> 26) & 0x3F;
    uint32_t rs    = (instr >> 21) & 0x1F;
    uint32_t rt    = (instr >> 16) & 0x1F;
    uint32_t rd    = (instr >> 11) & 0x1F;
    uint32_t sa    = (instr >> 6)  & 0x1F; /* also used as MMI sub-table selector */
    int32_t  imm   = (int16_t)(instr & 0xFFFF);
    uint32_t uimm  = instr & 0xFFFF;
    uint32_t funct = instr & 0x3F;

    uint32_t this_pc = pc;
    uint32_t fallthrough_pc = st->next_pc;
    st->pc = fallthrough_pc;
    st->next_pc = fallthrough_pc + 4;

    uint32_t rs32 = (uint32_t)st->gpr[rs].ud0;
    uint32_t rt32 = (uint32_t)st->gpr[rt].ud0;

#define GPR(x)  st->gpr[x].ud0
#define GPR1(x) st->gpr[x].ud1
/* Marks that the instruction right after this one (this_pc + 4, i.e.
 * whatever st->pc already got set to just above) is a branch-delay
 * slot - see branch_pending'''s comment in ee_core.h. Every BRANCH_TO()
 * call means a branch/jump is actually being taken (regular branch
 * taken, unconditional J/JAL/JR/JALR, or a taken Branch Likely) and
 * its delay slot WILL execute next, so this is set unconditionally
 * here. The regular (non-Likely) conditional branches additionally set
 * it manually even when NOT taken, since their delay slot executes
 * either way - see e.g. BEQ/BNE/BLTZ/BC1F below - whereas Branch
 * Likely'''s not-taken path deliberately skips both BRANCH_TO() and the
 * delay slot entirely (annulled), so it correctly leaves this unset. */
#define BRANCH_TO(target) do { st->next_pc = (target); st->branch_pending = 1; } while (0)
#define LINK(reg) do { GPR(reg) = this_pc + 8; } while (0)

    switch (op) {
    case 0x00: /* SPECIAL */
        switch (funct) {
        case 0x00: /* SLL */    if (rd) GPR(rd) = sext32(rt32 << sa); break;
        case 0x02: /* SRL */    if (rd) GPR(rd) = sext32(rt32 >> sa); break;
        case 0x03: /* SRA */    if (rd) GPR(rd) = sext32((uint32_t)((int32_t)rt32 >> sa)); break;
        case 0x04: /* SLLV */   if (rd) GPR(rd) = sext32(rt32 << (rs32 & 0x1F)); break;
        case 0x06: /* SRLV */   if (rd) GPR(rd) = sext32(rt32 >> (rs32 & 0x1F)); break;
        case 0x07: /* SRAV */   if (rd) GPR(rd) = sext32((uint32_t)((int32_t)rt32 >> (rs32 & 0x1F))); break;
        case 0x08: /* JR */     BRANCH_TO((uint32_t)GPR(rs)); break;
        case 0x09: /* JALR */   { uint32_t tgt = (uint32_t)GPR(rs); if (rd) LINK(rd); BRANCH_TO(tgt); } break;
        case 0x0A: /* MOVZ */   if (rd && GPR(rt) == 0) GPR(rd) = GPR(rs); break;
        case 0x0B: /* MOVN */   if (rd && GPR(rt) != 0) GPR(rd) = GPR(rs); break;
        case 0x0C: /* SYSCALL */ halt("SYSCALL (no BIOS syscall table implemented)"); return 1;
        case 0x0D: /* BREAK */  halt("BREAK"); return 1;
        case 0x0F: /* SYNC */   break; /* no-op: no cache/pipeline model */
        case 0x10: /* MFHI */   if (rd) GPR(rd) = st->hi.ud0; break;
        case 0x11: /* MTHI */   st->hi.ud0 = GPR(rs); break;
        case 0x12: /* MFLO */   if (rd) GPR(rd) = st->lo.ud0; break;
        case 0x13: /* MTLO */   st->lo.ud0 = GPR(rs); break;
        case 0x14: /* DSLLV */  if (rd) GPR(rd) = GPR(rt) << (rs32 & 0x3F); break;
        case 0x16: /* DSRLV */  if (rd) GPR(rd) = GPR(rt) >> (rs32 & 0x3F); break;
        case 0x17: /* DSRAV */  if (rd) GPR(rd) = (uint64_t)((int64_t)GPR(rt) >> (rs32 & 0x3F)); break;
        case 0x18: /* MULT */ {
            int64_t res = (int64_t)(int32_t)rs32 * (int64_t)(int32_t)rt32;
            st->lo.ud0 = sext32((uint32_t)(res & 0xFFFFFFFFu));
            st->hi.ud0 = sext32((uint32_t)(res >> 32));
            if (rd) GPR(rd) = st->lo.ud0;
        } break;
        case 0x19: /* MULTU */ {
            uint64_t res = (uint64_t)rs32 * (uint64_t)rt32;
            st->lo.ud0 = sext32((uint32_t)(res & 0xFFFFFFFFu));
            st->hi.ud0 = sext32((uint32_t)(res >> 32));
            if (rd) GPR(rd) = st->lo.ud0;
        } break;
        case 0x1A: /* DIV */
            if (rt32 != 0) {
                st->lo.ud0 = sext32((uint32_t)((int32_t)rs32 / (int32_t)rt32));
                st->hi.ud0 = sext32((uint32_t)((int32_t)rs32 % (int32_t)rt32));
            }
            break;
        case 0x1B: /* DIVU */
            if (rt32 != 0) {
                st->lo.ud0 = sext32(rs32 / rt32);
                st->hi.ud0 = sext32(rs32 % rt32);
            }
            break;
        case 0x20: /* ADD */
        case 0x21: /* ADDU */   if (rd) GPR(rd) = sext32(rs32 + rt32); break;
        case 0x22: /* SUB */
        case 0x23: /* SUBU */   if (rd) GPR(rd) = sext32(rs32 - rt32); break;
        case 0x24: /* AND */    if (rd) GPR(rd) = GPR(rs) & GPR(rt); break;
        case 0x25: /* OR */     if (rd) GPR(rd) = GPR(rs) | GPR(rt); break;
        case 0x26: /* XOR */    if (rd) GPR(rd) = GPR(rs) ^ GPR(rt); break;
        case 0x27: /* NOR */    if (rd) GPR(rd) = ~(GPR(rs) | GPR(rt)); break;
        case 0x2A: /* SLT */    if (rd) GPR(rd) = ((int64_t)GPR(rs) < (int64_t)GPR(rt)) ? 1 : 0; break;
        case 0x2B: /* SLTU */   if (rd) GPR(rd) = (GPR(rs) < GPR(rt)) ? 1 : 0; break;
        case 0x2D: /* DADDU */  if (rd) GPR(rd) = GPR(rs) + GPR(rt); break;
        case 0x2F: /* DSUBU */  if (rd) GPR(rd) = GPR(rs) - GPR(rt); break;
        case 0x38: /* DSLL */   if (rd) GPR(rd) = GPR(rt) << sa; break;
        case 0x3A: /* DSRL */   if (rd) GPR(rd) = GPR(rt) >> sa; break;
        case 0x3B: /* DSRA */   if (rd) GPR(rd) = (uint64_t)((int64_t)GPR(rt) >> sa); break;
        case 0x3C: /* DSLL32 */ if (rd) GPR(rd) = GPR(rt) << (sa + 32); break;
        case 0x3E: /* DSRL32 */ if (rd) GPR(rd) = GPR(rt) >> (sa + 32); break;
        case 0x3F: /* DSRA32 */ if (rd) GPR(rd) = (uint64_t)((int64_t)GPR(rt) >> (sa + 32)); break;
        default:
            halt("unimplemented SPECIAL funct");
            return 1;
        }
        break;

    case 0x01: /* REGIMM */
        switch (rt) {
        case 0x00: /* BLTZ */ st->branch_pending = 1; /* delay slot always executes for regular branches, taken or not */ if ((int64_t)GPR(rs) < 0)  BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        case 0x01: /* BGEZ */ st->branch_pending = 1; if ((int64_t)GPR(rs) >= 0) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        /* "Likely" branches (MIPS II+, ported from PCSX2's
         * Interpreter.cpp): if the condition is FALSE, the delay slot
         * is NOT executed at all (nullified) - unlike ordinary
         * branches, which always execute their delay slot regardless
         * of whether the branch is taken. Modeled by skipping straight
         * to fallthrough_pc+4 (past the delay slot) instead of letting
         * the normal fallthrough_pc execute next. */
        case 0x02: /* BLTZL */  if ((int64_t)GPR(rs) < 0)  BRANCH_TO(this_pc + 4 + (imm << 2)); else { st->pc = fallthrough_pc + 4; st->next_pc = fallthrough_pc + 8; } break;
        case 0x03: /* BGEZL */  if ((int64_t)GPR(rs) >= 0) BRANCH_TO(this_pc + 4 + (imm << 2)); else { st->pc = fallthrough_pc + 4; st->next_pc = fallthrough_pc + 8; } break;
        case 0x12: /* BLTZALL - links unconditionally (matches PCSX2's
             * _SetLink(31) running before the branch-taken check),
             * even when the branch itself is not taken. */
            LINK(31);
            if ((int64_t)GPR(rs) < 0) BRANCH_TO(this_pc + 4 + (imm << 2)); else { st->pc = fallthrough_pc + 4; st->next_pc = fallthrough_pc + 8; }
            break;
        case 0x13: /* BGEZALL - same unconditional-link caveat as BLTZALL. */
            LINK(31);
            if ((int64_t)GPR(rs) >= 0) BRANCH_TO(this_pc + 4 + (imm << 2)); else { st->pc = fallthrough_pc + 4; st->next_pc = fallthrough_pc + 8; }
            break;
        default:
            halt("unimplemented REGIMM opcode");
            return 1;
        }
        break;

    case 0x02: /* J */   BRANCH_TO((this_pc & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2)); break;
    case 0x03: /* JAL */  LINK(31); BRANCH_TO((this_pc & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2)); break;
    case 0x04: /* BEQ */  st->branch_pending = 1; if (GPR(rs) == GPR(rt)) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
    case 0x05: /* BNE */  st->branch_pending = 1; if (GPR(rs) != GPR(rt)) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
    case 0x06: /* BLEZ */ st->branch_pending = 1; if ((int64_t)GPR(rs) <= 0) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
    case 0x07: /* BGTZ */ st->branch_pending = 1; if ((int64_t)GPR(rs) > 0)  BRANCH_TO(this_pc + 4 + (imm << 2)); break;

    /* "Likely" branches (MIPS II+, primary opcodes 0x14-0x17), ported
     * from PCSX2's Interpreter.cpp. Same delay-slot-nullification
     * semantics as the REGIMM likely variants above: if not taken,
     * skip straight past the delay slot instead of executing it. This
     * family was found missing (halting cleanly on "unimplemented
     * primary opcode 0x14") once the COP0 PRId fix (see docs/STATUS.md
     * round 5) got real BIOS boot far enough to actually need it -
     * real BIOS code uses BEQL/BNEL/etc. constantly for tight
     * loops/polling, unlike the ordinary branches this project already
     * had full coverage of. */
    case 0x14: /* BEQL */  if (GPR(rs) == GPR(rt)) BRANCH_TO(this_pc + 4 + (imm << 2)); else { st->pc = fallthrough_pc + 4; st->next_pc = fallthrough_pc + 8; } break;
    case 0x15: /* BNEL */  if (GPR(rs) != GPR(rt)) BRANCH_TO(this_pc + 4 + (imm << 2)); else { st->pc = fallthrough_pc + 4; st->next_pc = fallthrough_pc + 8; } break;
    case 0x16: /* BLEZL */ if ((int64_t)GPR(rs) <= 0) BRANCH_TO(this_pc + 4 + (imm << 2)); else { st->pc = fallthrough_pc + 4; st->next_pc = fallthrough_pc + 8; } break;
    case 0x17: /* BGTZL */ if ((int64_t)GPR(rs) > 0)  BRANCH_TO(this_pc + 4 + (imm << 2)); else { st->pc = fallthrough_pc + 4; st->next_pc = fallthrough_pc + 8; } break;

    case 0x08: /* ADDI */
    case 0x09: /* ADDIU */ if (rt) GPR(rt) = sext32((uint32_t)((int32_t)rs32 + imm)); break;
    /* DADDI/DADDIU (primary 0x18/0x19): 64-bit reg + sign-extended
     * imm, full 64-bit result (no truncation/re-sign-extension like
     * the 32-bit ADDI/ADDIU pair above). Found missing (halting
     * cleanly on "unimplemented primary opcode 0x19") once round 11's
     * MCH_RICM/MCH_DRD RDRAM auto-init fix let real BIOS boot progress
     * roughly 100x further than before, into code this project had
     * never reached. Like ADDI above, DADDI's real overflow-trap
     * semantics aren't implemented (matches this project's existing,
     * documented ADDI simplification) - both variants behave like
     * DADDIU. */
    case 0x18: /* DADDI */
    case 0x19: /* DADDIU */ if (rt) GPR(rt) = GPR(rs) + (uint64_t)(int64_t)imm; break;
    case 0x0A: /* SLTI */  if (rt) GPR(rt) = ((int64_t)GPR(rs) < (int64_t)imm) ? 1 : 0; break;
    case 0x0B: /* SLTIU */ if (rt) GPR(rt) = (GPR(rs) < (uint64_t)(int64_t)imm) ? 1 : 0; break;
    case 0x0C: /* ANDI */  if (rt) GPR(rt) = GPR(rs) & (uint64_t)uimm; break;
    case 0x0D: /* ORI */   if (rt) GPR(rt) = GPR(rs) | (uint64_t)uimm; break;
    case 0x0E: /* XORI */  if (rt) GPR(rt) = GPR(rs) ^ (uint64_t)uimm; break;
    case 0x0F: /* LUI */   if (rt) GPR(rt) = sext32(uimm << 16); break;

    case 0x10: /* COP0 */
        switch (rs) {
        case 0x00: /* MFC0 */
            if (rt) GPR(rt) = sext32(st->cop0[rd]);
            break;
        case 0x04: /* MTC0 */
            if (rd == 16) { /* Config: preserve read-only IC/DC bits like PCSX2's WriteCP0Config */
                st->cop0[16] = (rt32 & ~0xFC0u) | 0x440u;
            } else if (rd == 11) { /* Compare: writing it clears the latched
                 * timer-interrupt pending bit (Cause.IP7) - see
                 * ee_latch_timer_interrupt()'s comment above (round 9). */
                st->cop0[11] = rt32;
                st->cop0[13] &= ~EE_CAUSE_IP7;
            } else {
                st->cop0[rd] = rt32;
            }
            break;
        default:
            if (rs & 0x10) {
                /* "CO" format: rs's top bit set means the real
                 * operation is selected by the 6-bit funct field
                 * (bits 0-5), not by rs itself - matches PCSX2's own
                 * tbl_COP0_C0[64] dispatch table in
                 * R5900OpcodeTables.cpp. */
                uint32_t co_funct = instr & 0x3F;
                switch (co_funct) {
                case 0x10: /* RFE - "Return From Exception".
                     * NOTE: real PCSX2's EE (R5900) opcode table does
                     * NOT implement this (tbl_COP0_C0[0x10] is
                     * COP0_Unknown there) - RFE is the ORIGINAL MIPS I
                     * exception-return instruction, superseded by
                     * ERET (funct 0x18, below) on MIPS III+. It shows
                     * up here because this real BIOS dump's early
                     * boot code includes PS1-backward-compatibility-
                     * mode boot code (this file's own header bytes
                     * contain the ASCII string "PS compatible mode"),
                     * which runs in a MIPS-I-compatible execution
                     * context where RFE is meaningful. Implemented
                     * with RFE's standard, unambiguous MIPS I
                     * semantics (restore the 3-level interrupt-
                     * enable/kernel-mode bit stack by shifting it
                     * right by 2) rather than skipped/faked, since
                     * that's well-defined regardless of the exact
                     * execution mode question above. RFE does NOT
                     * itself change PC (unlike ERET) - the caller
                     * separately jumps to EPC, typically via JR. */
                    st->cop0[12] = (st->cop0[12] & ~0x0Fu) | ((st->cop0[12] >> 2) & 0x0Fu);
                    break;
                case 0x18: /* ERET - ported from PCSX2's COP0::ERET().
                     * No branch delay slot (unlike ordinary jumps/
                     * branches) - takes effect immediately, so this
                     * sets st->pc/next_pc directly rather than going
                     * through the BRANCH_TO() delay-slot convention. */
                {
                    uint32_t target;
                    if (st->cop0[12] & 0x4u) { /* Status.ERL */
                        target = st->cop0[30]; /* ErrorEPC */
                        st->cop0[12] &= ~0x4u;
                    } else {
                        target = st->cop0[14]; /* EPC */
                        st->cop0[12] &= ~0x2u; /* Status.EXL */
                    }
                    st->pc = target;
                    st->next_pc = target + 4;
                    break;
                }
                case 0x38: /* EI - ported from PCSX2's COP0::EI(). Gated
                     * the same way real hardware/PCSX2 gates it:
                     * only takes effect if _EDI, EXL, ERL are set, or
                     * KSU==0 (kernel mode) - i.e. code running in
                     * user mode without the right permission bit
                     * can't enable interrupts via this instruction. */
                    if ((st->cop0[12] & 0x20000u) ||      /* _EDI  (bit 17) */
                        (st->cop0[12] & 0x2u) ||           /* EXL   (bit 1)  */
                        (st->cop0[12] & 0x4u) ||           /* ERL   (bit 2)  */
                        ((st->cop0[12] & 0x18u) == 0))      /* KSU==0 (bits 3-4) */
                    {
                        st->cop0[12] |= 0x10000u; /* Status.EIE = 1 (bit 16) */
                    }
                    break;
                case 0x39: /* DI - ported from PCSX2's COP0::DI(), same gating as EI. */
                    if ((st->cop0[12] & 0x20000u) ||
                        (st->cop0[12] & 0x2u) ||
                        (st->cop0[12] & 0x4u) ||
                        ((st->cop0[12] & 0x18u) == 0))
                    {
                        st->cop0[12] &= ~0x10000u; /* Status.EIE = 0 */
                    }
                    break;
                case 0x01: /* TLBR - Read Indexed TLB Entry. Ported from
                     * PCSX2's COP0::TLBR(). Loads the TLB entry at
                     * Index (cop0[0], masked to 6 bits/48 entries)
                     * back into PageMask/EntryHi/EntryLo0/EntryLo1
                     * (cop0[5]/[10]/[2]/[3]), applying the same
                     * read-back masking real hardware does (G is only
                     * set in the read-back copies if BOTH EntryLo0 and
                     * EntryLo1 have G set in the stored entry - see
                     * PCSX2's comment on this exact quirk). */
                {
                    uint32_t i = st->cop0[0] & 0x3Fu;
                    if (i > 47) break; /* real hardware/PCSX2 both just warn and no-op past entry 47 */
                    uint32_t lo0 = st->tlb[i].entry_lo0, lo1 = st->tlb[i].entry_lo1;
                    uint32_t g = (lo0 & 1u) & (lo1 & 1u);
                    st->cop0[5]  = st->tlb[i].page_mask;
                    st->cop0[10] = st->tlb[i].entry_hi & ~((st->tlb[i].page_mask) | 0x1F00u);
                    st->cop0[2]  = (lo0 & ~0xFC000000u & ~1u) | g;
                    st->cop0[3]  = (lo1 & ~0x7C000000u & ~1u) | g;
                    break;
                }
                case 0x02: /* TLBWI - Write Indexed TLB Entry. Ported
                     * from PCSX2's COP0::TLBWI()/WriteTLB(). Stores
                     * the current PageMask/EntryHi/EntryLo0/EntryLo1
                     * into the TLB at Index. NOTE: this project does
                     * not wire TLB lookups into actual address
                     * translation (ee_mem_ptr() still treats kuseg/
                     * kseg0/kseg1 as flat physical-masked mappings) -
                     * this stores the entry faithfully so MFC0/TLBR/
                     * TLBP round-trip correctly, matching real
                     * hardware's register-level behavior, without
                     * claiming to model the MMU's actual page-walk. */
                {
                    uint32_t j = st->cop0[0] & 0x3Fu;
                    if (j > 47) break;
                    st->tlb[j].page_mask = st->cop0[5];
                    st->tlb[j].entry_hi  = st->cop0[10];
                    st->tlb[j].entry_lo0 = st->cop0[2];
                    st->tlb[j].entry_lo1 = st->cop0[3];
                    break;
                }
                case 0x06: /* TLBWR - Write Random TLB Entry. Same as
                     * TLBWI but indexed by Random (cop0[1]) instead of
                     * Index. Real hardware/PCSX2 decrement Random
                     * every cycle between Wired and 47; this project
                     * does not model that decay (Random is just
                     * whatever value software last wrote via MTC0, or
                     * 0) - a documented simplification, not a
                     * fabricated behavior, since we don't claim
                     * cycle-accurate Random decay. */
                {
                    uint32_t j = st->cop0[1] & 0x3Fu;
                    if (j > 47) break;
                    st->tlb[j].page_mask = st->cop0[5];
                    st->tlb[j].entry_hi  = st->cop0[10];
                    st->tlb[j].entry_lo0 = st->cop0[2];
                    st->tlb[j].entry_lo1 = st->cop0[3];
                    break;
                }
                case 0x08: /* TLBP - Probe TLB for Matching Entry.
                     * Ported from PCSX2's COP0::TLBP(). Searches all
                     * 48 entries for one whose (masked) VPN2 and
                     * ASID/Global match the current EntryHi, sets
                     * Index to the match (or 0x80000000, i.e. sign bit
                     * set, if none found - the real "not found"
                     * convention, checked via MFC0 $rt,$0 returning a
                     * negative value). */
                {
                    uint32_t eh = st->cop0[10];
                    uint32_t want_vpn2 = (eh >> 13) & 0x7FFFFu;
                    uint32_t want_asid = eh & 0xFFu;
                    uint32_t found = 0xFFFFFFFFu;
                    for (uint32_t i = 0; i < 48; i++) {
                        uint32_t mask = (st->tlb[i].page_mask >> 13) & 0xFFFu;
                        uint32_t entry_vpn2 = (st->tlb[i].entry_hi >> 13) & 0x7FFFFu;
                        int is_global = (st->tlb[i].entry_lo0 & 1u) && (st->tlb[i].entry_lo1 & 1u);
                        uint32_t entry_asid = st->tlb[i].entry_hi & 0xFFu;
                        if ((entry_vpn2 & ~mask) == (want_vpn2 & ~mask) &&
                            (is_global || entry_asid == want_asid)) {
                            found = i;
                            break;
                        }
                    }
                    st->cop0[0] = (found == 0xFFFFFFFFu) ? 0x80000000u : found;
                    break;
                }
                default:
                {
                    char buf[96];
                    snprintf(buf, sizeof(buf),
                             "unimplemented COP0 CO-format funct 0x%02X (pc=0x%08X)",
                             (unsigned int)co_funct, (unsigned int)this_pc);
                    halt(buf);
                    return 1;
                }
                }
            } else {
                char buf[96];
                snprintf(buf, sizeof(buf),
                         "unimplemented COP0 sub-opcode (rs=0x%02X, pc=0x%08X, BC0 not implemented)",
                         (unsigned int)rs, (unsigned int)this_pc);
                halt(buf);
                return 1;
            }
            break;
        }
        break;

    case 0x11: /* COP1 (FPU, single-precision only) */
        switch (rs) {
        case 0x00: /* MFC1 */ if (rt) GPR(rt) = sext32(st->fpr[rd]); break;
        case 0x02: /* CFC1 */
            if (rt) {
                if (rd == 31) GPR(rt) = sext32(st->fcr31);
                else if (rd == 0) GPR(rt) = sext32(0x2E00u);
                else GPR(rt) = 0;
            }
            break;
        case 0x04: /* MTC1 */ st->fpr[rd] = rt32; break;
        case 0x06: /* CTC1 */ if (rd == 31) st->fcr31 = rt32; break;
        case 0x10: { /* COP1.S - single-precision arithmetic, fd=sa fs=rd ft=rt */
            uint32_t fd = sa, fs = rd, ft = rt;
            switch (funct) {
            case 0x00: /* ADD.S */ {
                float r = fpu_double(st->fpr[fs]) + fpu_double(st->fpr[ft]);
                st->fpr[fd] = float_to_bits(r);
                if (!fpu_check_overflow(&st->fpr[fd])) fpu_check_underflow(&st->fpr[fd]);
            } break;
            case 0x01: /* SUB.S */ {
                float r = fpu_double(st->fpr[fs]) - fpu_double(st->fpr[ft]);
                st->fpr[fd] = float_to_bits(r);
                if (!fpu_check_overflow(&st->fpr[fd])) fpu_check_underflow(&st->fpr[fd]);
            } break;
            case 0x02: /* MUL.S */ {
                float r = fpu_double(st->fpr[fs]) * fpu_double(st->fpr[ft]);
                st->fpr[fd] = float_to_bits(r);
                if (!fpu_check_overflow(&st->fpr[fd])) fpu_check_underflow(&st->fpr[fd]);
            } break;
            case 0x03: /* DIV.S */ {
                uint32_t divisor = st->fpr[ft], dividend = st->fpr[fs];
                if ((divisor & 0x7F800000u) == 0) {
                    /* divide-by-zero (denormal counts as zero too): result is
                     * signed +/-Fmax, sign = XOR of operand signs. */
                    st->fpr[fd] = ((divisor ^ dividend) & 0x80000000u) | FPU_POS_FMAX;
                } else {
                    float r = fpu_double(dividend) / fpu_double(divisor);
                    st->fpr[fd] = float_to_bits(r);
                    fpu_check_overflow(&st->fpr[fd]);
                    fpu_check_underflow(&st->fpr[fd]);
                }
            } break;
            case 0x05: /* ABS.S */ st->fpr[fd] = st->fpr[fs] & 0x7fffffffu; break;
            case 0x06: /* MOV.S */ st->fpr[fd] = st->fpr[fs]; break;
            case 0x07: /* NEG.S */ st->fpr[fd] = st->fpr[fs] ^ 0x80000000u; break;
            case 0x24: /* CVT.W.S (float -> int32) */
                if ((st->fpr[fs] & 0x7F800000u) <= 0x4E800000u) {
                    st->fpr[fd] = (uint32_t)(int32_t)bits_to_float(st->fpr[fs]);
                } else {
                    st->fpr[fd] = (st->fpr[fs] & 0x80000000u) ? 0x80000000u : 0x7fffffffu;
                }
                break;
            case 0x04: /* SQRT.S - real hardware/PCSX2 quirk: the source
                        * operand is Ft (rt field), NOT Fs, and Fs
                        * (rd field) is unused - ported exactly from
                        * PCSX2's SQRT_S(), not a typo. Documented
                        * simplification, matching this file's
                        * existing FPU scope: the D/I exception-cause
                        * control-register flags PCSX2 also sets here
                        * are NOT modeled (nothing in this project
                        * raises FPU exceptions from them yet - same
                        * simplification already in place for every
                        * other FPU op above, which also skip the
                        * O/U/SO/SU flags). */
                if ((st->fpr[ft] & 0x7F800000u) == 0) {
                    st->fpr[fd] = st->fpr[ft] & 0x80000000u; /* +/-0 */
                } else if (st->fpr[ft] & 0x80000000u) {
                    float r = sqrtf(fabsf(fpu_double(st->fpr[ft])));
                    st->fpr[fd] = float_to_bits(r);
                } else {
                    float r = sqrtf(fpu_double(st->fpr[ft]));
                    st->fpr[fd] = float_to_bits(r);
                }
                break;
            case 0x16: /* RSQRT.S: fd = fs / sqrt(ft) - ported from
                        * PCSX2's RSQRT_S(). Same documented flag
                        * simplification as SQRT.S above. */
                if ((st->fpr[ft] & 0x7F800000u) == 0) {
                    /* ft is +/-0 (denormals-are-zero): result is
                     * +/-Fmax, sign taken from ft. */
                    st->fpr[fd] = (st->fpr[ft] & 0x80000000u) | FPU_POS_FMAX;
                } else if (st->fpr[ft] & 0x80000000u) {
                    float denom = sqrtf(fabsf(fpu_double(st->fpr[ft])));
                    float r = fpu_double(st->fpr[fs]) / denom;
                    st->fpr[fd] = float_to_bits(r);
                    fpu_check_overflow(&st->fpr[fd]);
                    fpu_check_underflow(&st->fpr[fd]);
                } else {
                    float r = fpu_double(st->fpr[fs]) / sqrtf(fpu_double(st->fpr[ft]));
                    st->fpr[fd] = float_to_bits(r);
                    fpu_check_overflow(&st->fpr[fd]);
                    fpu_check_underflow(&st->fpr[fd]);
                }
                break;
            case 0x28: /* MAX.S - ported from PCSX2's MAX_S()/fp_max():
                        * a bit-level signed-int max/min, not a float
                        * compare - see fp_max()'s comment. No
                        * overflow/underflow clamping needed (result
                        * is always one of the two original values). */
                st->fpr[fd] = fp_max(st->fpr[fs], st->fpr[ft]);
                break;
            case 0x29: /* MIN.S - ported from PCSX2's MIN_S()/fp_min(). */
                st->fpr[fd] = fp_min(st->fpr[fs], st->fpr[ft]);
                break;
            case 0x18: /* ADDA.S - ACC = fs + ft, ported from PCSX2's
                        * ADDA_S(). */
            {
                float r = fpu_double(st->fpr[fs]) + fpu_double(st->fpr[ft]);
                st->acc = float_to_bits(r);
                if (!fpu_check_overflow(&st->acc)) fpu_check_underflow(&st->acc);
            } break;
            case 0x19: /* SUBA.S - ACC = fs - ft, ported from SUBA_S(). */
            {
                float r = fpu_double(st->fpr[fs]) - fpu_double(st->fpr[ft]);
                st->acc = float_to_bits(r);
                if (!fpu_check_overflow(&st->acc)) fpu_check_underflow(&st->acc);
            } break;
            case 0x1A: /* MULA.S - ACC = fs * ft, ported from MULA_S(). */
            {
                float r = fpu_double(st->fpr[fs]) * fpu_double(st->fpr[ft]);
                st->acc = float_to_bits(r);
                if (!fpu_check_overflow(&st->acc)) fpu_check_underflow(&st->acc);
            } break;
            case 0x1C: /* MADD.S - fd = ACC + (fs * ft), ported from
                        * PCSX2's MADD_S(). Real hardware/PCSX2 quirk
                        * worth preserving exactly: the intermediate
                        * product is run through fpuDouble() a SECOND
                        * time when it's read back for the addition
                        * (PCSX2's own FPRreg temp; temp.f = fpuDouble
                        * (fs)*fpuDouble(ft); then fpuDouble(temp.UL)
                        * again) - unlike MADDA.S below, which doesn't
                        * do this second pass. Not an oversight to
                        * "simplify away" - ported as-is. */
            {
                float prod = fpu_double(st->fpr[fs]) * fpu_double(st->fpr[ft]);
                uint32_t prod_bits = float_to_bits(prod);
                float r = fpu_double(st->acc) + fpu_double(prod_bits);
                st->fpr[fd] = float_to_bits(r);
                if (!fpu_check_overflow(&st->fpr[fd])) fpu_check_underflow(&st->fpr[fd]);
            } break;
            case 0x1D: /* MSUB.S - fd = ACC - (fs * ft), same
                        * double-fpuDouble()-pass quirk as MADD.S
                        * above, ported from PCSX2's MSUB_S(). */
            {
                float prod = fpu_double(st->fpr[fs]) * fpu_double(st->fpr[ft]);
                uint32_t prod_bits = float_to_bits(prod);
                float r = fpu_double(st->acc) - fpu_double(prod_bits);
                st->fpr[fd] = float_to_bits(r);
                if (!fpu_check_overflow(&st->fpr[fd])) fpu_check_underflow(&st->fpr[fd]);
            } break;
            case 0x1E: /* MADDA.S - ACC += fs * ft, ported from PCSX2's
                        * MADDA_S(). NOTE: no intermediate
                        * fpuDouble()-of-the-product pass here, unlike
                        * MADD.S above - that asymmetry is real,
                        * ported exactly as PCSX2 has it. */
            {
                float r = fpu_double(st->acc) + (fpu_double(st->fpr[fs]) * fpu_double(st->fpr[ft]));
                st->acc = float_to_bits(r);
                if (!fpu_check_overflow(&st->acc)) fpu_check_underflow(&st->acc);
            } break;
            case 0x1F: /* MSUBA.S - ACC -= fs * ft, ported from MSUBA_S(). */
            {
                float r = fpu_double(st->acc) - (fpu_double(st->fpr[fs]) * fpu_double(st->fpr[ft]));
                st->acc = float_to_bits(r);
                if (!fpu_check_overflow(&st->acc)) fpu_check_underflow(&st->acc);
            } break;
            case 0x32: /* C.EQ.S */
                if (fpu_double(st->fpr[fs]) == fpu_double(st->fpr[ft])) st->fcr31 |= 0x00800000u;
                else st->fcr31 &= ~0x00800000u;
                break;
            case 0x34: /* C.LT.S */
                if (fpu_double(st->fpr[fs]) < fpu_double(st->fpr[ft])) st->fcr31 |= 0x00800000u;
                else st->fcr31 &= ~0x00800000u;
                break;
            case 0x36: /* C.LE.S */
                if (fpu_double(st->fpr[fs]) <= fpu_double(st->fpr[ft])) st->fcr31 |= 0x00800000u;
                else st->fcr31 &= ~0x00800000u;
                break;
            default:
                halt("unimplemented COP1.S funct (SQRT/RSQRT/MADD/MAX/MIN family not implemented)");
                return 1;
            }
        } break;
        case 0x14: /* COP1.W - only CVT.S.W (int32 -> float) */
            if (funct == 0x20) {
                uint32_t fd = sa, fs = rd;
                st->fpr[fd] = float_to_bits((float)(int32_t)st->fpr[fs]);
            } else {
                halt("unimplemented COP1.W funct");
                return 1;
            }
            break;
        case 0x08: /* COP1 BC (branch on FP condition flag) - sub-selected
                    * by the rt field (matches PCSX2's tbl_COP1_BC1[32],
                    * indexed 0=BC1F, 1=BC1T, 2=BC1FL, 3=BC1TL). Only
                    * the two non-"likely" variants are implemented -
                    * see the case default below for why BC1FL/BC1TL
                    * are still open. */
            switch (rt) {
            case 0x00: /* BC1F - branch if the FP condition flag (fcr31
                        * bit 0x00800000, set by C.EQ/LT/LE.S) is
                        * CLEAR. Ported from PCSX2's BC1()/BC1F(). */
                st->branch_pending = 1; /* regular branch: delay slot always executes */
                if (!(st->fcr31 & 0x00800000u)) BRANCH_TO(this_pc + 4 + (imm << 2));
                break;
            case 0x01: /* BC1T - branch if the flag is SET. */
                st->branch_pending = 1;
                if ((st->fcr31 & 0x00800000u)) BRANCH_TO(this_pc + 4 + (imm << 2));
                break;
            default:
                /* BC1FL/BC1TL ("branch likely" variants): still not
                 * implemented. Real semantics additionally nullify
                 * (skip) the branch delay-slot instruction when the
                 * branch is NOT taken - this project has no "likely
                 * branch" infrastructure yet for ANY branch (integer
                 * BEQL/BNEL/etc. aren't implemented either), so adding
                 * just the FP half would be inconsistent. Left as a
                 * clearly scoped follow-up rather than a half-fix. */
                halt("unimplemented BC1 variant (BC1FL/BC1TL - likely branches not implemented)");
                return 1;
            }
            break;
        default:
            halt("unimplemented COP1 sub-opcode");
            return 1;
        }
        break;

    case 0x12: /* COP2 (VU0 macro mode) - control-register transfers only.
                * See ee_core.h's cop2_ctrl[] comment (round 12) for
                * scope/rationale. rd here is the COP2 control register
                * number (e.g. 28 = FBRST, confirmed via a live PCSX2
                * disassembly of the real BIOS call site that halted
                * on this opcode before this round). */
        switch (rs) {
        case 0x00: /* MFC2 */ if (rt) GPR(rt) = sext32(st->cop2_ctrl[rd]); break;
        case 0x02: /* CFC2 */ if (rt) GPR(rt) = sext32(st->cop2_ctrl[rd]); break;
        case 0x04: /* MTC2 */ st->cop2_ctrl[rd] = rt32; break;
        case 0x06: /* CTC2 */
            /* Real FBRST (control reg 28) semantics - ported from
             * PCSX2's own VU0.cpp CTC2(): bit 0x1 = VU0 force-break,
             * bit 0x2 = VU0 reset, bit 0x100 = VU1 force-break,
             * bit 0x200 = VU1 reset. Not modeled beyond plain storage
             * (see ee_core.h comment) - no VU0/VU1 execution state
             * exists yet for these bits to actually act on. */
            st->cop2_ctrl[rd] = rt32;
            break;
        default:
            /* MFC2/CFC2/MTC2/CTC2 cover every real BIOS/kernel use of
             * COP2 control-register transfers found so far. The
             * actual VU0 vector datapath (QMFC2/QMTC2 128-bit moves,
             * and the full VU macro arithmetic opcode family - ADD/
             * SUB/MUL/MAC/etc., dispatched via the 6-bit funct field
             * once rs's top bit is set, matching COP0/COP1's own "CO"-
             * format convention) is NOT implemented - a real, scoped
             * next wall if a boot path or game ever needs it. */
            halt("unimplemented COP2 sub-opcode (VU0 vector datapath not implemented)");
            return 1;
        }
        break;

    case 0x1C: /* MMI */
        switch (funct) {
        case 0x00: /* MADD */ {
            int64_t acc = (int64_t)((uint64_t)st->lo.ud0 & 0xFFFFFFFFu) | ((int64_t)(int32_t)(uint32_t)st->hi.ud0 << 32);
            int64_t res = acc + (int64_t)(int32_t)rs32 * (int64_t)(int32_t)rt32;
            st->lo.ud0 = sext32((uint32_t)(res & 0xFFFFFFFFu));
            st->hi.ud0 = sext32((uint32_t)(res >> 32));
            if (rd) GPR(rd) = st->lo.ud0;
        } break;
        case 0x01: /* MADDU */ {
            uint64_t acc = ((uint64_t)(uint32_t)st->lo.ud0) | ((uint64_t)(uint32_t)st->hi.ud0 << 32);
            uint64_t res = acc + (uint64_t)rs32 * (uint64_t)rt32;
            st->lo.ud0 = sext32((uint32_t)(res & 0xFFFFFFFFu));
            st->hi.ud0 = sext32((uint32_t)(res >> 32));
            if (rd) GPR(rd) = st->lo.ud0;
        } break;
        case 0x04: /* PLZCW */
            if (rd) {
                for (int n = 0; n < 2; n++) {
                    int32_t v = (int32_t)lane_w(st->gpr[rs], n);
                    int count = 0;
                    /* count leading bits equal to the sign bit, excluding it */
                    uint32_t sign = (uint32_t)v >> 31;
                    uint32_t x = (uint32_t)v;
                    for (int b = 30; b >= 0; b--) {
                        if (((x >> b) & 1) != sign) break;
                        count++;
                    }
                    set_lane_w(&st->gpr[rd], n, (uint32_t)count);
                }
            }
            break;
        case 0x10: /* MFHI1 */ if (rd) GPR(rd) = st->hi.ud1; break;
        case 0x11: /* MTHI1 */ st->hi.ud1 = GPR(rs); break;
        case 0x12: /* MFLO1 */ if (rd) GPR(rd) = st->lo.ud1; break;
        case 0x13: /* MTLO1 */ st->lo.ud1 = GPR(rs); break;
        case 0x18: /* MULT1 */ {
            int64_t res = (int64_t)(int32_t)rs32 * (int64_t)(int32_t)rt32;
            st->lo.ud1 = sext32((uint32_t)(res & 0xFFFFFFFFu));
            st->hi.ud1 = sext32((uint32_t)(res >> 32));
            if (rd) GPR(rd) = st->lo.ud1;
        } break;
        case 0x19: /* MULTU1 */ {
            uint64_t res = (uint64_t)rs32 * (uint64_t)rt32;
            st->lo.ud1 = sext32((uint32_t)(res & 0xFFFFFFFFu));
            st->hi.ud1 = sext32((uint32_t)(res >> 32));
            if (rd) GPR(rd) = st->lo.ud1;
        } break;
        case 0x1A: /* DIV1 */
            if (rt32 != 0) {
                st->lo.ud1 = sext32((uint32_t)((int32_t)rs32 / (int32_t)rt32));
                st->hi.ud1 = sext32((uint32_t)((int32_t)rs32 % (int32_t)rt32));
            }
            break;
        case 0x1B: /* DIVU1 */
            if (rt32 != 0) {
                st->lo.ud1 = sext32(rs32 / rt32);
                st->hi.ud1 = sext32(rs32 % rt32);
            }
            break;
        case 0x34: /* PSLLH */ if (rd) for (int n = 0; n < 8; n++) set_lane_h(&st->gpr[rd], n, (uint16_t)(lane_h(st->gpr[rt], n) << (sa & 0xF))); break;
        case 0x36: /* PSRLH */ if (rd) for (int n = 0; n < 8; n++) set_lane_h(&st->gpr[rd], n, (uint16_t)(lane_h(st->gpr[rt], n) >> (sa & 0xF))); break;
        case 0x37: /* PSRAH */ if (rd) for (int n = 0; n < 8; n++) set_lane_h(&st->gpr[rd], n, (uint16_t)((int16_t)lane_h(st->gpr[rt], n) >> (sa & 0xF))); break;
        case 0x3C: /* PSLLW */ if (rd) for (int n = 0; n < 4; n++) set_lane_w(&st->gpr[rd], n, lane_w(st->gpr[rt], n) << sa); break;
        case 0x3E: /* PSRLW */ if (rd) for (int n = 0; n < 4; n++) set_lane_w(&st->gpr[rd], n, lane_w(st->gpr[rt], n) >> sa); break;
        case 0x3F: /* PSRAW */ if (rd) for (int n = 0; n < 4; n++) set_lane_w(&st->gpr[rd], n, (uint32_t)((int32_t)lane_w(st->gpr[rt], n) >> sa)); break;

        case 0x08: /* MMI0 */
            switch (sa) {
            case 0x00: /* PADDW */ if (rd) for (int n = 0; n < 4; n++) set_lane_w(&st->gpr[rd], n, lane_w(st->gpr[rs], n) + lane_w(st->gpr[rt], n)); break;
            case 0x01: /* PSUBW */ if (rd) for (int n = 0; n < 4; n++) set_lane_w(&st->gpr[rd], n, lane_w(st->gpr[rs], n) - lane_w(st->gpr[rt], n)); break;
            case 0x04: /* PADDH */ if (rd) for (int n = 0; n < 8; n++) set_lane_h(&st->gpr[rd], n, (uint16_t)(lane_h(st->gpr[rs], n) + lane_h(st->gpr[rt], n))); break;
            case 0x05: /* PSUBH */ if (rd) for (int n = 0; n < 8; n++) set_lane_h(&st->gpr[rd], n, (uint16_t)(lane_h(st->gpr[rs], n) - lane_h(st->gpr[rt], n))); break;
            case 0x08: /* PADDB */ if (rd) for (int n = 0; n < 16; n++) set_lane_b(&st->gpr[rd], n, (uint8_t)(lane_b(st->gpr[rs], n) + lane_b(st->gpr[rt], n))); break;
            case 0x09: /* PSUBB */ if (rd) for (int n = 0; n < 16; n++) set_lane_b(&st->gpr[rd], n, (uint8_t)(lane_b(st->gpr[rs], n) - lane_b(st->gpr[rt], n))); break;
            case 0x02: /* PCGTW - per-lane signed 32-bit compare, ported
                        * from PCSX2's _PCGTW(): result is an all-1s
                        * (0xFFFFFFFF) or all-0s mask, not a boolean 0/1,
                        * matching real hardware's SIMD-compare convention. */
                if (rd) for (int n = 0; n < 4; n++)
                    set_lane_w(&st->gpr[rd], n, ((int32_t)lane_w(st->gpr[rs], n) > (int32_t)lane_w(st->gpr[rt], n)) ? 0xFFFFFFFFu : 0x00000000u);
                break;
            case 0x03: /* PMAXW - per-lane signed 32-bit max, ported from _PMAXW(). */
                if (rd) for (int n = 0; n < 4; n++) {
                    int32_t a = (int32_t)lane_w(st->gpr[rs], n), b = (int32_t)lane_w(st->gpr[rt], n);
                    set_lane_w(&st->gpr[rd], n, (uint32_t)((a > b) ? a : b));
                }
                break;
            case 0x06: /* PCGTH - per-lane signed 16-bit compare, ported from _PCGTH(). */
                if (rd) for (int n = 0; n < 8; n++)
                    set_lane_h(&st->gpr[rd], n, ((int16_t)lane_h(st->gpr[rs], n) > (int16_t)lane_h(st->gpr[rt], n)) ? 0xFFFFu : 0x0000u);
                break;
            case 0x07: /* PMAXH - per-lane signed 16-bit max, ported from _PMAXH(). */
                if (rd) for (int n = 0; n < 8; n++) {
                    int16_t a = (int16_t)lane_h(st->gpr[rs], n), b = (int16_t)lane_h(st->gpr[rt], n);
                    set_lane_h(&st->gpr[rd], n, (uint16_t)((a > b) ? a : b));
                }
                break;
            case 0x0A: /* PCGTB - per-lane signed 8-bit compare, ported from _PCGTB(). */
                if (rd) for (int n = 0; n < 16; n++)
                    set_lane_b(&st->gpr[rd], n, ((int8_t)lane_b(st->gpr[rs], n) > (int8_t)lane_b(st->gpr[rt], n)) ? 0xFFu : 0x00u);
                break;
            case 0x12: /* PEXTLW */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    set_lane_w(&out, 0, lane_w(Rt, 0)); set_lane_w(&out, 1, lane_w(Rs, 0));
                    set_lane_w(&out, 2, lane_w(Rt, 1)); set_lane_w(&out, 3, lane_w(Rs, 1));
                    st->gpr[rd] = out;
                }
                break;
            case 0x13: /* PPACW */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    set_lane_w(&out, 0, lane_w(Rt, 0)); set_lane_w(&out, 1, lane_w(Rt, 2));
                    set_lane_w(&out, 2, lane_w(Rs, 0)); set_lane_w(&out, 3, lane_w(Rs, 2));
                    st->gpr[rd] = out;
                }
                break;
            case 0x16: /* PEXTLH */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    set_lane_h(&out, 0, lane_h(Rt, 0)); set_lane_h(&out, 1, lane_h(Rs, 0));
                    set_lane_h(&out, 2, lane_h(Rt, 1)); set_lane_h(&out, 3, lane_h(Rs, 1));
                    set_lane_h(&out, 4, lane_h(Rt, 2)); set_lane_h(&out, 5, lane_h(Rs, 2));
                    set_lane_h(&out, 6, lane_h(Rt, 3)); set_lane_h(&out, 7, lane_h(Rs, 3));
                    st->gpr[rd] = out;
                }
                break;
            case 0x17: /* PPACH */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    set_lane_h(&out, 0, lane_h(Rt, 0)); set_lane_h(&out, 1, lane_h(Rt, 2));
                    set_lane_h(&out, 2, lane_h(Rt, 4)); set_lane_h(&out, 3, lane_h(Rt, 6));
                    set_lane_h(&out, 4, lane_h(Rs, 0)); set_lane_h(&out, 5, lane_h(Rs, 2));
                    set_lane_h(&out, 6, lane_h(Rs, 4)); set_lane_h(&out, 7, lane_h(Rs, 6));
                    st->gpr[rd] = out;
                }
                break;
            case 0x1A: /* PEXTLB */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    for (int n = 0; n < 8; n++) {
                        set_lane_b(&out, n * 2,     lane_b(Rt, n));
                        set_lane_b(&out, n * 2 + 1, lane_b(Rs, n));
                    }
                    st->gpr[rd] = out;
                }
                break;
            case 0x1B: /* PPACB */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    for (int n = 0; n < 8; n++) {
                        set_lane_b(&out, n,     lane_b(Rt, n * 2));
                        set_lane_b(&out, n + 8, lane_b(Rs, n * 2));
                    }
                    st->gpr[rd] = out;
                }
                break;
            case 0x10: /* PADDSW - saturated per-lane signed 32-bit
                        * add, ported from PCSX2's _PADDSW(): computes
                        * the sum in 64-bit to detect over/underflow,
                        * then clamps to INT32_MAX/INT32_MIN instead
                        * of wrapping - the "S" in the name is for
                        * "saturated", not "signed" (all PMMI arithmetic
                        * here is signed either way). */
                if (rd) for (int n = 0; n < 4; n++) {
                    int64_t sum = (int64_t)(int32_t)lane_w(st->gpr[rs], n) + (int64_t)(int32_t)lane_w(st->gpr[rt], n);
                    uint32_t r;
                    if (sum > 0x7FFFFFFFLL) r = 0x7FFFFFFFu;
                    else if (sum < -0x80000000LL) r = 0x80000000u;
                    else r = (uint32_t)(int32_t)sum;
                    set_lane_w(&st->gpr[rd], n, r);
                }
                break;
            case 0x11: /* PSUBSW - saturated per-lane signed 32-bit
                        * subtract, ported from _PSUBSW(). */
                if (rd) for (int n = 0; n < 4; n++) {
                    int64_t diff = (int64_t)(int32_t)lane_w(st->gpr[rs], n) - (int64_t)(int32_t)lane_w(st->gpr[rt], n);
                    uint32_t r;
                    if (diff >= 0x7FFFFFFFLL) r = 0x7FFFFFFFu;
                    else if (diff < -0x80000000LL) r = 0x80000000u;
                    else r = (uint32_t)(int32_t)diff;
                    set_lane_w(&st->gpr[rd], n, r);
                }
                break;
            case 0x14: /* PADDSH - saturated per-lane signed 16-bit
                        * add, ported from _PADDSH(). */
                if (rd) for (int n = 0; n < 8; n++) {
                    int32_t sum = (int32_t)(int16_t)lane_h(st->gpr[rs], n) + (int32_t)(int16_t)lane_h(st->gpr[rt], n);
                    uint16_t r;
                    if (sum > 0x7FFF) r = 0x7FFFu;
                    else if (sum < -0x8000) r = 0x8000u;
                    else r = (uint16_t)(int16_t)sum;
                    set_lane_h(&st->gpr[rd], n, r);
                }
                break;
            case 0x15: /* PSUBSH - saturated per-lane signed 16-bit
                        * subtract, ported from _PSUBSH(). */
                if (rd) for (int n = 0; n < 8; n++) {
                    int32_t diff = (int32_t)(int16_t)lane_h(st->gpr[rs], n) - (int32_t)(int16_t)lane_h(st->gpr[rt], n);
                    uint16_t r;
                    if (diff >= 0x7FFF) r = 0x7FFFu;
                    else if (diff < -0x8000) r = 0x8000u;
                    else r = (uint16_t)(int16_t)diff;
                    set_lane_h(&st->gpr[rd], n, r);
                }
                break;
            case 0x18: /* PADDSB - saturated per-lane signed 8-bit
                        * add, ported from _PADDSB(). */
                if (rd) for (int n = 0; n < 16; n++) {
                    int16_t sum = (int16_t)(int8_t)lane_b(st->gpr[rs], n) + (int16_t)(int8_t)lane_b(st->gpr[rt], n);
                    uint8_t r;
                    if (sum > 0x7F) r = 0x7Fu;
                    else if (sum < -128) r = 0x80u;
                    else r = (uint8_t)(int8_t)sum;
                    set_lane_b(&st->gpr[rd], n, r);
                }
                break;
            case 0x19: /* PSUBSB - saturated per-lane signed 8-bit
                        * subtract, ported from _PSUBSB(). */
                if (rd) for (int n = 0; n < 16; n++) {
                    int16_t diff = (int16_t)(int8_t)lane_b(st->gpr[rs], n) - (int16_t)(int8_t)lane_b(st->gpr[rt], n);
                    uint8_t r;
                    if (diff >= 0x7F) r = 0x7Fu;
                    else if (diff < -128) r = 0x80u;
                    else r = (uint8_t)(int8_t)diff;
                    set_lane_b(&st->gpr[rd], n, r);
                }
                break;
            case 0x1E: /* PEXT5 - unpacks a GS 16-bit 5551 pixel format
                        * (5 bits R, 5 bits G, 5 bits B, 1 bit A, packed
                        * in the low 16 bits of each 32-bit lane) up
                        * into a 32-bit lane with each channel
                        * left-aligned in its own byte (R in bits 3-7,
                        * G in bits 11-15, B in bits 19-23, A in bit
                        * 31) - ported exactly from PCSX2's _PEXT5().
                        * Uses only Rt; Rs is unused (matches real
                        * hardware/PCSX2 - this is a unary unpack, not
                        * a binary op). */
                if (rd) for (int n = 0; n < 4; n++) {
                    uint32_t v = lane_w(st->gpr[rt], n);
                    uint32_t r = ((v & 0x0000001Fu) << 3)  |
                                 ((v & 0x000003E0u) << 6)  |
                                 ((v & 0x00007C00u) << 9)  |
                                 ((v & 0x00008000u) << 16);
                    set_lane_w(&st->gpr[rd], n, r);
                }
                break;
            case 0x1F: /* PPAC5 - inverse of PEXT5: packs a 32-bit lane
                        * (as produced by PEXT5's layout) back down to
                        * a 16-bit 5551 pixel. Ported from _PPAC5().
                        * Also Rt-only, Rs unused. */
                if (rd) for (int n = 0; n < 4; n++) {
                    uint32_t v = lane_w(st->gpr[rt], n);
                    uint32_t r = ((v >> 3)  & 0x0000001Fu) |
                                 ((v >> 6)  & 0x000003E0u) |
                                 ((v >> 9)  & 0x00007C00u) |
                                 ((v >> 16) & 0x00008000u);
                    set_lane_w(&st->gpr[rd], n, r);
                }
                break;
            default:
                halt("unimplemented MMI0 sub-opcode");
                return 1;
            }
            break;

        case 0x28: /* MMI1 */
            switch (sa) {
            case 0x01: /* PABSW - per-lane 32-bit absolute value, ported
                        * from PCSX2's _PABSW(): real hardware quirk
                        * preserved exactly - INT32_MIN (0x80000000) has
                        * no positive 32-bit representation, so it's
                        * clamped to INT32_MAX (0x7FFFFFFF) instead of
                        * overflowing/wrapping. */
                if (rd) for (int n = 0; n < 4; n++) {
                    uint32_t v = lane_w(st->gpr[rt], n);
                    uint32_t r;
                    if (v == 0x80000000u) r = 0x7FFFFFFFu;
                    else if ((int32_t)v < 0) r = (uint32_t)(-(int32_t)v);
                    else r = v;
                    set_lane_w(&st->gpr[rd], n, r);
                }
                break;
            case 0x02: /* PCEQW - per-lane 32-bit equality compare (mask
                        * result), ported from _PCEQW(). */
                if (rd) for (int n = 0; n < 4; n++)
                    set_lane_w(&st->gpr[rd], n, (lane_w(st->gpr[rs], n) == lane_w(st->gpr[rt], n)) ? 0xFFFFFFFFu : 0x00000000u);
                break;
            case 0x03: /* PMINW - per-lane signed 32-bit min, ported from _PMINW(). */
                if (rd) for (int n = 0; n < 4; n++) {
                    int32_t a = (int32_t)lane_w(st->gpr[rs], n), b = (int32_t)lane_w(st->gpr[rt], n);
                    set_lane_w(&st->gpr[rd], n, (uint32_t)((a < b) ? a : b));
                }
                break;
            case 0x04: /* PADSBH - "add/subtract halfword", ported from
                        * PCSX2's PADSBH(): NOT a uniform op across all 8
                        * lanes - the low 4 lanes get PSUBH (fs-ft), the
                        * high 4 lanes get PADDH (fs+ft). A real,
                        * deliberately asymmetric instruction, not a typo. */
                if (rd) {
                    for (int n = 0; n < 4; n++) set_lane_h(&st->gpr[rd], n, (uint16_t)(lane_h(st->gpr[rs], n) - lane_h(st->gpr[rt], n)));
                    for (int n = 4; n < 8; n++) set_lane_h(&st->gpr[rd], n, (uint16_t)(lane_h(st->gpr[rs], n) + lane_h(st->gpr[rt], n)));
                }
                break;
            case 0x05: /* PABSH - per-lane 16-bit absolute value, same
                        * INT16_MIN-clamps-to-INT16_MAX quirk as PABSW,
                        * ported from _PABSH(). */
                if (rd) for (int n = 0; n < 8; n++) {
                    uint16_t v = lane_h(st->gpr[rt], n);
                    uint16_t r;
                    if (v == 0x8000u) r = 0x7FFFu;
                    else if ((int16_t)v < 0) r = (uint16_t)(-(int16_t)v);
                    else r = v;
                    set_lane_h(&st->gpr[rd], n, r);
                }
                break;
            case 0x06: /* PCEQH - per-lane 16-bit equality compare, ported from _PCEQH(). */
                if (rd) for (int n = 0; n < 8; n++)
                    set_lane_h(&st->gpr[rd], n, (lane_h(st->gpr[rs], n) == lane_h(st->gpr[rt], n)) ? 0xFFFFu : 0x0000u);
                break;
            case 0x07: /* PMINH - per-lane signed 16-bit min, ported from _PMINH(). */
                if (rd) for (int n = 0; n < 8; n++) {
                    int16_t a = (int16_t)lane_h(st->gpr[rs], n), b = (int16_t)lane_h(st->gpr[rt], n);
                    set_lane_h(&st->gpr[rd], n, (uint16_t)((a < b) ? a : b));
                }
                break;
            case 0x0A: /* PCEQB - per-lane 8-bit equality compare, ported from _PCEQB(). */
                if (rd) for (int n = 0; n < 16; n++)
                    set_lane_b(&st->gpr[rd], n, (lane_b(st->gpr[rs], n) == lane_b(st->gpr[rt], n)) ? 0xFFu : 0x00u);
                break;
            case 0x10: /* PADDUW */ if (rd) for (int n = 0; n < 4; n++) set_lane_w(&st->gpr[rd], n, lane_w(st->gpr[rs], n) + lane_w(st->gpr[rt], n)); break;
            case 0x11: /* PSUBUW */ if (rd) for (int n = 0; n < 4; n++) { uint32_t a = lane_w(st->gpr[rs], n), b = lane_w(st->gpr[rt], n); set_lane_w(&st->gpr[rd], n, (a > b) ? a - b : 0); } break;
            case 0x12: /* PEXTUW */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    set_lane_w(&out, 0, lane_w(Rt, 2)); set_lane_w(&out, 1, lane_w(Rs, 2));
                    set_lane_w(&out, 2, lane_w(Rt, 3)); set_lane_w(&out, 3, lane_w(Rs, 3));
                    st->gpr[rd] = out;
                }
                break;
            case 0x14: /* PADDUH */ if (rd) for (int n = 0; n < 8; n++) set_lane_h(&st->gpr[rd], n, (uint16_t)(lane_h(st->gpr[rs], n) + lane_h(st->gpr[rt], n))); break;
            case 0x15: /* PSUBUH */ if (rd) for (int n = 0; n < 8; n++) { uint16_t a = lane_h(st->gpr[rs], n), b = lane_h(st->gpr[rt], n); set_lane_h(&st->gpr[rd], n, (a > b) ? (uint16_t)(a - b) : 0); } break;
            case 0x16: /* PEXTUH */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    set_lane_h(&out, 0, lane_h(Rt, 4)); set_lane_h(&out, 1, lane_h(Rs, 4));
                    set_lane_h(&out, 2, lane_h(Rt, 5)); set_lane_h(&out, 3, lane_h(Rs, 5));
                    set_lane_h(&out, 4, lane_h(Rt, 6)); set_lane_h(&out, 5, lane_h(Rs, 6));
                    set_lane_h(&out, 6, lane_h(Rt, 7)); set_lane_h(&out, 7, lane_h(Rs, 7));
                    st->gpr[rd] = out;
                }
                break;
            case 0x18: /* PADDUB */ if (rd) for (int n = 0; n < 16; n++) set_lane_b(&st->gpr[rd], n, (uint8_t)(lane_b(st->gpr[rs], n) + lane_b(st->gpr[rt], n))); break;
            case 0x19: /* PSUBUB */ if (rd) for (int n = 0; n < 16; n++) { uint8_t a = lane_b(st->gpr[rs], n), b = lane_b(st->gpr[rt], n); set_lane_b(&st->gpr[rd], n, (a > b) ? (uint8_t)(a - b) : 0); } break;
            case 0x1A: /* PEXTUB */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    for (int n = 0; n < 8; n++) {
                        set_lane_b(&out, n * 2,     lane_b(Rt, n + 8));
                        set_lane_b(&out, n * 2 + 1, lane_b(Rs, n + 8));
                    }
                    st->gpr[rd] = out;
                }
                break;
            default:
                halt("unimplemented MMI1 sub-opcode (incl. QFSRV)");
                return 1;
            }
            break;

        case 0x09: /* MMI2 */
            switch (sa) {
            case 0x08: /* PMFHI */ if (rd) st->gpr[rd] = st->hi; break;
            case 0x09: /* PMFLO */ if (rd) st->gpr[rd] = st->lo; break;
            case 0x0E: /* PCPYLD */
                if (rd) {
                    ee_reg128_t out;
                    out.ud1 = st->gpr[rs].ud0;
                    out.ud0 = st->gpr[rt].ud0;
                    st->gpr[rd] = out;
                }
                break;
            case 0x12: /* PAND */ if (rd) { st->gpr[rd].ud0 = st->gpr[rs].ud0 & st->gpr[rt].ud0; st->gpr[rd].ud1 = st->gpr[rs].ud1 & st->gpr[rt].ud1; } break;
            case 0x13: /* PXOR */ if (rd) { st->gpr[rd].ud0 = st->gpr[rs].ud0 ^ st->gpr[rt].ud0; st->gpr[rd].ud1 = st->gpr[rs].ud1 ^ st->gpr[rt].ud1; } break;
            case 0x02: /* PSLLVW - variable logical-left-shift of two
                        * word lanes (Rt's lanes 0/2), each shifted by
                        * its OWN shift amount taken from the
                        * corresponding lane of Rs (lane 0's amount
                        * from Rs lane 0, lane 2's amount from Rs lane
                        * 2 - masked to 5 bits, standard MIPS variable-
                        * shift convention), sign-extended to 64 bits
                        * into gpr.ud0/ud1. Ported from PCSX2's
                        * PSLLVW(). */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt];
                    st->gpr[rd].ud0 = sext32(lane_w(Rt, 0) << (lane_w(Rs, 0) & 0x1F));
                    st->gpr[rd].ud1 = sext32(lane_w(Rt, 2) << (lane_w(Rs, 2) & 0x1F));
                }
                break;
            case 0x03: /* PSRLVW - same as PSLLVW but a variable
                        * LOGICAL right shift (not arithmetic), ported
                        * from PCSX2's PSRLVW(). */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt];
                    st->gpr[rd].ud0 = sext32(lane_w(Rt, 0) >> (lane_w(Rs, 0) & 0x1F));
                    st->gpr[rd].ud1 = sext32(lane_w(Rt, 2) >> (lane_w(Rs, 2) & 0x1F));
                }
                break;
            case 0x0A: /* PINTH - interleaves the low halfword lanes of
                        * Rt with the HIGH halfword lanes of Rs, ported
                        * from PCSX2's PINTH(). Note it's Rs's UPPER 4
                        * lanes (US[4..7]) that get interleaved in, not
                        * the lower ones - a real, easy-to-get-backwards
                        * detail worth preserving exactly. */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    set_lane_h(&out, 0, lane_h(Rt, 0)); set_lane_h(&out, 1, lane_h(Rs, 4));
                    set_lane_h(&out, 2, lane_h(Rt, 1)); set_lane_h(&out, 3, lane_h(Rs, 5));
                    set_lane_h(&out, 4, lane_h(Rt, 2)); set_lane_h(&out, 5, lane_h(Rs, 6));
                    set_lane_h(&out, 6, lane_h(Rt, 3)); set_lane_h(&out, 7, lane_h(Rs, 7));
                    st->gpr[rd] = out;
                }
                break;
            case 0x1A: /* PEXEH - swaps halfword lanes 0 and 2 within
                        * each 64-bit half (Rt only; Rs unused), ported
                        * from PEXEH(). */
                if (rd) {
                    ee_reg128_t Rt = st->gpr[rt], out;
                    set_lane_h(&out, 0, lane_h(Rt, 2)); set_lane_h(&out, 1, lane_h(Rt, 1));
                    set_lane_h(&out, 2, lane_h(Rt, 0)); set_lane_h(&out, 3, lane_h(Rt, 3));
                    set_lane_h(&out, 4, lane_h(Rt, 6)); set_lane_h(&out, 5, lane_h(Rt, 5));
                    set_lane_h(&out, 6, lane_h(Rt, 4)); set_lane_h(&out, 7, lane_h(Rt, 7));
                    st->gpr[rd] = out;
                }
                break;
            case 0x1B: /* PREVH - fully reverses the halfword lanes
                        * within each 64-bit half (Rt only), ported
                        * from PREVH(). */
                if (rd) {
                    ee_reg128_t Rt = st->gpr[rt], out;
                    set_lane_h(&out, 0, lane_h(Rt, 3)); set_lane_h(&out, 1, lane_h(Rt, 2));
                    set_lane_h(&out, 2, lane_h(Rt, 1)); set_lane_h(&out, 3, lane_h(Rt, 0));
                    set_lane_h(&out, 4, lane_h(Rt, 7)); set_lane_h(&out, 5, lane_h(Rt, 6));
                    set_lane_h(&out, 6, lane_h(Rt, 5)); set_lane_h(&out, 7, lane_h(Rt, 4));
                    st->gpr[rd] = out;
                }
                break;
            case 0x1E: /* PEXEW - swaps word lanes 0 and 2 (Rt only),
                        * ported from PEXEW(). */
                if (rd) {
                    ee_reg128_t Rt = st->gpr[rt], out;
                    set_lane_w(&out, 0, lane_w(Rt, 2)); set_lane_w(&out, 1, lane_w(Rt, 1));
                    set_lane_w(&out, 2, lane_w(Rt, 0)); set_lane_w(&out, 3, lane_w(Rt, 3));
                    st->gpr[rd] = out;
                }
                break;
            case 0x1F: /* PROT3W - rotates word lanes 0,1,2 left by one
                        * (lane 3 untouched), Rt only. Ported from
                        * PROT3W(). */
                if (rd) {
                    ee_reg128_t Rt = st->gpr[rt], out;
                    set_lane_w(&out, 0, lane_w(Rt, 1)); set_lane_w(&out, 1, lane_w(Rt, 2));
                    set_lane_w(&out, 2, lane_w(Rt, 0)); set_lane_w(&out, 3, lane_w(Rt, 3));
                    st->gpr[rd] = out;
                }
                break;
            default:
                halt("unimplemented MMI2 sub-opcode");
                return 1;
            }
            break;

        case 0x29: /* MMI3 */
            switch (sa) {
            case 0x08: /* PMTHI */ st->hi = st->gpr[rs]; break;
            case 0x09: /* PMTLO */ st->lo = st->gpr[rs]; break;
            case 0x0E: /* PCPYUD */
                if (rd) {
                    ee_reg128_t out;
                    out.ud0 = st->gpr[rs].ud1;
                    out.ud1 = st->gpr[rt].ud1;
                    st->gpr[rd] = out;
                }
                break;
            case 0x12: /* POR */  if (rd) { st->gpr[rd].ud0 = st->gpr[rs].ud0 | st->gpr[rt].ud0; st->gpr[rd].ud1 = st->gpr[rs].ud1 | st->gpr[rt].ud1; } break;
            case 0x13: /* PNOR */ if (rd) { st->gpr[rd].ud0 = ~(st->gpr[rs].ud0 | st->gpr[rt].ud0); st->gpr[rd].ud1 = ~(st->gpr[rs].ud1 | st->gpr[rt].ud1); } break;
            case 0x0A: /* PINTEH - interleaves EVEN halfword lanes of
                        * Rt with EVEN halfword lanes of Rs (odd lanes
                        * untouched by either input), ported from
                        * PINTEH(). Distinct from PINTH above: PINTH
                        * takes ALL of Rt's lanes plus Rs's upper half;
                        * PINTEH takes only the even-indexed lanes of
                        * BOTH Rs and Rt. */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    set_lane_h(&out, 0, lane_h(Rt, 0)); set_lane_h(&out, 1, lane_h(Rs, 0));
                    set_lane_h(&out, 2, lane_h(Rt, 2)); set_lane_h(&out, 3, lane_h(Rs, 2));
                    set_lane_h(&out, 4, lane_h(Rt, 4)); set_lane_h(&out, 5, lane_h(Rs, 4));
                    set_lane_h(&out, 6, lane_h(Rt, 6)); set_lane_h(&out, 7, lane_h(Rs, 6));
                    st->gpr[rd] = out;
                }
                break;
            case 0x1A: /* PEXCH - swaps halfword lanes 1 and 2 within
                        * each 64-bit half (Rt only), ported from
                        * PEXCH(). Note this is a DIFFERENT permutation
                        * from PEXEH above (which swaps lanes 0 and 2) -
                        * easy to confuse, kept as two distinct case
                        * bodies rather than merged to avoid mixing up
                        * which lane pair each one swaps. */
                if (rd) {
                    ee_reg128_t Rt = st->gpr[rt], out;
                    set_lane_h(&out, 0, lane_h(Rt, 0)); set_lane_h(&out, 1, lane_h(Rt, 2));
                    set_lane_h(&out, 2, lane_h(Rt, 1)); set_lane_h(&out, 3, lane_h(Rt, 3));
                    set_lane_h(&out, 4, lane_h(Rt, 4)); set_lane_h(&out, 5, lane_h(Rt, 6));
                    set_lane_h(&out, 6, lane_h(Rt, 5)); set_lane_h(&out, 7, lane_h(Rt, 7));
                    st->gpr[rd] = out;
                }
                break;
            case 0x1B: /* PCPYH - broadcasts halfword lane 0 across the
                        * low 64 bits and lane 4 across the high 64
                        * bits (Rt only), ported from PCPYH(). */
                if (rd) {
                    ee_reg128_t Rt = st->gpr[rt], out;
                    uint16_t lo = lane_h(Rt, 0), hi = lane_h(Rt, 4);
                    for (int n = 0; n < 4; n++) set_lane_h(&out, n, lo);
                    for (int n = 4; n < 8; n++) set_lane_h(&out, n, hi);
                    st->gpr[rd] = out;
                }
                break;
            case 0x1E: /* PEXCW - swaps word lanes 1 and 2 (Rt only),
                        * ported from PEXCW(). Distinct from PEXEW
                        * above (which swaps lanes 0 and 2). */
                if (rd) {
                    ee_reg128_t Rt = st->gpr[rt], out;
                    set_lane_w(&out, 0, lane_w(Rt, 0)); set_lane_w(&out, 1, lane_w(Rt, 2));
                    set_lane_w(&out, 2, lane_w(Rt, 1)); set_lane_w(&out, 3, lane_w(Rt, 3));
                    st->gpr[rd] = out;
                }
                break;
            default:
                halt("unimplemented MMI3 sub-opcode");
                return 1;
            }
            break;

        default:
            halt("unimplemented MMI top-level funct");
            return 1;
        }
        break;

    case 0x22: /* LWL */ {
        static const uint32_t LWL_MASK[4]  = { 0xffffffu, 0x0000ffffu, 0x000000ffu, 0x00000000u };
        static const uint8_t  LWL_SHIFT[4] = { 24, 16, 8, 0 };
        uint32_t addr = rs32 + imm;
        uint32_t shift = addr & 3;
        uint32_t mem = ee_mem_read32(st, addr & ~3u);
        if (rt) GPR(rt) = sext32((rt32 & LWL_MASK[shift]) | (mem << LWL_SHIFT[shift]));
    } break;
    case 0x26: /* LWR */ {
        static const uint32_t LWR_MASK[4]  = { 0x00000000u, 0xff000000u, 0xffff0000u, 0xffffff00u };
        static const uint8_t  LWR_SHIFT[4] = { 0, 8, 16, 24 };
        uint32_t addr = rs32 + imm;
        uint32_t shift = addr & 3;
        uint32_t mem = ee_mem_read32(st, addr & ~3u);
        uint32_t result = (rt32 & LWR_MASK[shift]) | (mem >> LWR_SHIFT[shift]);
        if (rt) {
            if (shift == 0)
                GPR(rt) = sext32(result); /* full 64-bit sign extend, matches PCSX2 */
            else
                GPR(rt) = (GPR(rt) & 0xFFFFFFFF00000000ULL) | result; /* upper 32 bits preserved */
        }
    } break;

    case 0x20: /* LB */  if (rt) GPR(rt) = (uint64_t)(int64_t)(int8_t)ee_mem_read8(st, rs32 + imm); else ee_mem_read8(st, rs32 + imm); break;
    case 0x21: /* LH */  if (rt) GPR(rt) = (uint64_t)(int64_t)(int16_t)ee_mem_read16(st, rs32 + imm); else ee_mem_read16(st, rs32 + imm); break;
    case 0x23: /* LW */  if (rt) GPR(rt) = sext32(ee_mem_read32(st, rs32 + imm)); else ee_mem_read32(st, rs32 + imm); break;
    case 0x24: /* LBU */ if (rt) GPR(rt) = ee_mem_read8(st, rs32 + imm); else ee_mem_read8(st, rs32 + imm); break;
    case 0x25: /* LHU */ if (rt) GPR(rt) = ee_mem_read16(st, rs32 + imm); else ee_mem_read16(st, rs32 + imm); break;
    case 0x27: /* LWU */ if (rt) GPR(rt) = ee_mem_read32(st, rs32 + imm); else ee_mem_read32(st, rs32 + imm); break;
    case 0x37: /* LD */  if (rt) GPR(rt) = ee_mem_read64(st, rs32 + imm); else ee_mem_read64(st, rs32 + imm); break;

    case 0x2A: /* SWL */ {
        static const uint32_t SWL_MASK[4]  = { 0xffffff00u, 0xffff0000u, 0xff000000u, 0x00000000u };
        static const uint8_t  SWL_SHIFT[4] = { 24, 16, 8, 0 };
        uint32_t addr = rs32 + imm;
        uint32_t shift = addr & 3;
        uint32_t mem = ee_mem_read32(st, addr & ~3u);
        ee_mem_write32(st, addr & ~3u, (rt32 >> SWL_SHIFT[shift]) | (mem & SWL_MASK[shift]));
    } break;
    case 0x2E: /* SWR */ {
        static const uint32_t SWR_MASK[4]  = { 0x00000000u, 0x000000ffu, 0x0000ffffu, 0x00ffffffu };
        static const uint8_t  SWR_SHIFT[4] = { 0, 8, 16, 24 };
        uint32_t addr = rs32 + imm;
        uint32_t shift = addr & 3;
        uint32_t mem = ee_mem_read32(st, addr & ~3u);
        ee_mem_write32(st, addr & ~3u, (rt32 << SWR_SHIFT[shift]) | (mem & SWR_MASK[shift]));
    } break;

    case 0x28: /* SB */ ee_mem_write8(st, rs32 + imm, (uint8_t)GPR(rt)); break;
    case 0x29: /* SH */ ee_mem_write16(st, rs32 + imm, (uint16_t)GPR(rt)); break;
    case 0x2B: /* SW */ ee_mem_write32(st, rs32 + imm, (uint32_t)GPR(rt)); break;
    case 0x2F: /* CACHE */ break; /* no-op: no cache model */
    case 0x33: /* PREF */  break; /* no-op: prefetch hint */
    case 0x3F: /* SD */ ee_mem_write64(st, rs32 + imm, GPR(rt)); break;

    /* LWC1/SWC1 - direct FPR<->memory word transfer (as opposed to
     * MFC1/MTC1, which move a raw 32-bit value between an FPR and a
     * GPR). Found missing once real BIOS boot got far enough to need
     * it (see docs/STATUS.md's "round 5"/COP0 PRId fix) - halted
     * cleanly on "unimplemented primary opcode 0x39" (SWC1). Standard
     * MIPS I FPU load/store, `rt` selects the FPR (not a GPR) here. */
    case 0x31: /* LWC1 */ st->fpr[rt] = ee_mem_read32(st, rs32 + imm); break;
    case 0x39: /* SWC1 */ ee_mem_write32(st, rs32 + imm, st->fpr[rt]); break;

    case 0x1E: /* LQ - 128-bit load, ported from PCSX2's R5900OpcodeImpl.cpp.
                * Address is masked to 16-byte alignment (real hardware
                * ignores the low 4 bits rather than faulting on
                * unaligned access, unlike LW/LD). Matches PCSX2's own
                * interpreter exactly in skipping the read entirely
                * when rt==$0 (unlike LW/LH/etc elsewhere in this
                * file, which still perform the read for its memory
                * side effects even when the destination is
                * discarded) - LQ has no such side-effect-only path in
                * real PCSX2, so neither does this. */
        if (rt) {
            uint32_t addr = (rs32 + imm) & ~0xFu;
            GPR(rt)  = ee_mem_read64(st, addr);
            GPR1(rt) = ee_mem_read64(st, addr + 8);
        }
        break;
    case 0x1F: /* SQ - 128-bit store, ported from PCSX2's R5900OpcodeImpl.cpp.
                * Same 16-byte alignment masking as LQ. Always writes
                * both halves, including when rt==$0 (whose value is
                * always zero) - matches real hardware/PCSX2, no
                * special-case needed. */
    {
        uint32_t addr = (rs32 + imm) & ~0xFu;
        ee_mem_write64(st, addr,     GPR(rt));
        ee_mem_write64(st, addr + 8, GPR1(rt));
    } break;

    default:
    {
        /* NOTE: this halt point can be reached in a way that has
         * nothing to do with the actual opcode value - see the
         * real-BIOS-testing note in docs/STATUS.md ("EE program
         * counter escapes into the hardware register window" /
         * out-of-range JALR target investigation). If `op` here is
         * 0 (SPECIAL) with funct also reading as 0, that's the
         * signature of this exact scenario: PC drifted through a
         * long stretch of unpopulated ("reads as zero") memory,
         * decoding every word as a harmless SLL/NOP, until it
         * happened to reach a live hardware register address whose
         * CURRENT value decoded as something genuinely invalid. */
        char buf[96];
        snprintf(buf, sizeof(buf), "unimplemented primary opcode 0x%02X (pc=0x%08X)",
                 (unsigned int)op, (unsigned int)this_pc);
        halt(buf);
        return 1;
    }
    }

#undef GPR
#undef GPR1
#undef BRANCH_TO
#undef LINK

    st->gpr[0].ud0 = 0;
    st->gpr[0].ud1 = 0;
    st->instructions_executed++;

    /* COP0 Count (register 9): a real, free-running counter compared
     * against Compare (register 11) by real hardware/BIOS delay loops
     * (a classic "MFC0 Count; SUBU; SLTU; BNE" busy-wait, e.g. the one
     * found at pc=0x9FC42500 in the real SCPH-10000 BIOS - see
     * docs/STATUS.md's "round 8"). Before this, Count never advanced
     * at all (only ever written via explicit MTC0), so any such delay
     * loop ran forever - not a translation/exception bug, just a
     * missing free-running counter. Real PCSX2 advances Count lazily
     * by however many bus cycles (cpuRegs.cycle) elapsed since the
     * last read (COP0.cpp's MFC0 case 9); this project has no cycle-
     * accurate timing model at all, so it advances Count by a fixed 1
     * per instruction instead - a real, working free-running counter
     * (monotonic, comparable against Compare, exactly the documented
     * COP0 Count/Compare mechanism), just without precise bus-clock-
     * rate fidelity, which isn't verifiable without a real timing
     * model and isn't needed just to let a delay loop terminate. */
    st->cop0[9]++;

    /* Latch unconditionally - see ee_latch_timer_interrupt()'s comment
     * for why this can't be skipped even mid-delay-slot. Only actually
     * TAKING the (possibly already-latched) interrupt is deferred to a
     * genuine instruction boundary: st->branch_pending here reflects
     * whether the NEXT instruction (whatever this step just set
     * st->pc to) is itself a delay slot. */
    ee_latch_timer_interrupt(st);
    if (!st->branch_pending)
        ee_check_timer_interrupt(st, st->pc);

    return 0;
}

/* Public single-instruction step, for callers (currently
 * source/core/system.c's interleaved EE/IOP scheduler) that need to
 * interleave execution with another core rather than run this core
 * to completion in isolation. Returns the same value as the internal
 * ee_step(): 0 to keep going, 1 if this step halted the core. Safe to
 * keep calling after a halt (ee_step() re-checks st->halted itself
 * via the same path ee_core_run()'s loop uses). */
int ee_core_step(void)
{
    if (g_state.halted)
        return 1;
    return ee_step();
}

void ee_core_run(const bios_image_t *bios)
{
    (void)bios;
    const uint64_t step_report_interval = 10000;

    while (!g_state.halted) {
        if (ee_step())
            break;

        if ((g_state.instructions_executed % step_report_interval) == 0) {
            printf("  ... %llu instructions executed, pc=0x%08lX\n",
                   (unsigned long long)g_state.instructions_executed, (unsigned long)g_state.pc);
        }
    }

    printf("\n[!] EE core halted after %llu instructions at pc=0x%08lX\n",
           (unsigned long long)g_state.instructions_executed, (unsigned long)g_state.pc);
    printf("    reason: %s\n", g_state.halt_reason[0] ? g_state.halt_reason : "(unknown)");
}

void ee_core_shutdown(void)
{
    if (g_state.ram) {
        free(g_state.ram);
        g_state.ram = NULL;
    }
}
