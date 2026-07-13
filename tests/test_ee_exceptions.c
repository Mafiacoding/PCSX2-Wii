/*
 * test_ee_exceptions.c - host-native test for ee_core.c's real MIPS
 * exception delivery: ee_raise_exception()/ee_raise_tlb_exception(),
 * ported from PCSX2's cpuException()/cpuTlbMiss() in R5900.cpp.
 *
 * This is the direct continuation of the COP0 TLB work
 * (test_ee_cop0_tlb.c): once real-BIOS boot progressed past the PRId
 * fix (round 5) and the TLB implementation (round 6), it diverged at
 * instruction #158-159 on a genuine TLB miss ($sp=0x70003eb0, no
 * installed TLB entry covers it) - architecturally exactly the case
 * real hardware services via a TLB Refill exception. Before this
 * work, a KUSEG TLB miss just silently read as 0 / no-op'd (see
 * test_ee_cop0_tlb.c's own history); this file tests the real
 * exception path that replaced that placeholder. See docs/STATUS.md
 * for the full trace.
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

static uint32_t enc_lui(int rt, uint16_t imm)        { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_addiu(int rt, int rs, int16_t imm) { return (0x09 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_beq(int rs, int rt, int16_t imm) { return (0x04 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_sw(int rt, int rs, int16_t imm)  { return (0x2B << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_lw(int rt, int rs, int16_t imm)  { return (0x23 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_mtc0(int rt, int rd) { return (0x10 << 26) | (0x04 << 21) | (rt << 16) | (rd << 11); }
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

/* --- 1. Data STORE TLB exception (SW to an unmapped KUSEG address):
 * Cause.ExcCode must be TLBS (store), not TLBL (load) - the opposite
 * ExcCode from a faulting load, the one thing that differs between
 * the two paths through ee_raise_tlb_exception(). */
static void test_store_exception(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_lui(4, 0x7FFF));  pc += 4; /* a0 = 0x7FFF0000, unmapped KUSEG */
    wle32(p+pc, enc_lui(5, 0x1234));  pc += 4; /* a1 = value to store */
    uint32_t sw_pc = pc;
    wle32(p+pc, enc_sw(5, 4, 0x0));   pc += 4; /* SW a1, 0(a0) -> faults */
    wle32(p+pc, enc_break());        pc += 4;

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    uint32_t base_pc = st->pc;
    ee_core_step(); /* LUI a0 */
    ee_core_step(); /* LUI a1 */
    ee_core_step(); /* SW - faults */

    CHECK((st->cop0[13] & 0x7Cu) == (3u << 2), "Store TLB miss: Cause.ExcCode == TLBS (3), not TLBL");
    CHECK(st->cop0[14] == base_pc + sw_pc, "Store TLB miss: EPC points at the faulting SW instruction");
    CHECK(st->cop0[8] == 0x7FFF0000u, "Store TLB miss: BadVAddr == the faulting virtual address");
}

/* --- 2. Branch-delay-slot fault: the faulting instruction is itself
 * in a delay slot, so Cause.BD must be set and EPC must point at the
 * BRANCH (not the delay-slot instruction) - ported from PCSX2's own
 * "if (bd) EPC = pc - 4" in cpuException(). */
static void test_delay_slot_fault(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_lui(4, 0x7FFF));       pc += 4; /* a0 = 0x7FFF0000, unmapped KUSEG */
    uint32_t beq_pc = pc;
    wle32(p+pc, enc_beq(0, 0, 1));         pc += 4; /* always taken (rs==rt==$0), target = beq_pc+4+(1<<2) */
    wle32(p+pc, enc_lw(5, 4, 0x0));        pc += 4; /* delay slot - faults */
    wle32(p+pc, enc_break());              pc += 4; /* branch target (never reached - fault preempts it) */

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    uint32_t base_pc = st->pc;
    ee_core_step(); /* LUI */
    ee_core_step(); /* BEQ (taken) */
    ee_core_step(); /* LW in delay slot - faults */

    CHECK((st->cop0[13] & 0x80000000u) != 0, "Delay-slot fault: Cause.BD == 1");
    CHECK(st->cop0[14] == base_pc + beq_pc, "Delay-slot fault: EPC points at the BEQ, not the delay-slot LW");
    CHECK((st->cop0[13] & 0x7Cu) == (2u << 2), "Delay-slot fault: Cause.ExcCode == TLBL (it was a load)");
}

/* --- 3. Instruction-fetch TLB exception: jumping into an unmapped
 * KUSEG address must fault on the FETCH itself (before the bogus
 * "instruction" there is even decoded), with EPC pointing at the
 * fetch address and Cause.BD == 0 (the jump itself, JR, is not in a
 * delay slot here). */
static void test_fetch_exception(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_lui(4, 0x7FFF));  pc += 4; /* a0 = 0x7FFF0000 */
    uint32_t jr_pc = pc;
    wle32(p+pc, (0x00 << 26) | (4 << 21) | 0x08); pc += 4; /* JR a0 -> jumps into unmapped KUSEG */
    /* JR's own delay slot - executes normally first. Task #178: this
     * used to be a BREAK (relying on the old unconditional-halt
     * placeholder to stop and let us inspect state cleanly). Now that
     * BREAK raises a real Breakpoint exception, using it here would
     * itself set Status.EXL=1 and Cause/EPC before the manual
     * fetch-exception setup below even runs - polluting exactly the
     * "still inside a previous exception" state that would trip the
     * nested-exception EPC guard (see test_nested_exception) and make
     * this test's own EPC check fail for an unrelated reason. Use a
     * harmless canary ADDIU instead: it proves the delay slot really
     * executed as ordinary code first, without touching COP0 at all. */
    wle32(p+pc, enc_addiu(7, 0, 0x2A)); pc += 4;

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    uint32_t base_pc = st->pc;
    ee_core_step(); /* LUI */
    ee_core_step(); /* JR (schedules the jump) */
    ee_core_step(); /* delay slot (canary ADDIU) - executes normally, no exception */
    CHECK(st->gpr[7].ud0 == 0x2Au, "Fetch-exception setup: JR's own delay slot (canary ADDIU) executed normally before the jump");

    /* Force pc to the unmapped target, bypassing the jump's natural
     * landing spot (which the JR already scheduled anyway - this just
     * makes the fault deterministic and independent of what garbage
     * bytes happen to sit at 0x7FFF0000), purely to exercise the
     * fetch-exception path on its own without needing a second real
     * program. */
    st->pc = 0x7FFF0000u;
    st->next_pc = 0x7FFF0004u;
    st->branch_pending = 0;
    ee_core_step(); /* fetch at 0x7FFF0000 faults */

    CHECK((st->cop0[13] & 0x7Cu) == (2u << 2), "Fetch TLB miss: Cause.ExcCode == TLBL");
    CHECK(st->cop0[14] == 0x7FFF0000u, "Fetch TLB miss: EPC == the unmapped fetch address");
    CHECK((st->cop0[13] & 0x80000000u) == 0, "Fetch TLB miss: Cause.BD == 0 (not itself a delay slot)");
    CHECK(st->cop0[8] == 0x7FFF0000u, "Fetch TLB miss: BadVAddr == the unmapped fetch address");
    (void)base_pc; (void)jr_pc;
}

/* --- 4. Status.BEV vectoring: with BEV cleared (as the real BIOS does
 * after installing its own RAM-resident exception handlers), the same
 * TLB Refill exception must vector into RAM (0x80000000+) instead of
 * the boot-time uncached ROM vector (0xBFC00200+). */
static void test_bev_vectoring(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_lui(4, 0x7FFF));  pc += 4;
    /* Clear BEV (bit 22) via MTC0, keep everything else from the reset
     * value - matches how a real BIOS would clear just that one bit
     * after installing handlers, not zero the whole register. */
    wle32(p+pc, enc_lui(6, 0x7000));  pc += 4; /* $a2 = 0x70000000 (reset value with BEV, bit 22, actually cleared - 0x7040.... keeps bit22 set, only the low nibble differs, so it must be masked out of the upper half instead) */
    wle32(p+pc, enc_mtc0(6, 12));     pc += 4;
    wle32(p+pc, enc_lw(5, 4, 0x0));   pc += 4; /* faults */
    wle32(p+pc, enc_break());        pc += 4;

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    ee_core_step(); /* LUI a0 */
    ee_core_step(); /* LUI a2 */
    ee_core_step(); /* MTC0 - clears BEV */
    CHECK((st->cop0[12] & 0x00400000u) == 0, "BEV vectoring setup: Status.BEV successfully cleared via MTC0");
    ee_core_step(); /* LW - faults */

    CHECK(st->pc == 0x80000000u, "BEV=0: TLB Refill vectors into RAM (0x80000000), not the boot-time ROM vector");
}

/* --- 5. Nested exception: if Status.EXL is already 1 (still inside a
 * previous, unresolved exception handler), a second exception must
 * NOT touch EPC again (the original handler's return address has to
 * survive), and must vector to the general-exception offset (0x180)
 * regardless of this new exception's own ExcCode - ported from
 * PCSX2's cpuException() ("offset = 0x180; //Override the cause"). */
static void test_nested_exception(void) {
    ee_state_t *st = ee_core_get_state();
    memset(st, 0, sizeof(*st));
    st->cop0[12] = 0x70400004u; /* real reset value: BEV=1, ERL=1 */

    ee_raise_tlb_exception(st, /*is_store=*/0, 0x11110000u, 0x1000u, 0);
    CHECK((st->cop0[12] & 0x2u) != 0, "Nested-exception setup: first fault set Status.EXL");
    CHECK(st->cop0[14] == 0x1000u, "Nested-exception setup: first fault's EPC recorded correctly");
    CHECK(st->pc == 0xBFC00200u, "Nested-exception setup: first fault vectored to the TLB Refill vector (offset 0)");

    /* Second, unrelated exception while EXL is still 1 (i.e. nothing
     * has ERET'd yet) - must NOT change EPC, and must use the general
     * vector (offset 0x180) even though this is ALSO a TLB code. */
    st->exc_raised_this_step = 0; /* new instruction/step context - the once-per-instruction guard below is a separate mechanism */
    ee_raise_tlb_exception(st, /*is_store=*/1, 0x22220000u, 0x2000u, 0);
    CHECK(st->cop0[14] == 0x1000u, "Nested exception: EPC unchanged (still points at the FIRST fault)");
    CHECK(st->pc == 0xBFC00380u, "Nested exception: vectors to the general-exception offset (0x180), not TLB Refill (0x0)");
}

/* --- 6. exc_raised_this_step guard: two TLB-miss-raising calls within
 * what the code considers "the same instruction" (guard not reset
 * between them) must only actually take effect once - the second call
 * is a no-op. This models the real reason the guard exists: SWL/SWR
 * internally do a read then a write of the SAME address (to merge
 * partial bytes), and would otherwise raise two conflicting exceptions
 * for a single guest instruction. White-box test (direct calls, not a
 * MIPS program) since it's specifically testing this internal guard
 * mechanism, not end-to-end instruction behavior. */
static void test_exc_raised_guard(void) {
    ee_state_t *st = ee_core_get_state();
    memset(st, 0, sizeof(*st));
    st->cop0[12] = 0x70400004u;
    st->exc_raised_this_step = 0; /* fresh "instruction" */

    ee_raise_tlb_exception(st, /*is_store=*/0, 0xAAAA0000u, 0x3000u, 0);
    CHECK(st->cop0[8] == 0xAAAA0000u, "Guard test: first call's BadVAddr recorded");
    CHECK((st->cop0[13] & 0x7Cu) == (2u << 2), "Guard test: first call's ExcCode (TLBL) recorded");

    /* Second call, SAME step (guard not reset) - must be fully
     * ignored, even though it's a different address/direction/pc. */
    ee_raise_tlb_exception(st, /*is_store=*/1, 0xBBBB0000u, 0x4000u, 0);
    CHECK(st->cop0[8] == 0xAAAA0000u, "Guard test: second call's BadVAddr did NOT overwrite the first");
    CHECK((st->cop0[13] & 0x7Cu) == (2u << 2), "Guard test: second call's ExcCode (TLBS) did NOT overwrite the first (still TLBL)");
}

int main(void) {
    test_store_exception();
    test_delay_slot_fault();
    test_fetch_exception();
    test_bev_vectoring();
    test_nested_exception();
    test_exc_raised_guard();

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
