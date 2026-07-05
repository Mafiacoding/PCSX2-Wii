/*
 * test_ee_fpu2.c - host-native test for ee_core.c's newer COP1/FPU
 * additions: SQRT.S, RSQRT.S, MAX.S, MIN.S, and BC1F/BC1T branches.
 * Ported from PCSX2's FPU.cpp (see the case comments in ee_core.c for
 * the exact reference and the one real-hardware quirk worth calling
 * out here: SQRT.S's source operand is Ft, not Fs).
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
static uint32_t enc_ceq_s(int fs, int ft) { return enc_cop1_s(0x32, 0, fs, ft); }
static uint32_t enc_bc1(int cond, int16_t imm) { return (0x11 << 26) | (0x08 << 21) | (cond << 16) | (uint16_t)imm; }
static uint32_t enc_nop(void) { return 0; }
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

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    uint8_t *p = bios.data;
    int pc;

    /* --- Test 1: SQRT.S. Real quirk: source is Ft (rt field), not Fs.
     * f1 = 9.0 loaded as "ft" operand (register field position 2, i.e.
     * the rt slot in the encoding); result goes to f0 (fd). */
    pc = 0;
    load_float(p, &pc, 1, 1, 9.0f);                       /* f1 = 9.0, will be used as Ft */
    wle32(p+pc, enc_cop1_s(0x04, /*fd=*/0, /*fs=*/2, /*ft=*/1)); pc += 4; /* SQRT.S f0, (fs unused)=f2, ft=f1 */
    wle32(p+pc, enc_mfc1(3, 0)); pc += 4;                 /* r3 = f0 */
    wle32(p+pc, enc_break()); pc += 4;

    if (ee_core_init(&bios) != 0) { printf("init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();
    ee_core_run(&bios);
    CHECK(st->halted == 1, "SQRT.S test: core halted on BREAK");
    float sqrt_result = bits_float((uint32_t)st->gpr[3].ud0);
    CHECK(sqrt_result > 2.9999f && sqrt_result < 3.0001f, "SQRT.S: sqrt(9.0) == 3.0 (source read from Ft, not Fs)");

    /* --- Test 2: SQRT.S of a negative Ft: real hardware takes sqrt of
     * the absolute value (does not produce NaN). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, -4.0f);
    wle32(p+pc, enc_cop1_s(0x04, 0, 2, 1)); pc += 4; /* SQRT.S f0, ft=f1(-4.0) */
    wle32(p+pc, enc_mfc1(3, 0)); pc += 4;
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    float sqrt_neg_result = bits_float((uint32_t)st->gpr[3].ud0);
    CHECK(sqrt_neg_result > 1.9999f && sqrt_neg_result < 2.0001f,
          "SQRT.S of a negative Ft (-4.0) takes sqrt(fabs()) = 2.0, not NaN");

    /* --- Test 3: RSQRT.S: f0 = f_fs / sqrt(f_ft) = 16.0 / sqrt(4.0) = 8.0 */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 16.0f); /* fs */
    load_float(p, &pc, 2, 2, 4.0f);  /* ft */
    wle32(p+pc, enc_cop1_s(0x16, 0, 1, 2)); pc += 4; /* RSQRT.S f0, fs=f1(16), ft=f2(4) */
    wle32(p+pc, enc_mfc1(3, 0)); pc += 4;
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    float rsqrt_result = bits_float((uint32_t)st->gpr[3].ud0);
    CHECK(rsqrt_result > 7.9999f && rsqrt_result < 8.0001f, "RSQRT.S: 16.0 / sqrt(4.0) == 8.0");

    /* --- Test 4: RSQRT.S with Ft == 0: real hardware returns +Fmax
     * (denormals-are-zero special case), not infinity/crash. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 16.0f);
    load_float(p, &pc, 2, 2, 0.0f);
    wle32(p+pc, enc_cop1_s(0x16, 0, 1, 2)); pc += 4;
    wle32(p+pc, enc_mfc1(3, 0)); pc += 4;
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    CHECK((uint32_t)st->gpr[3].ud0 == FPU_POS_FMAX,
          "RSQRT.S with Ft==0.0 returns +Fmax (real hardware special case), not a crash/NaN");

    /* --- Test 5: MAX.S / MIN.S with the bit-level signed-int
     * comparison quirk: both negative operands need the REVERSED
     * comparison to give the correct float max/min. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, -5.0f); /* fs */
    load_float(p, &pc, 2, 2, -2.0f); /* ft */
    wle32(p+pc, enc_cop1_s(0x28, 0, 1, 2)); pc += 4; /* MAX.S f0, -5.0, -2.0 -> should be -2.0 */
    wle32(p+pc, enc_cop1_s(0x29, 4, 1, 2)); pc += 4; /* MIN.S f4, -5.0, -2.0 -> should be -5.0 */
    wle32(p+pc, enc_mfc1(3, 0)); pc += 4;
    wle32(p+pc, enc_mfc1(5, 4)); pc += 4;
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    float max_result = bits_float((uint32_t)st->gpr[3].ud0);
    float min_result = bits_float((uint32_t)st->gpr[5].ud0);
    CHECK(max_result > -2.0001f && max_result < -1.9999f,
          "MAX.S(-5.0, -2.0) == -2.0 (bit-level comparison correctly reversed for both-negative case)");
    CHECK(min_result > -5.0001f && min_result < -4.9999f,
          "MIN.S(-5.0, -2.0) == -5.0");

    /* --- Test 6: MAX.S / MIN.S with mixed signs (the "normal", non-
     * reversed case) for good measure. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 3.0f);
    load_float(p, &pc, 2, 2, -7.0f);
    wle32(p+pc, enc_cop1_s(0x28, 0, 1, 2)); pc += 4; /* MAX.S(3.0, -7.0) -> 3.0 */
    wle32(p+pc, enc_mfc1(3, 0)); pc += 4;
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    float mixed_max = bits_float((uint32_t)st->gpr[3].ud0);
    CHECK(mixed_max > 2.9999f && mixed_max < 3.0001f, "MAX.S(3.0, -7.0) == 3.0 (mixed-sign case)");

    /* --- Test 7: BC1F/BC1T. Set the condition flag via C.EQ.S, then
     * confirm BC1T branches (taken) and BC1F does not, and vice versa
     * when the flag is clear. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 5.0f);
    load_float(p, &pc, 2, 2, 5.0f);
    wle32(p+pc, enc_ceq_s(1, 2)); pc += 4;             /* C.EQ.S: 5.0==5.0 -> flag SET */
    wle32(p+pc, enc_bc1(1, 2)); pc += 4;               /* BC1T: flag set -> SHOULD branch +2*4=+8 past delay slot */
    wle32(p+pc, enc_nop()); pc += 4;                    /* delay slot */
    wle32(p+pc, enc_lui(4, 0x0001)); pc += 4;           /* SKIPPED if BC1T branched correctly */
    wle32(p+pc, enc_lui(4, 0x0002)); pc += 4;           /* branch target: r4 = 0x00020000 */
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    CHECK((uint32_t)st->gpr[4].ud0 == 0x00020000u,
          "BC1T branches when the FP condition flag is SET (landed on the branch target, skipped the other LUI)");

    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 5.0f);
    load_float(p, &pc, 2, 2, 9.0f);
    wle32(p+pc, enc_ceq_s(1, 2)); pc += 4;             /* C.EQ.S: 5.0!=9.0 -> flag CLEAR */
    wle32(p+pc, enc_bc1(0, 2)); pc += 4;               /* BC1F: flag clear -> SHOULD branch */
    wle32(p+pc, enc_nop()); pc += 4;
    wle32(p+pc, enc_lui(4, 0x0001)); pc += 4;           /* SKIPPED if BC1F branched correctly */
    wle32(p+pc, enc_lui(4, 0x0002)); pc += 4;           /* branch target */
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    CHECK((uint32_t)st->gpr[4].ud0 == 0x00020000u,
          "BC1F branches when the FP condition flag is CLEAR");

    /* --- Test 8: BC1T must NOT branch when the flag is clear (proves
     * this isn't an unconditional jump). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 5.0f);
    load_float(p, &pc, 2, 2, 9.0f);
    wle32(p+pc, enc_ceq_s(1, 2)); pc += 4;             /* flag CLEAR */
    wle32(p+pc, enc_bc1(1, 2)); pc += 4;               /* BC1T: flag clear -> should NOT branch */
    wle32(p+pc, enc_nop()); pc += 4;
    wle32(p+pc, enc_lui(4, 0x0003)); pc += 4;           /* fallthrough: r4 = 0x00030000 */
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    ee_core_run(&bios);
    CHECK((uint32_t)st->gpr[4].ud0 == 0x00030000u,
          "BC1T correctly does NOT branch when the FP condition flag is clear");

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
