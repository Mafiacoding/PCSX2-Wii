/*
 * test_ee_timer_interrupt.c - host-native test for ee_core.c's real
 * EE Timer (Count==Compare) interrupt delivery: ee_latch_timer_interrupt()
 * / ee_check_timer_interrupt(), ported from PCSX2's _cpuTestTIMR()/
 * cpuTestTIMRInts() in R5900.cpp.
 *
 * This is the direct continuation of "EE JALR investigation round 8"
 * (test_ee_scratchpad_count.c): once the Scratchpad RAM + COP0 Count
 * fixes let the real SCPH-10000 BIOS boot run 800M+ instructions with
 * zero exceptions, it reached pc=0xBFC0092C - a real "j $" self-loop
 * right after a Compare=1 timer setup, a genuine "wait for interrupt"
 * idle pattern this project had never implemented any interrupt
 * delivery for at all. This file tests that new delivery path. See
 * docs/STATUS.md's "round 9" section for the full context.
 *
 * IMPORTANT ordering note that shaped every test below: COP0's Count
 * and Compare registers both reset to 0 (plain memset in
 * ee_core_init()), so Count>=Compare is trivially true from the very
 * first instruction any program executes, before any deliberate
 * Count/Compare setup. This is harmless on real hardware (and here)
 * as long as Status.IE stays 0 until software has explicitly armed
 * Compare first (which also acks/clears any such stale latch via the
 * Compare-write side effect - see ee_core.c's MTC0 case for register
 * 11) - exactly the order the real SCPH-10000 BIOS itself uses at
 * pc=0xBFC00814-0xBFC00824 (Status written with IE still 0, then
 * Count reset, then Compare armed - only enabling interrupts later).
 * Every test program below deliberately arms Compare BEFORE enabling
 * Status.IE for this same reason - enabling interrupts first would
 * spuriously race the reset-time Count==Compare==0 collision (note
 * this reset-time collision can still latch Cause.IP7 briefly on its
 * own before Compare is genuinely armed - harmless on its own since
 * Status.IE is still 0 then, and the eventual real Compare write acks
 * it regardless of whether it fires "for real" or "spuriously" first
 * - see test_timer_overshoot_still_latches() for the concrete case).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include "core/ee/ee_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t enc_lui(int rt, uint16_t imm)          { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm)   { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_addiu(int rt, int rs, int16_t imm)  { return (0x09 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_mtc0(int rt, int rd)                { return (0x10 << 26) | (0x04 << 21) | (rt << 16) | (rd << 11); }
static uint32_t enc_mfc0(int rt, int rd)                { return (0x10 << 26) | (0x00 << 21) | (rt << 16) | (rd << 11); }
static uint32_t enc_beq(int rs, int rt, int16_t imm)    { return (0x04 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_nop(void)   { return 0; }
static uint32_t enc_break(void) { return 0x0D; }
static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

static bios_image_t make_bios(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;
    return bios;
}

/* Status value with IE=1, EIE=1, IM7=1, EXL=0, ERL=0 - the real,
 * documented gating cpuTestTIMRInts()/_cpuTestTIMR() require before a
 * Count==Compare match is actually taken as an interrupt. BEV (bit
 * 22) is deliberately kept set here (added on top of the three low
 * bits below) purely so the exception vectors into the boot-time ROM
 * range like every other test in this project's suite defaults to
 * (test_bev_vectoring() in test_ee_exceptions.c is the one test that
 * deliberately clears it) - MTC0 on this register is a plain
 * overwrite (see ee_core.c's MTC0 case), so any bit not included here
 * would otherwise get cleared as a side effect of this write, same as
 * a real kernel has to explicitly preserve bits it cares about. */
#define STATUS_TIMER_ENABLED 0x00418001u

/* Chosen comfortably larger than the number of setup instructions any
 * test below executes before deliberately poking Count - so Count
 * can never accidentally reach this value on its own during setup
 * (each setup instruction also increments Count by 1, same as real
 * execution - see ee_step()'s epilogue). */
#define TEST_COMPARE_VALUE 1000u

/* --- 1. Basic fire: Count reaching Compare, with every gating bit
 * enabled, must actually take a real Interrupt exception (ExcCode 0)
 * - Cause.IP7 set, Status.EXL set, EPC pointing at the next
 * not-yet-executed instruction, Cause.BD clear (not a delay slot). */
static void test_timer_fires_on_match(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;

    /* Arm Compare FIRST (see the file header comment for why), then
     * enable Status - LUI/ORI to synthesize the full 32-bit Status
     * value (0x00418001 doesn't fit in ORI's/ADDIU's 16-bit immediate
     * alone). Then two NOPs (the second one is the instruction whose
     * step should trigger the match, once we've poked Count directly
     * to Compare-1). */
    wle32(p+pc, enc_addiu(2, 0, (int16_t)TEST_COMPARE_VALUE)); pc += 4; /* $2 = 1000 */
    wle32(p+pc, enc_mtc0(2, 11));               pc += 4; /* Compare = 1000 (also acks any stale reset-time latch) */
    wle32(p+pc, enc_lui(1, 0x0041));            pc += 4; /* $1 = 0x00410000 */
    wle32(p+pc, enc_ori(1, 1, 0x8001));         pc += 4; /* $1 = 0x00418001 (BEV|IE|EIE|IM7) */
    wle32(p+pc, enc_mtc0(1, 12));               pc += 4; /* Status = STATUS_TIMER_ENABLED */
    uint32_t nop1_pc = pc;
    wle32(p+pc, enc_nop());                     pc += 4; /* nop1: the step where Count reaches Compare */
    uint32_t nop2_pc = pc;
    wle32(p+pc, enc_nop());                     pc += 4; /* nop2: would run next if no interrupt fired */
    wle32(p+pc, enc_break());                   pc += 4;

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    uint32_t base_pc = st->pc;

    ee_core_step(); /* ADDIU $2,1000 */
    ee_core_step(); /* MTC0 Compare=1000 */
    ee_core_step(); /* LUI */
    ee_core_step(); /* ORI */
    ee_core_step(); /* MTC0 Status */
    CHECK(st->cop0[12] == STATUS_TIMER_ENABLED, "Timer fire: Status == BEV|IE|EIE|IM7 after setup");
    CHECK(st->cop0[11] == TEST_COMPARE_VALUE, "Timer fire: Compare == 1000 after setup");
    CHECK(st->cop0[9] == 5, "Timer fire: Count == 5 after 5 setup instructions, nowhere near Compare yet");
    CHECK((st->cop0[12] & 0x2u) == 0, "Timer fire: Status.EXL still 0 - setup itself didn't spuriously trigger anything");

    /* Poke Count directly to one less than Compare, exactly like other
     * tests poke TLB/COP0 state directly (see test_ee_cop0_tlb.c) -
     * far simpler and more precise than executing 995 more real
     * instructions to land Count on the match "naturally". */
    st->cop0[9] = TEST_COMPARE_VALUE - 1;
    CHECK(st->pc == base_pc + nop1_pc, "Timer fire: about to execute nop1 with Count==Compare-1");

    ee_core_step(); /* nop1: Count becomes 1000 == Compare -> latched AND taken (not in a delay slot) */

    CHECK(st->cop0[9] == TEST_COMPARE_VALUE, "Timer fire: Count advanced to the match value during nop1's step");
    CHECK((st->cop0[13] & 0x00008000u) != 0, "Timer fire: Cause.IP7 latched");
    CHECK((st->cop0[13] & 0x7Cu) == 0, "Timer fire: Cause.ExcCode == Int (0)");
    CHECK((st->cop0[13] & 0x80000000u) == 0, "Timer fire: Cause.BD == 0 (not a delay slot)");
    CHECK((st->cop0[12] & 0x2u) != 0, "Timer fire: Status.EXL == 1 (now inside the handler)");
    CHECK(st->cop0[14] == base_pc + nop2_pc, "Timer fire: EPC points at nop2 (the next not-yet-executed instruction)");
    CHECK(st->pc == 0xBFC00200u + 0x200u, "Timer fire: pc vectored to the Interrupt vector (BEV kept set, ROM base)");
}

/* --- 2. Gating: IE=0 must prevent the interrupt from being taken even
 * though Count reaches Compare and latches Cause.IP7 - execution must
 * simply fall through to the next instruction as normal. */
static void test_timer_gated_by_ie(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_addiu(2, 0, (int16_t)TEST_COMPARE_VALUE)); pc += 4;
    wle32(p+pc, enc_mtc0(2, 11));               pc += 4; /* Compare = 1000 */
    wle32(p+pc, enc_lui(1, 0x0000));            pc += 4;
    wle32(p+pc, enc_ori(1, 1, 0x8000));         pc += 4; /* $1 = 0x8000: EIE|IM7 set, but IE (bit0) NOT set */
    wle32(p+pc, enc_mtc0(1, 12));               pc += 4;
    uint32_t nop1_pc = pc;
    wle32(p+pc, enc_nop());                     pc += 4;
    uint32_t nop2_pc = pc;
    wle32(p+pc, enc_nop());                     pc += 4;
    wle32(p+pc, enc_break());                   pc += 4;

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    uint32_t base_pc = st->pc;
    ee_core_step(); ee_core_step(); ee_core_step(); ee_core_step(); ee_core_step();
    CHECK((st->cop0[12] & 0x1u) == 0, "IE gating: Status.IE == 0 after setup (not fixed by ORI's 0x8000)");

    st->cop0[9] = TEST_COMPARE_VALUE - 1;
    ee_core_step(); /* nop1: Count reaches Compare, latches Cause.IP7, but IE=0 blocks TAKING it */

    CHECK((st->cop0[13] & 0x00008000u) != 0, "IE gating: Cause.IP7 still latches even though IE=0");
    CHECK((st->cop0[12] & 0x2u) == 0, "IE gating: Status.EXL still 0 (interrupt was NOT taken)");
    CHECK(st->pc == base_pc + nop2_pc, "IE gating: pc simply fell through to nop2, no vectoring");
}

/* --- 3. Gating: IM7=0 (this specific interrupt line masked) must also
 * prevent the interrupt from being taken, even with IE/EIE both set. */
static void test_timer_gated_by_im7(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_addiu(2, 0, (int16_t)TEST_COMPARE_VALUE)); pc += 4;
    wle32(p+pc, enc_mtc0(2, 11));               pc += 4;
    wle32(p+pc, enc_lui(1, 0x0001));            pc += 4;
    wle32(p+pc, enc_ori(1, 1, 0x0001));         pc += 4; /* $1 = 0x00010001: IE|EIE set, IM7 NOT set */
    wle32(p+pc, enc_mtc0(1, 12));               pc += 4;
    uint32_t nop2_pc_marker = pc + 4;
    wle32(p+pc, enc_nop());                     pc += 4;
    wle32(p+pc, enc_nop());                     pc += 4;
    wle32(p+pc, enc_break());                   pc += 4;

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    uint32_t base_pc = st->pc;
    ee_core_step(); ee_core_step(); ee_core_step(); ee_core_step(); ee_core_step();
    st->cop0[9] = TEST_COMPARE_VALUE - 1;
    ee_core_step();

    CHECK((st->cop0[12] & 0x8000u) == 0, "IM7 gating: Status.IM7 == 0 after setup");
    CHECK((st->cop0[13] & 0x00008000u) != 0, "IM7 gating: Cause.IP7 still latches regardless of the mask");
    CHECK((st->cop0[12] & 0x2u) == 0, "IM7 gating: Status.EXL still 0 (interrupt masked off, not taken)");
    CHECK(st->pc == base_pc + nop2_pc_marker, "IM7 gating: pc fell through normally");
}

/* --- 4. Deferred across a branch delay slot: if Count reaches Compare
 * during the step that executes a taken branch itself, the interrupt
 * must NOT be taken right then (real hardware has no pipeline
 * checkpoint between a branch and its delay slot) - it must instead
 * be taken right after the delay slot instruction also executes, with
 * EPC pointing at the branch's target (the next not-yet-executed
 * instruction at that point), not at the delay slot or the branch. */
static void test_timer_deferred_across_delay_slot(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_addiu(2, 0, (int16_t)TEST_COMPARE_VALUE)); pc += 4;
    wle32(p+pc, enc_mtc0(2, 11));               pc += 4; /* Compare = 1000 */
    wle32(p+pc, enc_lui(1, 0x0001));            pc += 4;
    wle32(p+pc, enc_ori(1, 1, 0x8001));         pc += 4; /* IE|EIE|IM7 */
    wle32(p+pc, enc_mtc0(1, 12));               pc += 4;
    uint32_t beq_pc = pc;
    wle32(p+pc, enc_beq(0, 0, 1));               pc += 4; /* always taken; target = beq_pc+4+(1<<2) */
    wle32(p+pc, enc_nop());                      pc += 4; /* delay slot */
    uint32_t target_pc = pc;
    wle32(p+pc, enc_break());                    pc += 4; /* branch target */

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    uint32_t base_pc = st->pc;
    ee_core_step(); ee_core_step(); ee_core_step(); ee_core_step(); ee_core_step();

    st->cop0[9] = TEST_COMPARE_VALUE - 1;
    CHECK(st->pc == base_pc + beq_pc, "Delay-slot deferral: about to execute the BEQ with Count==Compare-1");

    ee_core_step(); /* BEQ (taken): Count reaches Compare here, but branch_pending becomes 1 for the delay slot - must NOT take yet */
    CHECK(st->cop0[9] == TEST_COMPARE_VALUE, "Delay-slot deferral: Count reached the match value during the BEQ's own step");
    CHECK((st->cop0[13] & 0x00008000u) != 0, "Delay-slot deferral: Cause.IP7 already latched after the BEQ step");
    CHECK((st->cop0[12] & 0x2u) == 0, "Delay-slot deferral: Status.EXL still 0 - NOT taken during the BEQ's own step");
    CHECK(st->pc == base_pc + beq_pc + 4, "Delay-slot deferral: pc still just the delay slot (normal branch flow, no vectoring yet)");

    ee_core_step(); /* delay-slot NOP: now a safe boundary - the already-latched interrupt gets taken */
    CHECK((st->cop0[12] & 0x2u) != 0, "Delay-slot deferral: Status.EXL == 1 now (taken right after the delay slot)");
    CHECK(st->cop0[14] == base_pc + target_pc, "Delay-slot deferral: EPC == the branch's target (next not-yet-executed instr), not the BEQ or the delay slot");
    CHECK((st->cop0[13] & 0x80000000u) == 0, "Delay-slot deferral: Cause.BD == 0 (the interrupt itself isn't 'in' a delay slot)");
}

/* --- 5. Compare ack: writing a new value to Compare must clear the
 * latched Cause.IP7 pending bit - the real, documented MIPS mechanism
 * a handler uses to re-arm the timer for its next tick, tested here
 * directly via MTC0 rather than through a full handler/ERET sequence
 * (which test_ee_cop0_special.c already covers generically). */
static void test_compare_write_clears_pending(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_addiu(3, 0, 9));  pc += 4; /* $3 = 9 (new Compare value) */
    wle32(p+pc, enc_mtc0(3, 11));     pc += 4; /* MTC0 Compare, 9 -> should clear Cause.IP7 */
    wle32(p+pc, enc_break());         pc += 4;

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();

    /* Manually latch it first, exactly as ee_latch_timer_interrupt()
     * would after a real Count==Compare match, without needing a
     * second full setup program. */
    st->cop0[13] |= 0x00008000u;
    CHECK((st->cop0[13] & 0x00008000u) != 0, "Compare ack: Cause.IP7 manually latched before the write");

    ee_core_step(); /* ADDIU */
    ee_core_step(); /* MTC0 Compare=9 */

    CHECK(st->cop0[11] == 9, "Compare ack: Compare register itself updated to the new value (9)");
    CHECK((st->cop0[13] & 0x00008000u) == 0, "Compare ack: Cause.IP7 cleared by the Compare write");
}

/* --- 6. Overshoot still latches: the exact real SCPH-10000 BIOS
 * pattern that revealed exact-equality Count==Compare was NOT
 * sufficient (see ee_latch_timer_interrupt()'s comment in ee_core.c
 * for the full story) - real boot code at pc=0xBFC00814-0xBFC00824
 * does "MTC0 Status,<IE=0>" then "MTC0 Count,0" then, two
 * instructions later, "MTC0 Compare,1". Because Count already
 * advances past 1 (to 2, then 3) during those same few instructions'
 * own epilogues, the match must still latch via Count>=Compare, not
 * be silently missed forever because Count already overshot the
 * exact value 1 before Compare was even written. This test mirrors
 * that exact real instruction sequence (same registers: $k0/26, same
 * MTC0 register numbers), then separately enables Status afterward to
 * observe the already-latched interrupt actually get taken once
 * gating finally allows it - proving the latch really did survive the
 * overshoot instead of being lost. */
static void test_timer_overshoot_still_latches(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_mtc0(0, 12));                pc += 4; /* Status = 0 (IE=0) - matches real pc=0xBFC00814's intent (IE stays 0 at this point) */
    wle32(p+pc, enc_mtc0(0, 9));                 pc += 4; /* MTC0 $0, Count - matches real pc=0xBFC0081C exactly (rt field = $0/zero) */
    wle32(p+pc, enc_addiu(26, 0, 1));            pc += 4; /* $k0(26) = 1 - matches real pc=0xBFC00820's "ADDIU k0,zero,1" */
    wle32(p+pc, enc_mtc0(26, 11));               pc += 4; /* MTC0 k0, Compare - matches real pc=0xBFC00824 exactly */
    wle32(p+pc, enc_lui(1, 0x0041));             pc += 4; /* now enable Status, separately/later, to observe the take */
    wle32(p+pc, enc_ori(1, 1, 0x8001));          pc += 4;
    uint32_t status_enable_pc = pc;
    wle32(p+pc, enc_mtc0(1, 12));                pc += 4; /* Status = BEV|IE|EIE|IM7 */
    uint32_t after_enable_pc = pc;
    wle32(p+pc, enc_break());                    pc += 4;

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    uint32_t base_pc = st->pc;

    ee_core_step(); /* MTC0 Status=0 -> Count 0->1 */
    ee_core_step(); /* MTC0 Count=0 -> body resets Count to 0, then this same step's epilogue increments it back to 1 */
    CHECK(st->cop0[9] == 1, "Overshoot: Count == 1 right after MTC0 Count,0 (its own epilogue already incremented it)");

    ee_core_step(); /* ADDIU k0,1 -> Count becomes 2 */
    CHECK(st->cop0[9] == 2, "Overshoot: Count == 2 after the ADDIU in between");
    /* NOTE: Cause.IP7 may already be spuriously latched here too -
     * Compare is still its reset default of 0 at this point (nothing
     * has acked it yet, since only a Compare WRITE acks it, and that
     * hasn't happened yet - see the file header comment on the
     * trivial Count>=Compare==0 collision). That's expected and
     * harmless (Status.IE is still 0), and not what this test is
     * about - the real point below is that it's latched (for
     * whichever reason) by the time Compare has genuinely been armed
     * and overshot. */

    ee_core_step(); /* MTC0 Compare=1 -> Compare becomes 1, but Count becomes 3 this same step (already past 1) */
    CHECK(st->cop0[11] == 1, "Overshoot: Compare == 1 after the write");
    CHECK(st->cop0[9] == 3, "Overshoot: Count == 3 - already past the Compare value of 1 by the time it was written");
    CHECK((st->cop0[13] & 0x00008000u) != 0, "Overshoot: Cause.IP7 latched anyway (Count>=Compare, not ==)");
    CHECK((st->cop0[12] & 0x2u) == 0, "Overshoot: not taken yet - Status.IE is still 0 at this point");

    ee_core_step(); /* LUI */
    ee_core_step(); /* ORI */
    CHECK(st->pc == base_pc + status_enable_pc, "Overshoot: about to execute the Status-enable MTC0");

    ee_core_step(); /* MTC0 Status enabled -> the already-latched interrupt is taken right at the end of this same step */
    CHECK((st->cop0[12] & 0x2u) != 0, "Overshoot: Status.EXL == 1 - the surviving latch was taken as soon as gating allowed it");
    CHECK(st->cop0[14] == base_pc + after_enable_pc, "Overshoot: EPC points at the instruction right after the Status-enable write");
}

int main(void) {
    test_timer_fires_on_match();
    test_timer_gated_by_ie();
    test_timer_gated_by_im7();
    test_timer_deferred_across_delay_slot();
    test_compare_write_clears_pending();
    test_timer_overshoot_still_latches();

    if (failures) {
        printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("\nAll checks passed.\n");
    return 0;
}
