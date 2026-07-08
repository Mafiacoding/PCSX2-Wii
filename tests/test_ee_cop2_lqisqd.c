/* test_ee_cop2_lqisqd.c - host-native test for Round 29 continued's
 * 22nd change: the remaining VU0 local-memory access family - VLQI
 * (idx52, load-quadword-increment), VLQD (idx54, load-quadword-
 * decrement), VSQD (idx55, store-quadword-decrement) - plus a
 * regression check that VSQI (idx53) now genuinely respects destmask
 * (a real bug found and fixed alongside this change: confirmed via a
 * real PCSX2 upstream reference clone's DisR5900asm.cpp that VSQI has
 * an xyzw suffix like every other CO-format op, but this project's
 * existing VSQI implementation was unconditionally writing all 4
 * lanes regardless of destmask).
 *
 * Field-role convention (ported from PCSX2's VUops.cpp _vuLQI/_vuLQD/
 * _vuSQD): for LOADS (VLQI/VLQD), the address VI register lives in
 * the FS field position and the destination VF register lives in the
 * FT field position - the OPPOSITE of VSQI/VSQD, where the address VI
 * register lives in FT and the source VF register lives in FS.
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
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_break(void)                       { return 0x0D; }

static uint32_t enc_qmtc2(int rt, int vf) { return (0x12u << 26) | (0x05u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vf << 11); }
static uint32_t enc_qmfc2(int rt, int vf) { return (0x12u << 26) | (0x01u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vf << 11); }
static uint32_t enc_mtc2(int rt, int vi)  { return (0x12u << 26) | (0x04u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vi << 11); }
static uint32_t enc_cfc2(int rt, int vi)  { return (0x12u << 26) | (0x02u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vi << 11); }

/* SPECIAL2 idx = (instr&0x3) | ((instr>>4)&0x7C); bits 2-5 forced to
 * 0xF for the outer funct dispatch. dest_ft_role holds the FT field
 * (16-20), src_fs_role holds the FS field (11-15) - which one is
 * "address" vs "data" depends on the specific opcode, per the header
 * comment above. */
static uint32_t enc_special2(uint32_t destmask, int ft_field, int fs_field, uint32_t idx)
{
    uint32_t f6_10 = (idx >> 2) & 0x1Fu;
    uint32_t f0_1 = idx & 0x3u;
    return (0x12u << 26) | ((0x10u | destmask) << 21) | ((uint32_t)ft_field << 16)
         | ((uint32_t)fs_field << 11) | (f6_10 << 6) | (0xFu << 2) | f0_1;
}

static void wle32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static float bits2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

int main(void)
{
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = calloc(1, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    uint8_t *prog = bios.data;
    int i = 0;

    /* r4 = 0x1000 (RAM base pointer) */
    wle32(prog + (i++)*4, enc_lui(4, 0x0000));
    wle32(prog + (i++)*4, enc_ori(4, 4, 0x1000));

    /* LQ r5 <- RAM[0x1000] (VF1: 2,3,4,5) */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
    wle32(prog + (i++)*4, enc_qmtc2(5, 1));

    /* r6 = 5 -> MTC2 into VI10 (address reg for VSQI/VLQI round-trip) */
    wle32(prog + (i++)*4, enc_ori(6, 0, 5));
    wle32(prog + (i++)*4, enc_mtc2(6, 10));

    /* VSQI.xyzw VF1,(VI10++): store all 4 lanes at quadword 5, VI10 -> 6 */
    wle32(prog + (i++)*4, enc_special2(0xF, 10, 1, 53));

    /* VSQI.x VF1,(VI10++) again at quadword 6 - only X lane, to prove
     * the destmask fix: quadword 6's Y/Z/W must stay 0. */
    wle32(prog + (i++)*4, enc_special2(0x8, 10, 1, 53));
    /* VI10 is now 7. CFC2 VI10 -> r7 to verify post-increment happened twice. */
    wle32(prog + (i++)*4, enc_cfc2(7, 10));

    /* r8 = 5 -> MTC2 into VI11 (address reg for VLQI) */
    wle32(prog + (i++)*4, enc_ori(8, 0, 5));
    wle32(prog + (i++)*4, enc_mtc2(8, 11));
    /* VLQI.xyzw VF2,(VI11++): load quadword 5 (the full VSQI store above) into VF2, VI11 -> 6 */
    wle32(prog + (i++)*4, enc_special2(0xF, 2, 11, 52));
    wle32(prog + (i++)*4, enc_qmfc2(9, 2));
    wle32(prog + (i++)*4, enc_cfc2(10, 11));

    /* r11 = 7 -> MTC2 into VI12 (address reg for VLQD - pre-decrement, so this loads quadword 6, the single-X-lane store) */
    wle32(prog + (i++)*4, enc_ori(11, 0, 7));
    wle32(prog + (i++)*4, enc_mtc2(11, 12));
    /* VLQD.xyzw VF3,(--VI12): pre-decrement VI12 to 6, load quadword 6 (the single-X-lane store) into VF3 */
    wle32(prog + (i++)*4, enc_special2(0xF, 3, 12, 54));
    wle32(prog + (i++)*4, enc_qmfc2(12, 3));
    wle32(prog + (i++)*4, enc_cfc2(13, 12));

    /* r14 = 10 -> MTC2 into VI13 (address reg for VSQD - pre-decrement, so this stores at quadword 9) */
    wle32(prog + (i++)*4, enc_ori(14, 0, 10));
    wle32(prog + (i++)*4, enc_mtc2(14, 13));
    /* VSQD.xyzw VF1,(--VI13): pre-decrement VI13 to 9, store VF1 (2,3,4,5) at quadword 9 */
    wle32(prog + (i++)*4, enc_special2(0xF, 13, 1, 55));
    wle32(prog + (i++)*4, enc_cfc2(15, 13));

    /* r16 = 9 -> MTC2 into VI14, then VLQI.xyzw VF4,(VI14++) to read back quadword 9 and confirm the VSQD store */
    wle32(prog + (i++)*4, enc_ori(16, 0, 9));
    wle32(prog + (i++)*4, enc_mtc2(16, 14));
    wle32(prog + (i++)*4, enc_special2(0xF, 4, 14, 52));
    wle32(prog + (i++)*4, enc_qmfc2(17, 4));

    wle32(prog + (i++)*4, enc_break());

    if (ee_core_init(&bios) != 0) { printf("ee_core_init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    /* VF1 = (2, 3, 4, 5) */
    wle32(st->ram + 0x1000, f2bits(2.0f));
    wle32(st->ram + 0x1004, f2bits(3.0f));
    wle32(st->ram + 0x1008, f2bits(4.0f));
    wle32(st->ram + 0x100C, f2bits(5.0f));

    ee_core_run(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK (VLQI/VLQD/VSQD/VSQI-destmask-fix recognized)");

    CHECK(st->gpr[7].ud0 == 7ULL, "VSQI post-incremented VI10 twice: 5 -> 6 -> 7");

    uint32_t vf2x = (uint32_t)(st->gpr[9].ud0 & 0xFFFFFFFFu);
    uint32_t vf2y = (uint32_t)(st->gpr[9].ud0 >> 32);
    uint32_t vf2z = (uint32_t)(st->gpr[9].ud1 & 0xFFFFFFFFu);
    uint32_t vf2w = (uint32_t)(st->gpr[9].ud1 >> 32);
    CHECK(bits2f(vf2x) == 2.0f && bits2f(vf2y) == 3.0f && bits2f(vf2z) == 4.0f && bits2f(vf2w) == 5.0f,
          "VLQI.xyzw VF2,(VI11++) loads back the full VSQI-stored VF1 from quadword 5: (2,3,4,5)");
    CHECK(st->gpr[10].ud0 == 6ULL, "VLQI post-incremented VI11: 5 -> 6");

    uint32_t vf3x = (uint32_t)(st->gpr[12].ud0 & 0xFFFFFFFFu);
    uint32_t vf3y = (uint32_t)(st->gpr[12].ud0 >> 32);
    CHECK(bits2f(vf3x) == 2.0f && vf3y == 0u,
          "VLQD.xyzw VF3,(--VI12) pre-decrements to 7 and loads the single-X-lane VSQI store: X=2 (from VF1.x), Y=0 (destmask fix - Y was never written)");
    CHECK(st->gpr[13].ud0 == 6ULL, "VLQD pre-decremented VI12: 7 -> 6");

    CHECK(st->gpr[15].ud0 == 9ULL, "VSQD pre-decremented VI13: 10 -> 9");

    uint32_t vf4x = (uint32_t)(st->gpr[17].ud0 & 0xFFFFFFFFu);
    uint32_t vf4y = (uint32_t)(st->gpr[17].ud0 >> 32);
    uint32_t vf4z = (uint32_t)(st->gpr[17].ud1 & 0xFFFFFFFFu);
    uint32_t vf4w = (uint32_t)(st->gpr[17].ud1 >> 32);
    CHECK(bits2f(vf4x) == 2.0f && bits2f(vf4y) == 3.0f && bits2f(vf4z) == 4.0f && bits2f(vf4w) == 5.0f,
          "VSQD's store at quadword 9 reads back correctly via VLQI: (2,3,4,5)");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
