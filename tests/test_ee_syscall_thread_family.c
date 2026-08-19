/*
 * test_ee_syscall_thread_family.c - host-native test for Round 187
 * (task #353): a fresh, full EE syscall-table audit against ps2sdk's
 * real ee/kernel/include/syscallnr.h found this entire real thread-
 * management family unhandled (silently halting the whole machine
 * via the generic "no BIOS syscall table implemented" fallback if
 * ever reached): DeleteThread(33), ExitThread(35),
 * ExitDeleteThread(36), TerminateThread(37),
 * DisableDispatchThread(39), EnableDispatchThread(40),
 * ChangeThreadPriority(41), RotateThreadReadyQueue(43),
 * ReleaseWaitThread(45), SleepThread(50), WakeupThread(51),
 * CancelWakeupThread(53), SuspendThread(55), ResumeThread(57).
 * Original (pre-Round-569) fix: raise a real MIPS Syscall exception
 * (ExcCode 8) so the real BIOS kernel handler does its own real
 * thread-state bookkeeping in software this project cannot safely
 * guess.
 *
 * UPDATED Round 240 (task #408 experiment): CreateThread (32) and
 * StartThread (34) were originally deliberately EXCLUDED from this
 * family and given fixed placeholder return values instead, then
 * changed to ALSO vector as real exceptions like every sibling in
 * this family (see git history for the full reasoning).
 *
 * SUPERSEDED Round 569 (task #543): a real, evidenced EE HLE
 * thread/semaphore scheduler (ee_hle_thread_try_handle(),
 * include/core/ee/ee_hle_thread.h / source/core/ee/ee_hle_thread.c)
 * was implemented and shipped. It is called as the VERY FIRST check
 * inside ee_core.c's case 0x0C (SYSCALL) body - "if
 * (ee_hle_thread_try_handle(st, sysnum, this_pc, in_delay_slot))
 * return 1;" - and it directly, correctly handles a real, cited
 * subset of these same numbers in software (own real TCB table,
 * priority/ready-queue bookkeeping, etc.) rather than vectoring a
 * MIPS exception for the BIOS kernel to handle. This was a
 * deliberate, verified, shipped improvement (real EE
 * multi-threading, needed for actual forward boot progress) - see
 * STATUS.md Round 569 and Round 623 writeups.
 *
 * ROUND 623 (task #554/#603 investigation) discovered this file was
 * never updated after Round 569 shipped, so it still asserted the
 * OLD (pre-Round-569) "must vector as an exception" behavior for
 * every number in this family, including several now legitimately
 * intercepted by ee_hle_thread_try_handle()'s own real, cited
 * `handled[]` list: { 32, 33, 34, 35, 36, 37, -38, 39, 40, 41, -42,
 * 43, -44, 47, -47, 48, -49, 50, 51, -52, 53, -54, 64, 65, 66, -67,
 * 68, 69 }. Live diagnostic instrumentation (fprintf probes at the
 * top of case 0x0C and immediately before the old exception-
 * vectoring if-block) proved execution for sysnum=33 (and siblings
 * 35/36/37/39/40/41/43/50/51/53/32/34) never even reaches the old
 * exception-vectoring code any more - it is legitimately claimed and
 * fully handled earlier by the real HLE scheduler, which returns 1
 * and never raises an exception. This is NOT an ee_core.c bug: the
 * HLE scheduler's behavior for an invalid/uninitialized thread id
 * (this test's synthetic harness never sets up a real TCB, so $a0 is
 * garbage) is to return -1 in $v0 and advance pc by +4, exactly like
 * every other syscall completion in this project's established
 * convention - this is correct, real syscall-return semantics, not a
 * fabricated/guessed result.
 *
 * This file is corrected accordingly: sysnums now claimed by the
 * real HLE scheduler are asserted against the real HLE-completion
 * behavior (halted==0, pc advances past the SYSCALL by exactly 4,
 * NOT vectored as a Syscall exception). The remaining sysnums in
 * this historical family that ee_hle_thread_try_handle()'s handled[]
 * list does NOT claim - ReleaseWaitThread (45), SuspendThread (55),
 * ResumeThread (57) - are unaffected by Round 569 and still correctly
 * vector as real exceptions exactly as before; their assertions are
 * unchanged.
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

static uint32_t enc_addiu(int rt, int rs, int16_t imm) { return (0x09 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_syscall(void) { return (0x0Cu); } /* SPECIAL opcode 0, funct 0x0C */
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

/* sysnum lives in $v1 (GPR 3), real EE convention. Numbers in
 * ee_hle_thread_try_handle()'s own real handled[] list are now
 * claimed by the real HLE thread/sema scheduler (Round 569) and must
 * NOT vector as a MIPS exception any more - checked via
 * expect_hle_handled=1. The remainder still vector as a real
 * exception exactly as before (expect_hle_handled=0). */
static void run_syscall_test(int32_t sysnum, const char *label, int expect_hle_handled) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_addiu(3, 0, (int16_t)sysnum)); pc += 4; /* $v1 = sysnum */
    uint32_t syscall_pc = pc;
    wle32(p+pc, enc_syscall());                    pc += 4; /* SYSCALL */
    wle32(p+pc, 0x0u);                              pc += 4; /* delay slot: NOP */

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    uint32_t base_pc = st->pc;

    ee_core_step(); /* ADDIU $v1, sysnum */
    ee_core_step(); /* SYSCALL */

    char msg[160];

    if (expect_hle_handled) {
        snprintf(msg, sizeof(msg), "%s: halted must remain 0 (real HLE thread scheduler, Round 569)", label);
        CHECK(st->halted == 0, msg);

        snprintf(msg, sizeof(msg), "%s: NOT vectored as a Syscall exception (claimed earlier by ee_hle_thread_try_handle)", label);
        CHECK((st->cop0[13] & 0x7Cu) != EE_EXC_CODE_SYS, msg);

        snprintf(msg, sizeof(msg), "%s: pc advances past SYSCALL by +4 (real HLE syscall-return convention)", label);
        CHECK(st->pc == base_pc + syscall_pc + 4u, msg);
    } else {
        snprintf(msg, sizeof(msg), "%s: halted must remain 0 (not the old halt() behavior)", label);
        CHECK(st->halted == 0, msg);

        snprintf(msg, sizeof(msg), "%s: Cause.ExcCode == Syscall (8)", label);
        CHECK((st->cop0[13] & 0x7Cu) == EE_EXC_CODE_SYS, msg);

        snprintf(msg, sizeof(msg), "%s: EPC points at the SYSCALL instruction itself", label);
        CHECK(st->cop0[14] == base_pc + syscall_pc, msg);

        snprintf(msg, sizeof(msg), "%s: pc vectors to the general exception offset (0xBFC00380, BEV=1 reset default)", label);
        CHECK(st->pc == 0xBFC00380u, msg);
    }
}

int main(void) {
    /* Round 623 correction: these are all in ee_hle_thread_try_handle()'s
     * real handled[] list (Round 569) and are now claimed by the real
     * HLE thread scheduler before the old exception-vectoring code is
     * ever reached - verified via live diagnostic instrumentation. */
    run_syscall_test(33, "DeleteThread (33/0x21) [Round 569: real HLE thread scheduler]", 1);
    run_syscall_test(35, "ExitThread (35/0x23) [Round 569: real HLE thread scheduler]", 1);
    run_syscall_test(36, "ExitDeleteThread (36/0x24) [Round 569: real HLE thread scheduler]", 1);
    run_syscall_test(37, "TerminateThread (37/0x25) [Round 569: real HLE thread scheduler]", 1);
    run_syscall_test(39, "DisableDispatchThread (39/0x27) [Round 569: real HLE thread scheduler]", 1);
    run_syscall_test(40, "EnableDispatchThread (40/0x28) [Round 569: real HLE thread scheduler]", 1);
    run_syscall_test(41, "ChangeThreadPriority (41/0x29) [Round 569: real HLE thread scheduler]", 1);
    run_syscall_test(43, "RotateThreadReadyQueue (43/0x2b) [Round 569: real HLE thread scheduler]", 1);
    run_syscall_test(50, "SleepThread (50/0x32) [Round 569: real HLE thread scheduler]", 1);
    run_syscall_test(51, "WakeupThread (51/0x33) [Round 569: real HLE thread scheduler]", 1);
    run_syscall_test(53, "CancelWakeupThread (53/0x35) [Round 569: real HLE thread scheduler]", 1);

    /* NOT in ee_hle_thread_try_handle()'s handled[] list - these three
     * are unaffected by Round 569 and still correctly vector as real
     * MIPS Syscall exceptions, exactly as originally implemented. */
    run_syscall_test(45, "ReleaseWaitThread (45/0x2d)", 0);
    run_syscall_test(55, "SuspendThread (55/0x37)", 0);
    run_syscall_test(57, "ResumeThread (57/0x39)", 0);

    /* Round 240: CreateThread (32) and StartThread (34) - also now
     * claimed by the real Round 569 HLE thread scheduler. */
    run_syscall_test(32, "CreateThread (32/0x20) [Round 569: real HLE thread scheduler]", 1);
    run_syscall_test(34, "StartThread (34/0x22) [Round 569: real HLE thread scheduler]", 1);

    if (failures == 0)
        printf("ALL CHECKS PASSED (0 failures)\n");
    else
        printf("%d check(s) FAILED\n", failures);
    return failures != 0;
}
