/*
 * test_ee_cop0_special.c - host-native test for ee_core.c's COP0
 * "CO"-format instructions: RFE, ERET, EI, DI (see the COP0 case in
 * ee_core.c for the full scope/reference notes - these were added
 * after real BIOS testing (SCPH-10000, see docs/STATUS.md) showed
 * the EE's interpreter halting almost immediately without them; the
 * EE went from halting after ~99K real BIOS instructions to running
 * past 5 MILLION without hitting another unimplemented opcode once
 * these were added).
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
static uint32_t enc_rfe(void)  { return 0x42000010u; }
static uint32_t enc_eret(void) { return 0x42000018u; }
static uint32_t enc_ei(void)   { return 0x42000038u; }
static uint32_t enc_di(void)   { return 0x42000039u; }
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

static void test_rfe(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    /* Status = 0x19 (0b011001) */
    wle32(p+pc, enc_addiu(1, 0, 0x19)); pc += 4;
    wle32(p+pc, enc_mtc0(1, 12)); pc += 4;
    wle32(p+pc, enc_rfe()); pc += 4;
    wle32(p+pc, enc_mfc0(2, 12)); pc += 4; /* r2 = Status after RFE */
    wle32(p+pc, enc_break()); pc += 4;

    ee_core_init(&bios);
    ee_core_run(&bios);
    ee_state_t *st = ee_core_get_state();

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL, "RFE test: core halted cleanly on BREAK");
    /* expected: (0x19 & ~0x0F) | ((0x19 >> 2) & 0x0F) = 0x10 | 0x06 = 0x16 */
    CHECK((st->gpr[2].ud0 & 0x3F) == 0x16u, "RFE shifted the Status KU/IE stack right by 2 correctly (0x19 -> 0x16)");
}

static void test_eret(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    /* r1 = 0xBFC00100 (ERET target, within BIOS space) */
    wle32(p+pc, enc_lui(1, 0xBFC0)); pc += 4;
    wle32(p+pc, enc_ori(1, 1, 0x0100)); pc += 4;
    wle32(p+pc, enc_mtc0(1, 14)); pc += 4;       /* EPC = 0xBFC00100 */
    wle32(p+pc, enc_addiu(2, 0, 0x2)); pc += 4;  /* r2 = 0x2 (EXL bit) */
    wle32(p+pc, enc_mtc0(2, 12)); pc += 4;       /* Status = 0x2 (EXL=1, ERL=0) */
    wle32(p+pc, enc_eret()); pc += 4;            /* no delay slot: jumps straight to EPC */
    /* if ERET incorrectly had a delay slot, this next instruction
     * would execute before the jump - make it something obviously
     * wrong (r9=0xDEAD) so a bug here is loud, not silent */
    wle32(p+pc, enc_addiu(9, 0, 0x0DEAD & 0x7FFF)); pc += 4;

    /* target: offset 0x100 */
    wle32(p + 0x100, enc_mfc0(3, 12)); /* r3 = Status (to check EXL got cleared) */
    wle32(p + 0x104, enc_break());

    ee_core_init(&bios);
    ee_core_run(&bios);
    ee_state_t *st = ee_core_get_state();

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL, "ERET test: core halted cleanly on BREAK (control reached the EPC target)");
    CHECK((st->gpr[3].ud0 & 0x2u) == 0u, "ERET cleared Status.EXL after taking the EPC branch");
    CHECK(st->gpr[9].ud0 == 0u, "ERET has NO branch delay slot - the instruction after it never executed");
}

static void test_ei_di(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    /* Status starts at 0 (KSU=0, gate condition satisfied) */
    wle32(p+pc, enc_ei()); pc += 4;
    wle32(p+pc, enc_mfc0(1, 12)); pc += 4;  /* r1 = Status after EI */
    wle32(p+pc, enc_di()); pc += 4;
    wle32(p+pc, enc_mfc0(2, 12)); pc += 4;  /* r2 = Status after DI */
    /* now set KSU=1 (bits 3-4 = 01, i.e. user mode) with _EDI/EXL/ERL
     * all clear - EI should now be gated OFF and NOT set EIE */
    wle32(p+pc, enc_addiu(3, 0, 0x08)); pc += 4; /* 0x08 = bit 3 (KSU low bit) */
    wle32(p+pc, enc_mtc0(3, 12)); pc += 4;
    wle32(p+pc, enc_ei()); pc += 4;
    wle32(p+pc, enc_mfc0(4, 12)); pc += 4;  /* r4 = Status after gated-off EI */
    wle32(p+pc, enc_break()); pc += 4;

    ee_core_init(&bios);
    ee_core_run(&bios);
    ee_state_t *st = ee_core_get_state();

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL, "EI/DI test: core halted cleanly on BREAK");
    CHECK((st->gpr[1].ud0 & 0x10000u) != 0u, "EI set Status.EIE (bit 16) when the gate condition (KSU==0) was satisfied");
    CHECK((st->gpr[2].ud0 & 0x10000u) == 0u, "DI cleared Status.EIE again");
    CHECK((st->gpr[4].ud0 & 0x10000u) == 0u, "EI did NOT set Status.EIE when gated off (KSU!=0, no _EDI/EXL/ERL)");
}

int main(void) {
    test_rfe();
    test_eret();
    test_ei_di();
    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
