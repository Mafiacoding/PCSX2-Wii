/*
 * test_ee_mmi_sat.c - host-native test for ee_core.c's MMI0 saturated
 * arithmetic (PADDSW/PSUBSW/PADDSH/PSUBSH/PADDSB/PSUBSB) and
 * PEXT5/PPAC5 (GS 5551-pixel-format unpack/pack), all ported from
 * PCSX2's MMI.cpp. Operands are planted directly into EE RAM and
 * loaded via LQ, same approach as tests/test_ee_lqsq.c and
 * tests/test_ee_mmi_compare.c.
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

/* Task #178 test-harness compatibility helper: BREAK now raises a
 * genuine Breakpoint exception (ExcCode 9, see ee_core.c's SPECIAL
 * funct 0x0D case) instead of unconditionally halting the emulated
 * core - real R5900 hardware never stops executing just because it
 * hit a BREAK. This project's existing test suite used a trailing
 * BREAK + st->halted as a convenient "run to completion, then inspect
 * final state" marker; rather than rewriting every such test's
 * assertions, this drop-in replacement for ee_core_run() steps until
 * EITHER the core genuinely halts on its own (a real bug - e.g. an
 * unimplemented opcode) or a Breakpoint exception was just raised
 * (Cause.ExcCode==9 and Status.EXL just got set, i.e. we're now
 * sitting right at the vectored PC), and in the latter case
 * synthesizes the exact same st->halted=1 / halt_reason convention the
 * old unconditional-halt code produced - purely a test-harness
 * bookkeeping shim. It changes nothing about ee_core.c's real,
 * production BREAK behavior (which is what task #178 is actually
 * testing against the real BIOS). */
static void run_until_break(const bios_image_t *bios) {
    (void)bios;
    ee_state_t *st = ee_core_get_state();
    long guard;
    for (guard = 0; guard < 2000000L; guard++) {
        if (ee_core_step()) return; /* genuine halt - not a BREAK, leave as-is */
        if (((st->cop0[13] >> 2) & 0x1Fu) == 9u && (st->cop0[12] & 0x2u) != 0u) {
            st->halted = 1;
            snprintf(st->halt_reason, sizeof(st->halt_reason),
                     "BREAK (task #178: real Breakpoint exception raised, ExcCode 9)");
            return;
        }
    }
    st->halted = 1;
    snprintf(st->halt_reason, sizeof(st->halt_reason),
             "run_until_break() safety cap reached without a Breakpoint exception");
}

static uint32_t enc_lui(int rt, uint16_t imm)        { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_lq(int rt, int rs, int16_t imm)  { return (0x1E << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_mmi(int rs, int rt, int rd, int sa, int funct) {
    return (0x1C << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}
static uint32_t enc_break(void) { return 0x0D; }
static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

#define FUNCT_MMI0 0x08
#define SA_PADDSW 0x10
#define SA_PSUBSW 0x11
#define SA_PADDSH 0x14
#define SA_PSUBSH 0x15
#define SA_PADDSB 0x18
#define SA_PSUBSB 0x19
#define SA_PEXT5  0x1E
#define SA_PPAC5  0x1F

static void build_mmi_prog(uint8_t *prog, int sa, int funct) {
    int pc = 0;
    wle32(prog + pc, enc_lui(4, 0x8000)); pc += 4;
    wle32(prog + pc, enc_ori(4, 4, 0x1000)); pc += 4;
    wle32(prog + pc, enc_lq(1, 4, 0x00)); pc += 4;   /* gpr1 = rs operand */
    wle32(prog + pc, enc_lq(2, 4, 0x10)); pc += 4;   /* gpr2 = rt operand */
    wle32(prog + pc, enc_mmi(1, 2, 3, sa, funct)); pc += 4; /* gpr3 = op(gpr1, gpr2) */
    wle32(prog + pc, enc_break()); pc += 4;
}

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;
    uint8_t *prog = bios.data;
    ee_state_t *st;

    /* --- PADDSW: saturated 32-bit add. lane0: normal (5+3=8). lane1:
     * overflow (INT32_MAX + 100 clamps to INT32_MAX). lane2: underflow
     * (INT32_MIN + -100 clamps to INT32_MIN). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PADDSW, FUNCT_MMI0);
    if (ee_core_init(&bios) != 0) { printf("init failed\n"); return 1; }
    st = ee_core_get_state();
    wle32(st->ram + 0x1000, (uint32_t)5);            wle32(st->ram + 0x1010, (uint32_t)3);
    wle32(st->ram + 0x1004, 0x7FFFFFFFu);            wle32(st->ram + 0x1014, (uint32_t)100);
    wle32(st->ram + 0x1008, 0x80000000u);            wle32(st->ram + 0x1018, (uint32_t)(-100));
    run_until_break(&bios);
    CHECK(st->halted == 1, "PADDSW test: core halted on BREAK");
    CHECK((int32_t)lane_w(st->gpr[3], 0) == 8,          "PADDSW lane0: 5+3=8 (no saturation needed)");
    CHECK(lane_w(st->gpr[3], 1) == 0x7FFFFFFFu,         "PADDSW lane1: INT32_MAX+100 saturates to INT32_MAX");
    CHECK(lane_w(st->gpr[3], 2) == 0x80000000u,         "PADDSW lane2: INT32_MIN+(-100) saturates to INT32_MIN");

    /* --- PSUBSW: saturated 32-bit subtract. lane0: normal (10-3=7).
     * lane1: overflow (INT32_MAX - (-100) saturates to INT32_MAX).
     * lane2: underflow (INT32_MIN - 100 saturates to INT32_MIN). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PSUBSW, FUNCT_MMI0);
    ee_core_init(&bios);
    st = ee_core_get_state();
    wle32(st->ram + 0x1000, (uint32_t)10);           wle32(st->ram + 0x1010, (uint32_t)3);
    wle32(st->ram + 0x1004, 0x7FFFFFFFu);            wle32(st->ram + 0x1014, (uint32_t)(-100));
    wle32(st->ram + 0x1008, 0x80000000u);            wle32(st->ram + 0x1018, (uint32_t)100);
    run_until_break(&bios);
    CHECK((int32_t)lane_w(st->gpr[3], 0) == 7,          "PSUBSW lane0: 10-3=7 (no saturation needed)");
    CHECK(lane_w(st->gpr[3], 1) == 0x7FFFFFFFu,         "PSUBSW lane1: INT32_MAX-(-100) saturates to INT32_MAX");
    CHECK(lane_w(st->gpr[3], 2) == 0x80000000u,         "PSUBSW lane2: INT32_MIN-100 saturates to INT32_MIN");

    /* --- PADDSH: saturated 16-bit add. lane0: normal. lane1: overflow. lane2: underflow. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PADDSH, FUNCT_MMI0);
    ee_core_init(&bios);
    st = ee_core_get_state();
    {
        uint16_t rs0=5, rs1=0x7FFF, rs2=0x8000;
        uint16_t rt0=3, rt1=50,     rt2=(uint16_t)(-50);
        st->ram[0x1000]=rs0&0xFF; st->ram[0x1001]=rs0>>8;
        st->ram[0x1002]=rs1&0xFF; st->ram[0x1003]=rs1>>8;
        st->ram[0x1004]=rs2&0xFF; st->ram[0x1005]=rs2>>8;
        st->ram[0x1010]=rt0&0xFF; st->ram[0x1011]=rt0>>8;
        st->ram[0x1012]=rt1&0xFF; st->ram[0x1013]=rt1>>8;
        st->ram[0x1014]=rt2&0xFF; st->ram[0x1015]=rt2>>8;
    }
    run_until_break(&bios);
    CHECK((int16_t)lane_h(st->gpr[3], 0) == 8,      "PADDSH lane0: 5+3=8 (no saturation needed)");
    CHECK(lane_h(st->gpr[3], 1) == 0x7FFFu,         "PADDSH lane1: INT16_MAX+50 saturates to INT16_MAX");
    CHECK(lane_h(st->gpr[3], 2) == 0x8000u,         "PADDSH lane2: INT16_MIN+(-50) saturates to INT16_MIN");

    /* --- PSUBSH: saturated 16-bit subtract. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PSUBSH, FUNCT_MMI0);
    ee_core_init(&bios);
    st = ee_core_get_state();
    {
        uint16_t rs0=10, rs1=0x7FFF, rs2=0x8000;
        uint16_t rt0=3,  rt1=(uint16_t)(-50), rt2=50;
        st->ram[0x1000]=rs0&0xFF; st->ram[0x1001]=rs0>>8;
        st->ram[0x1002]=rs1&0xFF; st->ram[0x1003]=rs1>>8;
        st->ram[0x1004]=rs2&0xFF; st->ram[0x1005]=rs2>>8;
        st->ram[0x1010]=rt0&0xFF; st->ram[0x1011]=rt0>>8;
        st->ram[0x1012]=rt1&0xFF; st->ram[0x1013]=rt1>>8;
        st->ram[0x1014]=rt2&0xFF; st->ram[0x1015]=rt2>>8;
    }
    run_until_break(&bios);
    CHECK((int16_t)lane_h(st->gpr[3], 0) == 7,      "PSUBSH lane0: 10-3=7 (no saturation needed)");
    CHECK(lane_h(st->gpr[3], 1) == 0x7FFFu,         "PSUBSH lane1: INT16_MAX-(-50) saturates to INT16_MAX");
    CHECK(lane_h(st->gpr[3], 2) == 0x8000u,         "PSUBSH lane2: INT16_MIN-50 saturates to INT16_MIN");

    /* --- PADDSB: saturated 8-bit add. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PADDSB, FUNCT_MMI0);
    ee_core_init(&bios);
    st = ee_core_get_state();
    st->ram[0x1000]=5;    st->ram[0x1010]=3;
    st->ram[0x1001]=0x7F; st->ram[0x1011]=10;
    st->ram[0x1002]=0x80; st->ram[0x1012]=(uint8_t)(-10);
    run_until_break(&bios);
    CHECK((int8_t)lane_b(st->gpr[3], 0) == 8,   "PADDSB lane0: 5+3=8 (no saturation needed)");
    CHECK(lane_b(st->gpr[3], 1) == 0x7Fu,       "PADDSB lane1: INT8_MAX+10 saturates to INT8_MAX (0x7F)");
    CHECK(lane_b(st->gpr[3], 2) == 0x80u,       "PADDSB lane2: INT8_MIN+(-10) saturates to INT8_MIN (0x80)");

    /* --- PSUBSB: saturated 8-bit subtract. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PSUBSB, FUNCT_MMI0);
    ee_core_init(&bios);
    st = ee_core_get_state();
    st->ram[0x1000]=10;   st->ram[0x1010]=3;
    st->ram[0x1001]=0x7F; st->ram[0x1011]=(uint8_t)(-10);
    st->ram[0x1002]=0x80; st->ram[0x1012]=10;
    run_until_break(&bios);
    CHECK((int8_t)lane_b(st->gpr[3], 0) == 7,   "PSUBSB lane0: 10-3=7 (no saturation needed)");
    CHECK(lane_b(st->gpr[3], 1) == 0x7Fu,       "PSUBSB lane1: INT8_MAX-(-10) saturates to INT8_MAX (0x7F)");
    CHECK(lane_b(st->gpr[3], 2) == 0x80u,       "PSUBSB lane2: INT8_MIN-10 saturates to INT8_MIN (0x80)");

    /* --- PEXT5: unpack a 5551 pixel (bottom 16 bits of a 32-bit
     * lane) into R/G/B left-aligned in bytes 0/1/2 and A in bit 31.
     * Uses rt only (rt=gpr2 in build_mmi_prog); rs (gpr1) is left
     * zeroed and must have no effect. Pixel layout is
     * R(bits0-4) | G(bits5-9)<<5 | B(bits10-14)<<10 | A(bit15)<<15.
     * R=0x1F, G=0x03, B=0x00, A=1 -> 0x1F | (0x03<<5) | (1<<15) =
     * 0x807F. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PEXT5, FUNCT_MMI0);
    ee_core_init(&bios);
    st = ee_core_get_state();
    wle32(st->ram + 0x1010, 0x0000807Fu); /* rt lane0 = the 5551 pixel */
    run_until_break(&bios);
    {
        uint32_t r = lane_w(st->gpr[3], 0);
        CHECK(((r >> 3) & 0x1F)  == 0x1F, "PEXT5: R channel (bits 3-7) == 0x1F");
        CHECK(((r >> 11) & 0x1F) == 0x03, "PEXT5: G channel (bits 11-15) == 0x03");
        CHECK(((r >> 19) & 0x1F) == 0x00, "PEXT5: B channel (bits 19-23) == 0x00");
        CHECK(((r >> 31) & 0x01) == 0x01, "PEXT5: A bit (bit 31) == 1");
    }

    /* --- PPAC5: the inverse of PEXT5 - pack the unpacked layout back
     * down to a 16-bit 5551 value. Round-trips the same pixel used
     * above by feeding PEXT5's output back through PPAC5 in a second
     * run, confirming get the original 0x801F back. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PPAC5, FUNCT_MMI0);
    ee_core_init(&bios);
    st = ee_core_get_state();
    {
        /* Manually construct the PEXT5-unpacked form of 0x807F:
         * R=0x1F<<3, G=0x03<<11, B=0<<19, A=1<<31 */
        uint32_t unpacked = (0x1Fu << 3) | (0x03u << 11) | (0u << 19) | (1u << 31);
        wle32(st->ram + 0x1010, unpacked);
    }
    run_until_break(&bios);
    CHECK(lane_w(st->gpr[3], 0) == 0x0000807Fu, "PPAC5: packs the unpacked layout back to the original 0x807F pixel (round-trip with PEXT5)");

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
