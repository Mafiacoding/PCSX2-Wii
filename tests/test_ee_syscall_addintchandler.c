/*
 * test_ee_syscall_addintchandler.c - host-native test for Round 186
 * (task #352): EE syscall 16 (AddIntcHandler) / 17 (RemoveIntcHandler)
 * must vector as real MIPS Syscall exceptions (ExcCode 8), exactly
 * like the already-tested precedent for AddDmacHandler (18)/
 * RemoveDmacHandler (19) - NOT be silently bypassed or halt the
 * machine (the previous, incorrect behavior: these numbers fell
 * through to the generic "no BIOS syscall table implemented" halt()).
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

/* sysnum lives in $v1 (GPR 3), real EE convention (see ee_core.c's
 * own "int32_t sysnum = (int32_t)GPR(3);"). */
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
    run_syscall_test(16, "AddIntcHandler (16/0x10)");
    run_syscall_test(17, "RemoveIntcHandler (17/0x11)");

    /* Regression check: sysnum 18/19 (AddDmacHandler/RemoveDmacHandler)
     * must still behave identically to before this round's addition -
     * confirms the new 16/17 block doesn't shadow or break the
     * pre-existing 18/19 block right next to it. */
    run_syscall_test(18, "AddDmacHandler (18/0x12) - unaffected regression check");
    run_syscall_test(19, "RemoveDmacHandler (19/0x13) - unaffected regression check");

    if (failures == 0)
        printf("ALL CHECKS PASSED (0 failures)\n");
    else
        printf("%d check(s) FAILED\n", failures);
    return failures != 0;
}
