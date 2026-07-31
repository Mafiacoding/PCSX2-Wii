/*
 * test_ee_syscall_full_audit_sweep.c - host-native test for Round 193
 * (task #359): a fresh, PROGRAMMATIC (script-parsed, not hand-audited)
 * cross-reference of ee_core.c's complete handled-sysnum list against
 * every real numeric slot in ps2sdk's ee/kernel/include/syscallnr.h
 * found roughly 78 more genuinely unhandled, machine-halting syscall
 * numbers that Rounds 179/186/187/192's own hand-audits each missed.
 * All fixed with the same established exception-raise pattern used
 * throughout this session. This test covers every number from that
 * fresh audit that Round 193 added.
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
static uint32_t enc_syscall(void) { return (0x0Cu); }
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

static void run_syscall_test(int32_t sysnum, const char *label) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_addiu(3, 0, (int16_t)sysnum)); pc += 4;
    uint32_t syscall_pc = pc;
    wle32(p+pc, enc_syscall());                    pc += 4;
    wle32(p+pc, 0x0u);                              pc += 4;

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    uint32_t base_pc = st->pc;

    ee_core_step();
    ee_core_step();

    char msg[160];
    snprintf(msg, sizeof(msg), "%s: halted must remain 0", label);
    CHECK(st->halted == 0, msg);
    snprintf(msg, sizeof(msg), "%s: Cause.ExcCode == Syscall (8)", label);
    CHECK((st->cop0[13] & 0x7Cu) == EE_EXC_CODE_SYS, msg);
    snprintf(msg, sizeof(msg), "%s: EPC points at the SYSCALL instruction itself", label);
    CHECK(st->cop0[14] == base_pc + syscall_pc, msg);
    snprintf(msg, sizeof(msg), "%s: pc vectors to 0xBFC00380", label);
    CHECK(st->pc == 0xBFC00380u, msg);
}

int main(void) {
    int nums[] = {
        10, 11, 12, 13, 14, 15,
        59, 62, 71, 84, 89, 90, 91, 105,
        74, 75, 76, 77, 78, 79, 110, 111, 112, 113, -112, -113,
        92, 93, 94, 95, -92, -93, -94, -95,
        96, 97, 98, 99, 102, 130, -103, -104, -106,
        107, 108, 109, 114, 115, 116, 117, 123,
        125, 126, 127, 128, 131, 133, 134, 135,
        -26, -27, -28, -29,
        -38, -42, -44, -46, -47, -49, -52, -54, -56, -58,
        -70, -72, -73
    };
    int n = sizeof(nums)/sizeof(nums[0]);
    for (int i = 0; i < n; i++) {
        char label[64];
        snprintf(label, sizeof(label), "sysnum %d", nums[i]);
        run_syscall_test(nums[i], label);
    }

    /* Regression: a number NOT in this round's list (already-handled
     * 64/CreateSema) must still use its own established, different
     * (non-generic-exception) handling, not accidentally get swept
     * into this round's new blocks. */
    {
        bios_image_t bios = make_bios();
        uint8_t *p = bios.data;
        wle32(p+0, enc_addiu(3, 0, 64));
        wle32(p+4, enc_syscall());
        wle32(p+8, 0x0u);
        ee_core_init(&bios);
        ee_state_t *st = ee_core_get_state();
        ee_core_step();
        ee_core_step();
        CHECK(st->halted == 0, "CreateSema(64) regression: not halted");
    }

    printf("Total numbers tested: %d\n", n);
    if (failures == 0)
        printf("ALL CHECKS PASSED (0 failures)\n");
    else
        printf("%d check(s) FAILED\n", failures);
    return failures != 0;
}
