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
 * Same established fix pattern as syscalls 6/7/16/17/18/19/124: raise
 * a real MIPS Syscall exception (ExcCode 8) instead of halting.
 *
 * UPDATED Round 240 (task #408 experiment): CreateThread (32) and
 * StartThread (34) were originally deliberately EXCLUDED from this
 * family and given fixed placeholder return values instead (see the
 * git history for the old comment) because this project assumed it
 * "has no real concurrent EE thread scheduler to actually run a
 * second thread body on." Round 240 tested the more principled
 * hypothesis that this exclusion was unnecessary: a real PS2/MIPS
 * kernel's own thread-management code performs its own context
 * switches using only ordinary real instructions this interpreter
 * already correctly executes (loads/stores against its own real,
 * RAM-resident TCB table, MTC0 to EPC, ERET) - so letting 32/34 ALSO
 * vector as real Syscall exceptions, exactly like every sibling in
 * this same family, should let the real, unmodified kernel do its own
 * genuine thread bring-up with zero fabricated software state, same
 * as this file's other 14 numbers already do. The former "regression
 * check" asserting 32/34 must NOT raise an exception is replaced below
 * with the opposite assertion, reflecting this deliberate behavior
 * change. Honest result (documented in STATUS.md/ROADMAP.md): a
 * host-native full-boot diagnostic showed this change has ZERO
 * measurable effect on the current traced boot wall, because the
 * current trace never actually reaches a CreateThread/StartThread
 * call site within the tested instruction budget - the hypothesis is
 * therefore unconfirmed (neither proven nor disproven) by real BIOS
 * execution, but kept as the more architecturally consistent choice
 * for the syscalls that DO get reached in other traces/future work.
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

/* sysnum lives in $v1 (GPR 3), real EE convention. */
static void run_syscall_test(int32_t sysnum, const char *label) {
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
    ee_core_step(); /* SYSCALL - should raise a real exception */

    char msg[128];
    snprintf(msg, sizeof(msg), "%s: halted must remain 0 (not the old halt() behavior)", label);
    CHECK(st->halted == 0, msg);

    snprintf(msg, sizeof(msg), "%s: Cause.ExcCode == Syscall (8)", label);
    CHECK((st->cop0[13] & 0x7Cu) == EE_EXC_CODE_SYS, msg);

    snprintf(msg, sizeof(msg), "%s: EPC points at the SYSCALL instruction itself", label);
    CHECK(st->cop0[14] == base_pc + syscall_pc, msg);

    snprintf(msg, sizeof(msg), "%s: pc vectors to the general exception offset (0xBFC00380, BEV=1 reset default)", label);
    CHECK(st->pc == 0xBFC00380u, msg);
}

int main(void) {
    run_syscall_test(33, "DeleteThread (33/0x21)");
    run_syscall_test(35, "ExitThread (35/0x23)");
    run_syscall_test(36, "ExitDeleteThread (36/0x24)");
    run_syscall_test(37, "TerminateThread (37/0x25)");
    run_syscall_test(39, "DisableDispatchThread (39/0x27)");
    run_syscall_test(40, "EnableDispatchThread (40/0x28)");
    run_syscall_test(41, "ChangeThreadPriority (41/0x29)");
    run_syscall_test(43, "RotateThreadReadyQueue (43/0x2b)");
    run_syscall_test(45, "ReleaseWaitThread (45/0x2d)");
    run_syscall_test(50, "SleepThread (50/0x32)");
    run_syscall_test(51, "WakeupThread (51/0x33)");
    run_syscall_test(53, "CancelWakeupThread (53/0x35)");
    run_syscall_test(55, "SuspendThread (55/0x37)");
    run_syscall_test(57, "ResumeThread (57/0x39)");

    /* Round 240: CreateThread (32) and StartThread (34) now vector
     * exactly like every other syscall in this family - see the
     * updated file header comment above for the full reasoning and
     * honest result. */
    run_syscall_test(32, "CreateThread (32/0x20) [Round 240: now vectors like the rest]");
    run_syscall_test(34, "StartThread (34/0x22) [Round 240: now vectors like the rest]");

    if (failures == 0)
        printf("ALL CHECKS PASSED (0 failures)\n");
    else
        printf("%d check(s) FAILED\n", failures);
    return failures != 0;
}
