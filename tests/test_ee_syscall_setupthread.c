/*
 * test_ee_syscall_setupthread.c - Round 274 (task #423, 315th
 * finding): host-native test for SetupThread (syscall 60/0x3C)'s
 * real, cited $a1==0xFFFFFFFF (-1) sentinel handling.
 *
 * Real, live-traced evidence (docs/STATUS.md's 315th finding): the
 * real BIOS's own OSDSYS ELF genuinely calls SetupThread with
 * $a1(stack_base)=0xFFFFFFFF and $a2(stack_size)=0x5000 (20480) at
 * its real entry (0x00200064). This project's previous plain
 * "stack_base + stack_size" arithmetic (Round 171) did not
 * special-case this value: the unsigned 32-bit add overflows and
 * wraps to a near-zero address (0x00004FF0 after 16-byte alignment),
 * which then genuinely faults (real AdES exception) the moment
 * OSDSYS's own compiled code tries to use it as a stack pointer -
 * confirmed via a live host-native diagnostic trace, not inferred.
 *
 * This test covers both the ordinary (unaffected) case and the new
 * sentinel-substitution case, confirming: (a) normal finite
 * stack_base/stack_size pairs are computed exactly as before (no
 * regression), (b) the 0xFFFFFFFF sentinel now produces a safe,
 * high-RAM, 16-byte-aligned stack top instead of a wrapped near-zero
 * address, and (c) neither case raises a spurious exception (syscall
 * 60 is a real, direct-return syscall, not one that vectors).
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

/* Sets up $v1=60 (SetupThread) then SYSCALL, with $a0/$a1/$a2 poked
 * directly beforehand (real EE calling convention: $a0=gp,
 * $a1=stack_base, $a2=stack_size). */
static void run_setupthread_test(uint32_t gp, uint32_t stack_base, int32_t stack_size,
                                  uint32_t expect_sp_top, const char *label) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_addiu(3, 0, 60));                pc += 4; /* $v1 = 60 */
    wle32(p+pc, enc_syscall());                       pc += 4; /* SYSCALL */
    wle32(p+pc, 0x0u);                                pc += 4; /* delay slot: NOP */

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();

    st->gpr[4].ud0 = gp;         /* $a0 */
    st->gpr[5].ud0 = stack_base; /* $a1 */
    st->gpr[6].ud0 = (uint32_t)stack_size; /* $a2 */

    ee_core_step(); /* ADDIU $v1, 60 */
    ee_core_step(); /* SYSCALL - direct-return, must NOT vector */

    char msg[160];
    snprintf(msg, sizeof(msg), "%s: halted must remain 0", label);
    CHECK(st->halted == 0, msg);

    snprintf(msg, sizeof(msg), "%s: does NOT raise a real exception (direct-return syscall)", label);
    CHECK((st->cop0[13] & 0x7Cu) != EE_EXC_CODE_SYS, msg);

    snprintf(msg, sizeof(msg), "%s: $gp set from $a0", label);
    CHECK(st->gpr[28].ud0 == gp, msg);

    snprintf(msg, sizeof(msg), "%s: $v0 (sp_top) == 0x%08X", label, expect_sp_top);
    CHECK((uint32_t)st->gpr[2].ud0 == expect_sp_top, msg);

    snprintf(msg, sizeof(msg), "%s: returned sp_top is 16-byte aligned", label);
    CHECK(((uint32_t)st->gpr[2].ud0 & 0xFu) == 0u, msg);
}

int main(void) {
    /* Ordinary, finite stack_base/stack_size - unaffected by the
     * Round 274 fix, must compute exactly as before (no regression). */
    run_setupthread_test(0x00295170u, 0x00300000u, 0x2000,
                          (uint32_t)((0x00300000u + 0x2000u) & ~0xFu),
                          "SetupThread: ordinary finite stack_base");

    /* Zero stack_base, small stack_size - another ordinary case,
     * confirms the sentinel check is exact-match only (0xFFFFFFFF),
     * not a "small value" heuristic. */
    run_setupthread_test(0x00100000u, 0x00010000u, 0x1000,
                          (uint32_t)((0x00010000u + 0x1000u) & ~0xFu),
                          "SetupThread: small finite stack_base");

    /* The real, live-traced OSDSYS case: stack_base=0xFFFFFFFF (-1),
     * stack_size=0x5000 (20480) - must NOT overflow-wrap to a
     * near-zero address; must produce the safe EE_RAM_SIZE-derived
     * default instead. */
    run_setupthread_test(0x00295170u, 0xFFFFFFFFu, 0x5000,
                          (uint32_t)((EE_RAM_SIZE - 0x10000u) & ~0xFu),
                          "SetupThread: OSDSYS's real -1 sentinel stack_base");

    /* Same sentinel with a different stack_size - the substituted
     * default must be independent of stack_size (the sentinel means
     * "kernel picks", so stack_size is not used to compute it). */
    run_setupthread_test(0x00295170u, 0xFFFFFFFFu, 0x100,
                          (uint32_t)((EE_RAM_SIZE - 0x10000u) & ~0xFu),
                          "SetupThread: -1 sentinel, different stack_size, same safe default");

    printf("\nTotal failures: %d\n", failures);
    return failures ? 1 : 0;
}
