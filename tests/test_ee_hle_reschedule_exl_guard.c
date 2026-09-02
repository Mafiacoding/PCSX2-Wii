/*
 * test_ee_hle_reschedule_exl_guard.c - Round 812 (task #811/#813,
 * user-relayed external-review plan): targeted host-native regression
 * test for reschedule()'s new Status.EXL/ERL guard in
 * source/core/ee/ee_hle_thread.c.
 *
 * Background (full writeup in docs/STATUS.md's Round 812 entry): a
 * captured GT3 disc-boot event log (2,532,948 tid=1-filtered lines)
 * proved that reschedule()'s switch-out path unconditionally called
 * save_context(st, g.current_thread_id), and that this silently
 * corrupted thread 1's real saved pc (0x0101bc24, its genuine
 * WaitSema(5) busy-park instruction) with an unrelated interrupt
 * handler's mid-flight pc (0x0101bb28, WakeupThread's own syscall
 * trampoline's jr-ra target) whenever reschedule() was reached from
 * an interrupt-context syscall (WakeupThread/-52 iWakeupThread is the
 * real PS2 kernel's own convention for exactly this) while Status.EXL
 * was still set - i.e. while the "live" st registers actually
 * belonged to the interrupt handler, not to g.current_thread_id's own
 * suspended state. The fix adds a `st->cop0[12] & 0x6u` (EXL|ERL)
 * guard at the very top of reschedule(), deferring the entire
 * context-switch decision (including the none-ready branch's re-save)
 * until the exception genuinely clears - mirroring the guard Round
 * 598 already added to ee_hle_thread_check_preempt()'s separate
 * per-instruction path.
 *
 * Reproducing the exact multi-billion-instruction GT3 scenario
 * organically is impractical for a fast regression test (the original
 * checkpoint lineage that reached thread 1's WAIT/SEMA/5 park took
 * many prior rounds/sessions of chained boot to build). Instead this
 * test drives the *real* WakeupThread syscall handler (sysnum 51,
 * source/core/ee/ee_hle_thread.c ~line 706-728) - the exact call site
 * implicated by the captured evidence - through ee_core_step(), and
 * checks the precise invariant the fix establishes. Scenario, chosen
 * so the switch is genuinely due (not merely a vacuous "nothing would
 * have changed anyway" no-op even without the fix):
 *
 *   - thread B (worse/higher priority=80 than root's 64) is created
 *     and started, then root SleepThread()s - since B is the only
 *     other READY thread, root's own reschedule() call (inside
 *     SleepThread's handler) switches control to B for real. Root is
 *     now WAIT/SLEEP, B is RUN.
 *   - WakeupThread(root) is called (as if from an interrupt handler)
 *     while B is current. Root has BETTER priority (64) than B (80),
 *     so pick_next_ready() genuinely wants to switch back to root -
 *     this is a real, due context switch, not a no-op.
 *   - With Status.EXL SET at that moment, the fixed reschedule() must
 *     defer: current_thread_id must stay on B, root must NOT become
 *     RUN (it may still be flipped WAIT->READY by WakeupThread's own
 *     status-transition code, which runs before reschedule() and is
 *     unaffected by the guard - only the switch itself is deferred).
 *   - Re-issuing the identical WakeupThread(root) call with Status.EXL
 *     CLEAR must then perform the real switch: current_thread_id
 *     becomes root, root is RUN, B is switched out to READY. This
 *     proves the guard defers rather than permanently suppressing the
 *     scheduling decision.
 *
 * EE_THS_RUN=0x01 / EE_THS_READY=0x02 / EE_THS_WAIT=0x04 are
 * ee_hle_thread.c's own private status constants (not exposed in the
 * public header); their numeric values are re-stated here for direct
 * comparison against ee_hle_thread_get_status(), matching the values
 * cited throughout this project's own Round 812 event-log evidence
 * and source comments.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include "core/ee/ee_core.c"
#include "core/ee/ee_hle_thread.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

#define TEST_EE_THS_RUN   0x01u
#define TEST_EE_THS_READY 0x02u
#define TEST_EE_THS_WAIT  0x04u

static uint32_t enc_syscall(void) { return 0x0Cu; } /* SPECIAL opcode 0, funct 0x0C */
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

int main(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    /* Five back-to-back SYSCALL instructions; the harness pokes $v1
     * (sysnum) and $a0/$a1 (args) directly before each ee_core_step(),
     * exactly like tests/test_ee_syscall_setupthread.c's established
     * pattern - no MIPS-level arg-building code needed. */
    wle32(p + 0,  enc_syscall()); /* #1: CreateThread */
    wle32(p + 4,  enc_syscall()); /* #2: StartThread */
    wle32(p + 8,  enc_syscall()); /* #3: SleepThread (root blocks, real switch to B) */
    wle32(p + 12, enc_syscall()); /* #4: WakeupThread(root), EXL SET   -> must defer */
    wle32(p + 16, enc_syscall()); /* #5: WakeupThread(root), EXL CLEAR -> must switch */

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    const uint32_t base_pc = st->pc; /* reset vector, read fresh rather than assumed */

    /* Real MIPS/EE reset state has Status.ERL=1 (confirmed here:
     * post-init cop0[12]==0x70400004); real kernel init code clears
     * it early via COP0 Status writes, well before any HLE thread
     * scheduling activity occurs (matching this project's already-
     * verified, unregressed real boot traces). Clear it explicitly
     * here to model that already-initialized, steady-state condition
     * - otherwise EVERY reschedule() call below (including the
     * legitimate, non-exception ones the test relies on to reach its
     * test scenario) would be deferred by the very guard under test,
     * for the wrong reason. */
    st->cop0[12] &= ~0x6u;

    /* CreateThread(ee_thread_t *thread) param struct in RAM, written
     * directly via ee_mem_write32 (visible here because this test
     * #includes ee_core.c directly, per project convention) - field
     * offsets per ee_hle_thread.c's CreateThread handler (sysnum 32):
     * func@4, stack@8, stack_size@0xC, gp_reg@0x10, priority@0x14,
     * attr@0x1C, option@0x20. KSEG0 (0x80000000+) is identity-mapped
     * to physical RAM with no TLB entry required; a plain KUSEG
     * address faults with a TLB-refill exception in this synthetic
     * no-TLB-setup test. */
    const uint32_t param = 0x80002000u;
    /* entry: B is genuinely switched into by SleepThread's real switch
     * below (this test does NOT avoid running B's own code - it needs
     * a live "current thread" to attempt the reschedule() switch back
     * away from). Point B's entry at syscall #4 (base_pc+12) - the
     * SAME flat instruction stream this test already laid out above -
     * so that once B is switched in, its own live pc naturally
     * continues fetching this test's next intended syscall instead of
     * landing on an unrelated/bogus address. */
    const uint32_t thread_b_entry = base_pc + 12u;
    ee_mem_write32(st, param + 0x04u, thread_b_entry); /* entry */
    ee_mem_write32(st, param + 0x08u, 0x00300000u); /* stack_base */
    ee_mem_write32(st, param + 0x0Cu, 0x00002000u); /* stack_size */
    ee_mem_write32(st, param + 0x10u, 0x00000000u); /* gp_reg */
    ee_mem_write32(st, param + 0x14u, 80u);         /* priority: worse (higher number) than root thread's 64 (ensure_root_thread) */
    ee_mem_write32(st, param + 0x1Cu, 0x00000000u); /* attr */
    ee_mem_write32(st, param + 0x20u, 0x00000000u); /* option */

    /* --- syscall #1: CreateThread(&param) --- */
    st->gpr[4].ud0 = param; /* $a0 */
    st->gpr[3].ud0 = 32;    /* $v1 = CreateThread sysnum */
    ee_core_step();
    int tid_b = (int)(int32_t)st->gpr[2].ud0; /* $v0 = new tid */
    CHECK(tid_b > 0, "CreateThread: returned a valid (positive) thread id for thread B");

    int root_tid = ee_hle_thread_get_current_thread_id();
    CHECK(root_tid > 0 && root_tid != tid_b, "root/caller thread id established and distinct from thread B");

    /* --- syscall #2: StartThread(tid_b, 0) --- */
    st->gpr[4].ud0 = (uint64_t)(uint32_t)tid_b; /* $a0 */
    st->gpr[5].ud0 = 0;                          /* $a1 = arg */
    st->gpr[3].ud0 = 34;                         /* $v1 = StartThread sysnum */
    ee_core_step();

    CHECK(ee_hle_thread_get_status(tid_b) == TEST_EE_THS_READY,
          "StartThread: thread B is READY (created, not yet switched in)");
    CHECK(ee_hle_thread_get_current_thread_id() == root_tid,
          "StartThread: root/caller thread remains current (B's priority 80 is worse than root's 64, no preemption)");

    /* --- syscall #3: SleepThread() - root genuinely blocks; since B
     * is the only other READY thread, this reschedule() (called with
     * Status.EXL clear, a completely ordinary in-thread syscall) must
     * perform a REAL switch to B, giving us a live "current thread"
     * whose saved context we can then try to corrupt in step #4. --- */
    st->gpr[3].ud0 = 50; /* $v1 = SleepThread sysnum, no args */
    ee_core_step();

    CHECK(ee_hle_thread_get_status(root_tid) == TEST_EE_THS_WAIT,
          "SleepThread: root is now WAIT (genuinely blocked, wakeup_count was 0)");
    CHECK(ee_hle_thread_get_current_thread_id() == tid_b,
          "SleepThread: real switch to thread B occurred (B was the only other READY thread)");
    CHECK(ee_hle_thread_get_status(tid_b) == TEST_EE_THS_RUN,
          "SleepThread: thread B is now RUN");

    /* --- syscall #4: WakeupThread(root) while Status.EXL is set ---
     * This is the exact call site implicated by the Round 812 GT3
     * event-log evidence: WakeupThread's handler always calls
     * reschedule() unconditionally at its end (source line ~727).
     * Root has BETTER priority (64) than the current thread B (80),
     * so this switch is genuinely due - pick_next_ready() would
     * select root right now if reschedule() were allowed to run its
     * normal switch logic. The guard must defer it anyway. */
    st->cop0[12] |= 0x2u; /* Status.EXL */
    st->gpr[4].ud0 = (uint64_t)(uint32_t)root_tid; /* $a0 = target */
    st->gpr[3].ud0 = 51;                            /* $v1 = WakeupThread sysnum */
    ee_core_step();

    CHECK(ee_hle_thread_get_current_thread_id() == tid_b,
          "Round 812 fix: WakeupThread-triggered reschedule() under Status.EXL does NOT switch back to root, even though root now has the better priority");
    CHECK(ee_hle_thread_get_status(root_tid) != TEST_EE_THS_RUN,
          "Round 812 fix: root is NOT switched to RUN while Status.EXL is set (deferred, per reschedule()'s new guard)");

    /* --- syscall #5: WakeupThread(root) again, now with Status.EXL
     * clear - the deferred switch must now actually happen, proving
     * the guard defers rather than permanently suppressing the
     * scheduling decision. (Root is already READY at this point, so
     * this second call's own status-transition branch is a no-op;
     * only the unconditional trailing reschedule() call matters.) --- */
    st->cop0[12] &= ~0x6u; /* clear Status.EXL/ERL */
    st->gpr[4].ud0 = (uint64_t)(uint32_t)root_tid;
    st->gpr[3].ud0 = 51;
    ee_core_step();

    CHECK(ee_hle_thread_get_current_thread_id() == root_tid,
          "with Status.EXL clear, the identical WakeupThread call now performs the real switch back to root");
    CHECK(ee_hle_thread_get_status(root_tid) == TEST_EE_THS_RUN,
          "root is RUN once actually switched back in");
    CHECK(ee_hle_thread_get_status(tid_b) == TEST_EE_THS_READY,
          "thread B correctly switched out to READY");

    printf("\nTotal failures: %d\n", failures);
    return failures ? 1 : 0;
}
