/*
 * test_ee_fpu3.c - host-native test for ee_core.c's FPU accumulator
 * (ACC) opcode family: ADDA.S, SUBA.S, MULA.S, MADD.S, MSUB.S,
 * MADDA.S, MSUBA.S. Ported from PCSX2's FPU.cpp - see the case
 * comments in ee_core.c for the exact reference, including the real
 * hardware/PCSX2 quirk this test specifically tries to make
 * observable: MADD.S/MSUB.S re-run the intermediate fs*ft product
 * through fpuDouble() a second time before combining with ACC, but
 * MADDA.S/MSUBA.S do not.
 *
 * There is no MIPS instruction that reads ACC directly - real
 * hardware only exposes it via MADD.S/MSUB.S/MADDA.S/MSUBA.S
 * themselves. This test reads ACC back out by following it with a
 * MADD.S(fd, fzero, fzero), which computes fd = ACC + (0*0) =
 * fpuDouble(ACC), a value-preserving readback for any ACC value that
 * doesn't need underflow/denormal clamping (all values used here are
 * ordinary/finite except in the last test, which deliberately checks
 * the +Fmax-clamped-infinity case).
 */
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include "core/ee/ee_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t enc_lui(int rt, uint16_t imm) { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_mtc1(int rt, int fs) { return (0x11 << 26) | (0x04 << 21) | (rt << 16) | (fs << 11); }
static uint32_t enc_mfc1(int rt, int fs) { return (0x11 << 26) | (0x00 << 21) | (rt << 16) | (fs << 11); }
static uint32_t enc_cop1_s(int funct, int fd, int fs, int ft) {
    return (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
}
static uint32_t enc_break(void) { return 0x0D; }
static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }
static uint32_t float_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static float bits_float(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }

static void load_float(uint8_t *p, int *pc, int greg, int freg, float val) {
    uint32_t bits = float_bits(val);
    wle32(p + *pc, enc_lui(greg, (uint16_t)(bits >> 16))); *pc += 4;
    wle32(p + *pc, enc_ori(greg, greg, (uint16_t)(bits & 0xFFFF))); *pc += 4;
    wle32(p + *pc, enc_mtc1(greg, freg)); *pc += 4;
}

/* funct values, from ee_core.c / PCSX2's tbl_COP1_S */
#define F_ADDA  0x18
#define F_SUBA  0x19
#define F_MULA  0x1A
#define F_MADD  0x1C
#define F_MSUB  0x1D
#define F_MADDA 0x1E
#define F_MSUBA 0x1F

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    uint8_t *p = bios.data;
    int pc;

    /* f9 is used as a "0.0" register throughout, for ACC readback via
     * MADD.S(fd, f9, f9) => fd = ACC + (0*0) = fpuDouble(ACC). */

    /* --- Test 1: ADDA.S. ACC = 3.0 + 4.0 = 7.0. */
    pc = 0;
    load_float(p, &pc, 1, 1, 3.0f);
    load_float(p, &pc, 2, 2, 4.0f);
    load_float(p, &pc, 3, 9, 0.0f);
    wle32(p+pc, enc_cop1_s(F_ADDA, 0, 1, 2)); pc += 4;      /* ACC = f1 + f2 */
    wle32(p+pc, enc_cop1_s(F_MADD, 5, 9, 9)); pc += 4;      /* f5 = ACC readback */
    wle32(p+pc, enc_mfc1(3, 5)); pc += 4;
    wle32(p+pc, enc_break()); pc += 4;
    if (ee_core_init(&bios) != 0) { printf("init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();
    ee_core_run(&bios);
    CHECK(st->halted == 1, "ADDA.S test: core halted on BREAK");
    float acc1 = bits_float((uint32_t)st->gpr[3].ud0);
    CHECK(acc1 > 6.9999f && acc1 < 7.0001f, "ADDA.S: ACC = 3.0 + 4.0 == 7.0");

    /* --- Test 2: SUBA.S. ACC = 10.0 - 4.0 = 6.0. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 10.0f);
    load_float(p, &pc, 2, 2, 4.0f);
    load_float(p, &pc, 3, 9, 0.0f);
    wle32(p+pc, enc_cop1_s(F_SUBA, 0, 1, 2)); pc += 4;
    wle32(p+pc, enc_cop1_s(F_MADD, 5, 9, 9)); pc += 4;
    wle32(p+pc, enc_mfc1(3, 5)); pc += 4;
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    float acc2 = bits_float((uint32_t)st->gpr[3].ud0);
    CHECK(acc2 > 5.9999f && acc2 < 6.0001f, "SUBA.S: ACC = 10.0 - 4.0 == 6.0");

    /* --- Test 3: MULA.S. ACC = 3.0 * 5.0 = 15.0. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 3.0f);
    load_float(p, &pc, 2, 2, 5.0f);
    load_float(p, &pc, 3, 9, 0.0f);
    wle32(p+pc, enc_cop1_s(F_MULA, 0, 1, 2)); pc += 4;
    wle32(p+pc, enc_cop1_s(F_MADD, 5, 9, 9)); pc += 4;
    wle32(p+pc, enc_mfc1(3, 5)); pc += 4;
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    float acc3 = bits_float((uint32_t)st->gpr[3].ud0);
    CHECK(acc3 > 14.9999f && acc3 < 15.0001f, "MULA.S: ACC = 3.0 * 5.0 == 15.0");

    /* --- Test 4: MADD.S. Preset ACC = 2.0 (via ADDA.S(2.0, 0.0)),
     * then MADD.S(fd, 3.0, 4.0) => fd = ACC + (3.0*4.0) = 2 + 12 = 14. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 2.0f);
    load_float(p, &pc, 2, 9, 0.0f);
    load_float(p, &pc, 3, 3, 3.0f);
    load_float(p, &pc, 4, 4, 4.0f);
    wle32(p+pc, enc_cop1_s(F_ADDA, 0, 1, 9)); pc += 4;      /* ACC = 2.0 + 0.0 */
    wle32(p+pc, enc_cop1_s(F_MADD, 5, 3, 4)); pc += 4;      /* f5 = ACC + 3.0*4.0 */
    wle32(p+pc, enc_mfc1(3, 5)); pc += 4;
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    float madd_result = bits_float((uint32_t)st->gpr[3].ud0);
    CHECK(madd_result > 13.9999f && madd_result < 14.0001f, "MADD.S: fd = ACC(2.0) + 3.0*4.0 == 14.0");

    /* --- Test 5: MSUB.S. Preset ACC = 20.0, then MSUB.S(fd, 3.0, 4.0)
     * => fd = ACC - (3.0*4.0) = 20 - 12 = 8. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 20.0f);
    load_float(p, &pc, 2, 9, 0.0f);
    load_float(p, &pc, 3, 3, 3.0f);
    load_float(p, &pc, 4, 4, 4.0f);
    wle32(p+pc, enc_cop1_s(F_ADDA, 0, 1, 9)); pc += 4;      /* ACC = 20.0 */
    wle32(p+pc, enc_cop1_s(F_MSUB, 5, 3, 4)); pc += 4;      /* f5 = ACC - 3.0*4.0 */
    wle32(p+pc, enc_mfc1(3, 5)); pc += 4;
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    float msub_result = bits_float((uint32_t)st->gpr[3].ud0);
    CHECK(msub_result > 7.9999f && msub_result < 8.0001f, "MSUB.S: fd = ACC(20.0) - 3.0*4.0 == 8.0");

    /* --- Test 6: MADDA.S. Preset ACC = 1.0, then MADDA.S(2.0, 3.0)
     * => ACC += 2.0*3.0 = 1 + 6 = 7.0. Read back via MADD.S(fd,0,0). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 1.0f);
    load_float(p, &pc, 2, 9, 0.0f);
    load_float(p, &pc, 3, 3, 2.0f);
    load_float(p, &pc, 4, 4, 3.0f);
    wle32(p+pc, enc_cop1_s(F_ADDA, 0, 1, 9)); pc += 4;      /* ACC = 1.0 */
    wle32(p+pc, enc_cop1_s(F_MADDA, 0, 3, 4)); pc += 4;     /* ACC += 2.0*3.0 */
    wle32(p+pc, enc_cop1_s(F_MADD, 5, 9, 9)); pc += 4;      /* f5 = ACC readback */
    wle32(p+pc, enc_mfc1(3, 5)); pc += 4;
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    float madda_result = bits_float((uint32_t)st->gpr[3].ud0);
    CHECK(madda_result > 6.9999f && madda_result < 7.0001f, "MADDA.S: ACC = 1.0 + 2.0*3.0 == 7.0");

    /* --- Test 7: MSUBA.S. Preset ACC = 10.0, then MSUBA.S(2.0, 3.0)
     * => ACC -= 2.0*3.0 = 10 - 6 = 4.0. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 10.0f);
    load_float(p, &pc, 2, 9, 0.0f);
    load_float(p, &pc, 3, 3, 2.0f);
    load_float(p, &pc, 4, 4, 3.0f);
    wle32(p+pc, enc_cop1_s(F_ADDA, 0, 1, 9)); pc += 4;      /* ACC = 10.0 */
    wle32(p+pc, enc_cop1_s(F_MSUBA, 0, 3, 4)); pc += 4;     /* ACC -= 2.0*3.0 */
    wle32(p+pc, enc_cop1_s(F_MADD, 5, 9, 9)); pc += 4;      /* f5 = ACC readback */
    wle32(p+pc, enc_mfc1(3, 5)); pc += 4;
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    float msuba_result = bits_float((uint32_t)st->gpr[3].ud0);
    CHECK(msuba_result > 3.9999f && msuba_result < 4.0001f, "MSUBA.S: ACC = 10.0 - 2.0*3.0 == 4.0");

    /* --- Test 8: the real distinguishing case for the MADD.S-vs-
     * MADDA.S fpuDouble()-double-pass asymmetry. Both are given the
     * SAME inputs: ACC preset to an overflow-clamped -Fmax (via
     * ADDA.S(-FLT_MAX, -FLT_MAX), which overflows to -infinity and is
     * then clamped to -Fmax by ADDA.S's own fpu_check_overflow), and
     * an fs*ft product that overflows to +infinity (1e30 * 1e30).
     *
     * MADD.S clamps the intermediate product to +Fmax via fpuDouble()
     * BEFORE adding to ACC: result = fpuDouble(-Fmax) + fpuDouble(+inf
     * clamped to +Fmax) = -Fmax + Fmax = 0.0 exactly.
     *
     * MADDA.S adds the RAW (unclamped) native +infinity product
     * directly: result = fpuDouble(-Fmax)[finite] + (+infinity)
     * [native, unclamped] = +infinity, which is only clamped to
     * +Fmax AFTERWARD by fpu_check_overflow on the acc register
     * itself.
     *
     * So MADD.S's fd ends up 0.0, while MADDA.S's ACC ends up +Fmax -
     * a different, directly observable result from identical inputs,
     * proving the asymmetry is real and not merely a documentation
     * detail. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, -3.4e38f); /* near -FLT_MAX */
    load_float(p, &pc, 2, 2, -3.4e38f); /* sum overflows to -infinity -> ADDA.S clamps ACC to -Fmax */
    load_float(p, &pc, 3, 3, 1e30f);
    load_float(p, &pc, 4, 4, 1e30f);    /* fs*ft = 1e60 -> native float overflow to +infinity */
    load_float(p, &pc, 6, 9, 0.0f);
    wle32(p+pc, enc_cop1_s(F_ADDA, 0, 1, 2)); pc += 4;      /* ACC = -Fmax (clamped) */
    wle32(p+pc, enc_cop1_s(F_MADD, 5, 3, 4)); pc += 4;      /* f5 = ACC + clamped-product (MADD.S path) */
    wle32(p+pc, enc_mfc1(7, 5)); pc += 4;                   /* r7 = MADD.S result */
    wle32(p+pc, enc_cop1_s(F_MADDA, 0, 3, 4)); pc += 4;     /* ACC += raw-unclamped-product (MADDA.S path) */
    wle32(p+pc, enc_cop1_s(F_MADD, 8, 9, 9)); pc += 4;      /* f8 = ACC readback */
    wle32(p+pc, enc_mfc1(10, 8)); pc += 4;                  /* r10 = MADDA.S's ACC afterward */
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    float madd_asym = bits_float((uint32_t)st->gpr[7].ud0);
    CHECK(madd_asym > -0.0001f && madd_asym < 0.0001f,
          "MADD.S asymmetry case: fd = fpuDouble(-Fmax) + fpuDouble(clamped +inf product) == 0.0 exactly");
    CHECK((uint32_t)st->gpr[10].ud0 == FPU_POS_FMAX,
          "MADDA.S asymmetry case: SAME inputs give ACC == +Fmax (raw product not pre-clamped, "
          "sum overflows to +inf, THEN clamped) - proves the double-fpuDouble()-pass difference is real");

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
