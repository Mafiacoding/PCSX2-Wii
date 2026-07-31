/*
 * test_ee_mmi_hilo2.c - host-native test for ee_core.c's Round 64
 * addition: the 19 previously-unimplemented real MMI-family opcodes
 * (MADD1/MADDU1/PMFHL/PMTHL at the SPECIAL2 top level; QFSRV in MMI1;
 * PMADDW/PMSUBW/PMULTW/PDIVW/PMADDH/PHMADH/PMSUBH/PHMSBH/PMULTH/PDIVBW
 * in MMI2; PMADDUW/PSRAVW/PMULTUW/PDIVUW in MMI3), all ported from
 * PCSX2 v1.6.0's pcsx2/MMI.cpp with encodings cross-checked against
 * the same tag's pcsx2/R5900OpcodeTables.cpp. Test setup pokes
 * st->lo/st->hi/st->sa_reg directly from C (legitimate test-harness
 * setup, same spirit as this file's siblings poking st->ram directly
 * via wle32()) rather than emulating separate MTHI/MTLO/MTSA
 * instructions first, to keep each case isolated and easy to verify
 * by hand.
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

static void run_until_break(void) {
    ee_state_t *st = ee_core_get_state();
    long guard;
    for (guard = 0; guard < 2000000L; guard++) {
        if (ee_core_step()) return;
        if (((st->cop0[13] >> 2) & 0x1Fu) == 9u && (st->cop0[12] & 0x2u) != 0u) {
            st->halted = 1;
            snprintf(st->halt_reason, sizeof(st->halt_reason), "BREAK");
            return;
        }
    }
    st->halted = 1;
    snprintf(st->halt_reason, sizeof(st->halt_reason), "safety cap reached");
}

static uint32_t enc_lui(int rt, uint16_t imm)        { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_lq(int rt, int rs, int16_t imm)  { return (0x1E << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_mmi(int rs, int rt, int rd, int sa, int funct) {
    return (0x1C << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}
static uint32_t enc_break(void) { return 0x0D; }
static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }
static void wle16(uint8_t *p, uint16_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF; }

static void build_prog(uint8_t *prog, int sa, int funct) {
    int pc = 0;
    wle32(prog + pc, enc_lui(4, 0x8000)); pc += 4;
    wle32(prog + pc, enc_ori(4, 4, 0x1000)); pc += 4;
    wle32(prog + pc, enc_lq(1, 4, 0x00)); pc += 4;
    wle32(prog + pc, enc_lq(2, 4, 0x10)); pc += 4;
    wle32(prog + pc, enc_mmi(1, 2, 3, sa, funct)); pc += 4;
    wle32(prog + pc, enc_break()); pc += 4;
}

#define FUNCT_MMI1 0x28
#define FUNCT_MMI2 0x09
#define FUNCT_MMI3 0x29

static bios_image_t bios;
static ee_state_t *st;

static void fresh_init(void) {
    memset(bios.data, 0, BIOS_MAX_SIZE);
    if (ee_core_init(&bios) != 0) { printf("init failed\n"); exit(1); }
    st = ee_core_get_state();
}

int main(void) {
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;
    uint8_t *prog = bios.data;

    fresh_init();
    build_prog(prog, 0, 0x20);
    wle32(st->ram + 0x1000, 5); wle32(st->ram + 0x1010, 3);
    run_until_break();
    CHECK(st->gpr[3].ud0 == 15 && st->lo.ud1 == 15 && st->hi.ud1 == 0,
          "MADD1: 5*3+0 == 15 in pipe-1 LO, HI stays 0, GPR3=15");

    fresh_init();
    build_prog(prog, 0, 0x21);
    wle32(st->ram + 0x1000, 0x80000000u); wle32(st->ram + 0x1010, 2u);
    run_until_break();
    CHECK(st->lo.ud1 == 0 && st->hi.ud1 == 1,
          "MADDU1: 0x80000000*2 == 0x100000000, LO(pipe1)=0, HI(pipe1)=1");

    fresh_init();
    build_prog(prog, 0x00, 0x30);
    st->lo.ud0 = 0x1111111122222222ULL; st->hi.ud0 = 0x3333333344444444ULL;
    st->lo.ud1 = 0x5555555566666666ULL; st->hi.ud1 = 0x7777777788888888ULL;
    run_until_break();
    CHECK(st->gpr[3].ud0 == 0x4444444422222222ULL,
          "PMFHL LW word0/1: LO.ud0 low32 | HI.ud0 low32<<32");
    CHECK(st->gpr[3].ud1 == 0x8888888866666666ULL,
          "PMFHL LW word2/3: LO.ud1 low32 | HI.ud1 low32<<32");

    fresh_init();
    build_prog(prog, 0x02, 0x30);
    st->lo.ud0 = 0x00000000AAAAAAAAULL; st->hi.ud0 = 0x0000000000000001ULL;
    st->lo.ud1 = 0xFFFFFFFFFFFFFFF6ULL; st->hi.ud1 = 0xFFFFFFFFFFFFFFFFULL;
    run_until_break();
    CHECK(st->gpr[3].ud0 == 0x000000007FFFFFFFULL,
          "PMFHL SLW: pipe0 overflow clamps to INT32_MAX (0x7fffffff)");
    CHECK(st->gpr[3].ud1 == 0xFFFFFFFFFFFFFFF6ULL,
          "PMFHL SLW: pipe1 in-range value (-10) passes through sign-extended");

    fresh_init();
    build_prog(prog, 0x00, 0x31);
    st->lo.ud0 = 0xDEADBEEF00000000ULL; st->hi.ud0 = 0xCAFEF00D00000000ULL;
    st->lo.ud1 = 0x1234567800000000ULL; st->hi.ud1 = 0x8765432100000000ULL;
    wle32(st->ram + 0x1000, 0x11111111u);
    wle32(st->ram + 0x1004, 0x22222222u);
    wle32(st->ram + 0x1008, 0x33333333u);
    wle32(st->ram + 0x100C, 0x44444444u);
    run_until_break();
    CHECK(st->lo.ud0 == 0xDEADBEEF11111111ULL, "PMTHL: lo.ud0 low32 replaced, upper 32 untouched");
    CHECK(st->hi.ud0 == 0xCAFEF00D22222222ULL, "PMTHL: hi.ud0 low32 replaced, upper 32 untouched");
    CHECK(st->lo.ud1 == 0x1234567833333333ULL, "PMTHL: lo.ud1 low32 replaced, upper 32 untouched");
    CHECK(st->hi.ud1 == 0x8765432144444444ULL, "PMTHL: hi.ud1 low32 replaced, upper 32 untouched");

    fresh_init();
    build_prog(prog, 0x1B, FUNCT_MMI1);
    st->sa_reg = 0;
    wle32(st->ram + 0x1000, 0xAAAAAAAAu); wle32(st->ram + 0x1004, 0xBBBBBBBBu);
    wle32(st->ram + 0x1008, 0xCCCCCCCCu); wle32(st->ram + 0x100C, 0xDDDDDDDDu);
    wle32(st->ram + 0x1010, 0x11111111u); wle32(st->ram + 0x1014, 0x22222222u);
    wle32(st->ram + 0x1018, 0x33333333u); wle32(st->ram + 0x101C, 0x44444444u);
    run_until_break();
    CHECK(st->gpr[3].ud0 == 0x2222222211111111ULL && st->gpr[3].ud1 == 0x4444444433333333ULL,
          "QFSRV sa_reg=0: Rd == Rt (pure copy, no shift)");

    fresh_init();
    build_prog(prog, 0x1B, FUNCT_MMI1);
    st->sa_reg = 4;
    wle32(st->ram + 0x1000, 0x11111111u); wle32(st->ram + 0x1004, 0x22222222u);
    wle32(st->ram + 0x1008, 0x33333333u); wle32(st->ram + 0x100C, 0x44444444u);
    wle32(st->ram + 0x1010, 0xAAAAAAAAu); wle32(st->ram + 0x1014, 0xBBBBBBBBu);
    wle32(st->ram + 0x1018, 0xCCCCCCCCu); wle32(st->ram + 0x101C, 0xDDDDDDDDu);
    run_until_break();
    CHECK(st->gpr[3].ud0 == 0xCCCCCCCCBBBBBBBBULL,
          "QFSRV sa_reg=4 (sa_amt=32): ud0 == (Rt.ud0>>32)|(Rt.ud1<<32)");
    CHECK(st->gpr[3].ud1 == 0x11111111DDDDDDDDULL,
          "QFSRV sa_reg=4 (sa_amt=32): ud1 == (Rt.ud1>>32)|(Rs.ud0<<32)");

    fresh_init();
    build_prog(prog, 0x00, FUNCT_MMI2);
    wle32(st->ram + 0x1000, 3); wle32(st->ram + 0x1008, 100);
    wle32(st->ram + 0x1010, 4); wle32(st->ram + 0x1018, 5);
    run_until_break();
    CHECK(st->gpr[3].ud0 == 12 && st->gpr[3].ud1 == 500 && st->lo.ud0 == 12 && st->lo.ud1 == 500,
          "PMADDW: 3*4+0=12 (pipe0), 100*5+0=500 (pipe1)");

    fresh_init();
    build_prog(prog, 0x04, FUNCT_MMI2);
    st->lo.ud0 = 50; st->lo.ud1 = 1000;
    wle32(st->ram + 0x1000, 3); wle32(st->ram + 0x1008, 10);
    wle32(st->ram + 0x1010, 4); wle32(st->ram + 0x1018, 5);
    run_until_break();
    CHECK(st->gpr[3].ud0 == 38 && st->gpr[3].ud1 == 950,
          "PMSUBW: 50-(3*4)=38 (pipe0), 1000-(10*5)=950 (pipe1)");

    fresh_init();
    build_prog(prog, 0x0C, FUNCT_MMI2);
    wle32(st->ram + 0x1000, 6); wle32(st->ram + 0x1008, (uint32_t)-3);
    wle32(st->ram + 0x1010, 7); wle32(st->ram + 0x1018, 9);
    run_until_break();
    CHECK(st->gpr[3].ud0 == 42, "PMULTW pipe0: 6*7 == 42");
    CHECK(st->gpr[3].ud1 == (uint64_t)(int64_t)-27, "PMULTW pipe1: -3*9 == -27, sign-extended 64-bit");

    fresh_init();
    build_prog(prog, 0x0D, FUNCT_MMI2);
    wle32(st->ram + 0x1000, 17); wle32(st->ram + 0x1008, (uint32_t)-7);
    wle32(st->ram + 0x1010, 5);  wle32(st->ram + 0x1018, 2);
    run_until_break();
    CHECK(st->lo.ud0 == 3 && st->hi.ud0 == 2, "PDIVW pipe0: 17/5==3 rem 2");
    CHECK(st->lo.ud1 == (uint64_t)(int64_t)-3 && st->hi.ud1 == (uint64_t)(int64_t)-1,
          "PDIVW pipe1: -7/2==-3 rem -1 (truncate toward zero)");

    fresh_init();
    build_prog(prog, 0x10, FUNCT_MMI2);
    for (int i = 0; i < 4; i++) { wle32(st->ram + 0x1000 + i*4, 0x00010001u); wle32(st->ram + 0x1010 + i*4, 0x00010001u); }
    run_until_break();
    CHECK(st->gpr[3].ud0 == 0x0000000100000001ULL && st->gpr[3].ud1 == 0x0000000100000001ULL,
          "PMADDH: all 8 lanes 1*1+0 == 1, GPR result word0..3 all == 1");

    fresh_init();
    build_prog(prog, 0x11, FUNCT_MMI2);
    wle16(st->ram + 0x1000, 2); wle16(st->ram + 0x1002, 3);
    wle16(st->ram + 0x1010, 2); wle16(st->ram + 0x1012, 3);
    run_until_break();
    CHECK(st->gpr[3].ud0 == 13, "PHMADH: word0 == 9 (3*3) + 4 (2*2) == 13");

    fresh_init();
    build_prog(prog, 0x14, FUNCT_MMI2);
    st->lo.ud0 = 100;
    wle16(st->ram + 0x1000, 4); wle16(st->ram + 0x1010, 5);
    run_until_break();
    CHECK((int32_t)(uint32_t)st->gpr[3].ud0 == 80, "PMSUBH: 100 - (4*5) == 80 (word0)");

    fresh_init();
    build_prog(prog, 0x15, FUNCT_MMI2);
    wle16(st->ram + 0x1000, 2); wle16(st->ram + 0x1002, 3);
    wle16(st->ram + 0x1010, 2); wle16(st->ram + 0x1012, 3);
    run_until_break();
    CHECK((int32_t)(uint32_t)st->gpr[3].ud0 == (9 - 4), "PHMSBH: word0 == 9-4 == 5");

    fresh_init();
    build_prog(prog, 0x1C, FUNCT_MMI2);
    for (int i = 0; i < 4; i++) { wle32(st->ram + 0x1000 + i*4, 0x00030002u); wle32(st->ram + 0x1010 + i*4, 0x00050004u); }
    run_until_break();
    CHECK(st->gpr[3].ud0 == 0x0000000800000008ULL,
          "PMULTH: GPR word0=lane0(2*4)=8, word1=lane2(2*4)=8 (odd lanes 1/3 never reach GPR, real hardware quirk)");
    CHECK(st->lo.ud0 == (((uint64_t)15u << 32) | 8u),
          "PMULTH: LO.ud0 == lane1(3*5=15)<<32 | lane0(2*4=8) - the full pair IS captured in LO/HI, just not GPR");

    fresh_init();
    build_prog(prog, 0x1D, FUNCT_MMI2);
    wle32(st->ram + 0x1000, 20); wle32(st->ram + 0x1004, 21);
    wle32(st->ram + 0x1008, 22); wle32(st->ram + 0x100C, 23);
    wle16(st->ram + 0x1010, 5);
    run_until_break();
    CHECK(lane_w(st->lo, 0) == 4 && lane_w(st->hi, 0) == 0, "PDIVBW lane0: 20/5==4 rem 0");
    CHECK(lane_w(st->lo, 1) == 4 && lane_w(st->hi, 1) == 1, "PDIVBW lane1: 21/5==4 rem 1");
    CHECK(lane_w(st->lo, 3) == 4 && lane_w(st->hi, 3) == 3, "PDIVBW lane3: 23/5==4 rem 3 (same broadcast divisor)");

    fresh_init();
    build_prog(prog, 0x00, FUNCT_MMI3);
    wle32(st->ram + 0x1000, 0xFFFFFFFFu); wle32(st->ram + 0x1008, 2u);
    wle32(st->ram + 0x1010, 2u); wle32(st->ram + 0x1018, 3u);
    run_until_break();
    CHECK(st->lo.ud0 == (uint64_t)(int64_t)(int32_t)0xFFFFFFFEu && st->hi.ud0 == 1,
          "PMADDUW pipe0: 0xFFFFFFFF*2 == 0x1FFFFFFFE (lo=0xFFFFFFFE sign-ext, hi=1)");
    CHECK(st->lo.ud1 == 6 && st->hi.ud1 == 0, "PMADDUW pipe1: 2*3+0 == 6");

    fresh_init();
    build_prog(prog, 0x03, FUNCT_MMI3);
    wle32(st->ram + 0x1000, 4); wle32(st->ram + 0x1008, 2);
    wle32(st->ram + 0x1010, 0x80000000u); wle32(st->ram + 0x1018, 16u);
    run_until_break();
    CHECK(st->gpr[3].ud0 == (uint64_t)(int64_t)(int32_t)0xF8000000u,
          "PSRAVW: 0x80000000 >> 4 (arithmetic) == 0xF8000000, sign-extended");
    CHECK(st->gpr[3].ud1 == 4, "PSRAVW: 16 >> 2 == 4");

    fresh_init();
    build_prog(prog, 0x0C, FUNCT_MMI3);
    wle32(st->ram + 0x1000, 0xFFFFFFFFu); wle32(st->ram + 0x1008, 6u);
    wle32(st->ram + 0x1010, 2u); wle32(st->ram + 0x1018, 7u);
    run_until_break();
    CHECK(st->gpr[3].ud0 == 0x1FFFFFFFEULL, "PMULTUW pipe0 GPR: full unsigned 64-bit product 0xFFFFFFFF*2 == 0x1FFFFFFFE (NOT sign-extended low32)");
    CHECK(st->lo.ud0 == (uint64_t)(int64_t)(int32_t)0xFFFFFFFEu, "PMULTUW pipe0 LO: low32 of the product (0xFFFFFFFE) sign-extended");
    CHECK(st->gpr[3].ud1 == 42, "PMULTUW pipe1: 6*7 == 42");

    fresh_init();
    build_prog(prog, 0x0D, FUNCT_MMI3);
    wle32(st->ram + 0x1000, 20); wle32(st->ram + 0x1008, 7);
    wle32(st->ram + 0x1010, 6);  wle32(st->ram + 0x1018, 0);
    run_until_break();
    CHECK(st->lo.ud0 == 3 && st->hi.ud0 == 2, "PDIVUW pipe0: 20/6==3 rem 2");
    CHECK(st->lo.ud1 == (uint64_t)(int64_t)-1 && st->hi.ud1 == 7,
          "PDIVUW pipe1 div-by-zero: LO==-1 (0xFFFFFFFF...), HI==dividend (7)");

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
