/*
 * test_ee_syscall_setupthread.c - SetupThread (syscall 60/0x3C).
 *
 * Round 274 (task #423, 315th finding) history: this test originally
 * covered the software-emulated $sp-computation model, including its
 * real, cited $a1==0xFFFFFFFF (-1) sentinel handling (the real BIOS's
 * own OSDSYS ELF genuinely calls SetupThread with
 * $a1(stack_base)=0xFFFFFFFF and $a2(stack_size)=0x5000 at its real
 * entry, 0x00200064 - live-traced evidence, docs/STATUS.md's 315th
 * finding).
 *
 * ROUND 989 CORRECTION (task #969, SCPH-50004 diskless restart-loop
 * investigation): fresh disassembly of the SCPH-50004 decompression
 * stub's own crt0-style startup code showed it calls SetupThread with
 * a 5th argument, $a3(args)=0x00157C00 - the SAME RAM address the
 * stub later reads argc from (and computes argv at args+4) to decide
 * whether to proceed into OSDSYS normally or fall into its
 * reinit/restart branch. Real ps2sdk's own kernel.h (this project's
 * docs/reference/ps2sdk/ee/kernel/include/kernel.h, line 220) confirms
 * SetupThread's real signature takes exactly this 5th "args" parameter:
 *   extern void *SetupThread(void *gp, void *stack, s32 stack_size,
 *                             void *args, void *root_func);
 * This project's Round 171/274 software model never read or used
 * $a3 at all - it only ever computed $sp from gp/stack_base/stack_size
 * and direct-returned, exactly like this test's original assertions
 * expected. That is the actual modeling gap Round 989 found: real
 * SetupThread also writes the calling thread's real argc/argv (already
 * known to the kernel from the original _ExecPS2 dispatch that started
 * the thread) into the RAM cell pointed to by args.
 *
 * Per this project's own established "task #180 lesson" (already
 * applied to syscalls 6/7/16/17/18/19 - do not hand-guess a real,
 * resident-in-ROM kernel function's internal bookkeeping when this
 * project's own BIOS image already contains real code for it; let it
 * vector as a genuine MIPS Syscall exception instead, so the BIOS's own
 * resident kernel code performs the entire mechanism itself), Round 989
 * removed the software $sp-computation shortcut for syscall 60 and lets
 * it vector as a real exception like its siblings. This was empirically
 * verified: with the exception-raise in place, genuine resident BIOS
 * ROM code (observed at pc=0x80004FB4/0x80004FC4, in the low-kernel
 * address range) is what performs SetupThread for real, and it writes
 * argc=1 to 0x00157C00 and a valid argv pointer to 0x00157C04 - the
 * exact missing values that were causing SCPH-50004's diskless boot to
 * read argc==0 and fall into its infinite restart loop. A 160M-
 * instruction, 4-slice boot survey confirmed this breaks the loop, with
 * the EE settling at a stable, legitimate-looking resting point
 * (pc=0x0026fe9c, disassembly-verified as real OSDSYS code) instead of
 * repeatedly cycling through "Restart Without Memory Clear" messages.
 *
 * This file now asserts the Round 989 exception-vectoring behavior,
 * matching the established pattern used by
 * test_ee_syscall_full_audit_sweep.c's run_syscall_test() for
 * syscalls 6/7/16-19: halted remains 0, Cause.ExcCode == Syscall (8),
 * EPC points at the SYSCALL instruction itself, and pc vectors to the
 * general exception offset (0xBFC00380, BEV=1 reset default). The old
 * $sp/$gp/sentinel-substitution assertions are gone because that
 * software path no longer runs - real BIOS ROM code owns the entire
 * SetupThread mechanism now, including whatever real $sp/$gp handling
 * it does internally (verified separately via the resting-point
 * disassembly referenced above, not re-asserted here).
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

/* Sets up $v1=60 (SetupThread) then SYSCALL, with $a0/$a1/$a2/$a3 poked
 * directly beforehand (real EE calling convention: $a0=gp,
 * $a1=stack_base, $a2=stack_size, $a3=args). Verifies the Round 989
 * exception-vectoring behavior (matching syscalls 6/7/16-19), not the
 * retired software $sp-computation model. */
static void run_setupthread_test(uint32_t gp, uint32_t stack_base, int32_t stack_size,
                                  uint32_t args, const char *label) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_addiu(3, 0, 60));                pc += 4; /* $v1 = 60 */
    uint32_t syscall_pc = pc;
    wle32(p+pc, enc_syscall());                       pc += 4; /* SYSCALL */
    wle32(p+pc, 0x0u);                                pc += 4; /* delay slot: NOP */

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    uint32_t base_pc = st->pc;

    st->gpr[4].ud0 = gp;         /* $a0 */
    st->gpr[5].ud0 = stack_base; /* $a1 */
    st->gpr[6].ud0 = (uint32_t)stack_size; /* $a2 */
    st->gpr[7].ud0 = args;       /* $a3 */

    ee_core_step(); /* ADDIU $v1, 60 */
    ee_core_step(); /* SYSCALL - must vector, real BIOS ROM owns it now */

    char msg[160];
    snprintf(msg, sizeof(msg), "%s: halted must remain 0", label);
    CHECK(st->halted == 0, msg);

    snprintf(msg, sizeof(msg), "%s: Cause.ExcCode == Syscall (8)", label);
    CHECK((st->cop0[13] & 0x7Cu) == EE_EXC_CODE_SYS, msg);

    snprintf(msg, sizeof(msg), "%s: EPC points at the SYSCALL instruction itself", label);
    CHECK(st->cop0[14] == base_pc + syscall_pc, msg);

    snprintf(msg, sizeof(msg), "%s: pc vectors to the general exception offset (0xBFC00380, BEV=1 reset default)", label);
    CHECK(st->pc == 0xBFC00380u, msg);
}

int main(void) {
    /* Ordinary, finite stack_base/stack_size/args - the common case. */
    run_setupthread_test(0x00295170u, 0x00300000u, 0x2000, 0x00157C00u,
                          "SetupThread: ordinary finite stack_base");

    /* Zero stack_base, small stack_size - another ordinary case. */
    run_setupthread_test(0x00100000u, 0x00010000u, 0x1000, 0x00157C00u,
                          "SetupThread: small finite stack_base");

    /* The real, live-traced OSDSYS case: stack_base=0xFFFFFFFF (-1),
     * stack_size=0x5000 (20480) - the sentinel case from Round 274's
     * original finding. Now that this vectors as a real exception, the
     * sentinel substitution (if any) happens inside real BIOS ROM code,
     * not in this emulator's software path - only exception-vectoring
     * behavior is asserted here. */
    run_setupthread_test(0x00295170u, 0xFFFFFFFFu, 0x5000, 0x00157C00u,
                          "SetupThread: OSDSYS's real -1 sentinel stack_base");

    /* The real SCPH-50004 decompression-stub case (Round 989's actual
     * disassembly evidence): $a3(args)=0x00157C00, the same address
     * later read for argc. */
    run_setupthread_test(0x00295170u, 0xFFFFFFFFu, 0x100000, 0x00157C00u,
                          "SetupThread: SCPH-50004 decompression-stub args=0x00157C00 case");

    printf("\nTotal failures: %d\n", failures);
    return failures ? 1 : 0;
}
