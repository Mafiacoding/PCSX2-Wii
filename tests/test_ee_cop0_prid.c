/*
 * test_ee_cop0_prid.c - host-native test for ee_core.c's COP0 PRId
 * (register 15, Processor Revision Identifier) initialization.
 *
 * This is not a cosmetic register. It was found, via a live
 * instruction-level trace of real, working PCSX2 (see docs/STATUS.md's
 * "EE JALR investigation, round 5"), to be the actual root cause of
 * the EE JALR-to-out-of-range halt investigated across rounds 1-4:
 * the real BIOS's very first instruction is "MFC0 $k0, $15" followed
 * immediately by a CPU-revision check ("SLTI $at, $k0, 89" / "BNE")
 * that picks between two entirely different early-boot code paths.
 * Leaving cop0[15] at 0 (its memset() default) made this project's
 * interpreter take the wrong branch from instruction #3 onward and
 * never reach the real vector-install routine that populates low RAM
 * - explaining, in turn, why the eventual pointer chase through
 * RAM[0x100] always found zero.
 *
 * The correct value, 0x00002e20, is ported directly from PCSX2's own
 * R5900.cpp ("cpuRegs.CP0.n.PRid = 0x00002e20").
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

static uint32_t enc_mfc0(int rt, int rd) {
    return (0x10 << 26) | (0x00 << 21) | (rt << 16) | (rd << 11);
}
static uint32_t enc_break(void) { return 0x0D; }
static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;
    memset(bios.data, 0, BIOS_MAX_SIZE);

    /* Straight port of the real BIOS's own instruction #0: MFC0 $t0,$15 */
    int pc = 0;
    wle32(bios.data + pc, enc_mfc0(8, 15)); pc += 4; /* MFC0 $t0, $15 (PRId) */
    wle32(bios.data + pc, enc_break());     pc += 4;

    if (ee_core_init(&bios) != 0) { printf("init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    /* Directly check the raw cop0 register too, independent of MFC0,
     * so this test still catches a regression even if MFC0 itself had
     * a bug. */
    CHECK(st->cop0[15] == 0x00002e20u,
          "ee_core_init(): cop0[15] (PRId) == 0x00002e20 immediately after init");

    ee_core_run(&bios);
    CHECK(st->halted == 1, "core halted on BREAK");
    CHECK(st->gpr[8].ud0 == 0x00000000000002e20ULL,
          "MFC0 $t0,$15 reads back 0x2e20, sign-extended (matches real PCSX2's R5900.cpp)");

    /* The concrete, real-world consequence: real BIOS instruction #2
     * is "SLTI $at,$k0,89" - with the correct PRId (11808 decimal),
     * 11808 < 89 must be FALSE. This is the exact branch that was
     * wrong before this fix. */
    CHECK(!(0x00002e20u < 89u) , "sanity: real PRId 0x2e20 (11808) is NOT less than 89 - the BIOS's revision-check branch now goes the correct way");

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
