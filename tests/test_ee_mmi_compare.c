/*
 * test_ee_mmi_compare.c - host-native test for ee_core.c's MMI
 * compare/max/min/abs opcode family: PCGTW/PCGTH/PCGTB, PMAXW/PMAXH,
 * PCEQW/PCEQH/PCEQB, PMINW/PMINH, PABSW/PABSH, PADSBH. All ported from
 * PCSX2's MMI.cpp - see the case comments in ee_core.c for the exact
 * reference. Operands are planted directly into EE RAM (st->ram) and
 * loaded into GPRs via LQ (128-bit load), since these are all
 * whole-register SIMD lane operations - matches the pattern
 * established in tests/test_ee_lqsq.c.
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
static void wle64(uint8_t *p, uint64_t v) { for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

/* funct values for the MMI0/MMI1 sub-groups (dispatched on the low 6
 * "funct" bits after primary opcode 0x1C, then further dispatched on
 * "sa" - see ee_core.c's case 0x1C switch). */
#define FUNCT_MMI0 0x08
#define FUNCT_MMI1 0x28

/* MMI0 sub-opcodes (sa field) */
#define SA_PCGTW 0x02
#define SA_PMAXW 0x03
#define SA_PCGTH 0x06
#define SA_PMAXH 0x07
#define SA_PCGTB 0x0A
/* MMI1 sub-opcodes (sa field) */
#define SA_PABSW  0x01
#define SA_PCEQW  0x02
#define SA_PMINW  0x03
#define SA_PADSBH 0x04
#define SA_PABSH  0x05
#define SA_PCEQH  0x06
#define SA_PMINH  0x07
#define SA_PCEQB  0x0A

/* Runs a program that: sets r4 = 0x1000 (RAM base), LQ's rs-value from
 * 0x1000 into gpr[1], LQ's rt-value from 0x1010 into gpr[2], performs
 * the given MMI op (rs=1, rt=2, rd=3), then BREAKs. Caller must plant
 * rs_bytes/rt_bytes into st->ram at 0x1000/0x1010 AFTER ee_core_init()
 * (which zeroes RAM), then call ee_core_run(). */
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

    /* --- PCGTW: 4 signed 32-bit lanes, mask result (all-1s/all-0s). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PCGTW, FUNCT_MMI0);
    if (ee_core_init(&bios) != 0) { printf("init failed\n"); return 1; }
    st = ee_core_get_state();
    wle32(st->ram + 0x1000, (uint32_t)5);    wle32(st->ram + 0x1004, (uint32_t)-3);
    wle32(st->ram + 0x1008, (uint32_t)100);  wle32(st->ram + 0x100C, (uint32_t)-100);
    wle32(st->ram + 0x1010, (uint32_t)3);    wle32(st->ram + 0x1014, (uint32_t)-3);
    wle32(st->ram + 0x1018, (uint32_t)-50);  wle32(st->ram + 0x101C, (uint32_t)200);
    run_until_break(&bios);
    CHECK(st->halted == 1, "PCGTW test: core halted on BREAK");
    CHECK(lane_w(st->gpr[3], 0) == 0xFFFFFFFFu, "PCGTW lane0: 5 > 3 -> all-1s mask");
    CHECK(lane_w(st->gpr[3], 1) == 0x00000000u, "PCGTW lane1: -3 > -3 is false -> all-0s mask");
    CHECK(lane_w(st->gpr[3], 2) == 0xFFFFFFFFu, "PCGTW lane2: 100 > -50 -> all-1s mask");
    CHECK(lane_w(st->gpr[3], 3) == 0x00000000u, "PCGTW lane3: -100 > 200 is false -> all-0s mask");

    /* --- PMAXW: signed 32-bit max, same operands as above. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PMAXW, FUNCT_MMI0);
    ee_core_init(&bios);
    st = ee_core_get_state();
    wle32(st->ram + 0x1000, (uint32_t)5);    wle32(st->ram + 0x1004, (uint32_t)-3);
    wle32(st->ram + 0x1008, (uint32_t)100);  wle32(st->ram + 0x100C, (uint32_t)-100);
    wle32(st->ram + 0x1010, (uint32_t)3);    wle32(st->ram + 0x1014, (uint32_t)-3);
    wle32(st->ram + 0x1018, (uint32_t)-50);  wle32(st->ram + 0x101C, (uint32_t)200);
    run_until_break(&bios);
    CHECK((int32_t)lane_w(st->gpr[3], 0) == 5,    "PMAXW lane0: max(5,3) == 5");
    CHECK((int32_t)lane_w(st->gpr[3], 1) == -3,   "PMAXW lane1: max(-3,-3) == -3");
    CHECK((int32_t)lane_w(st->gpr[3], 2) == 100,  "PMAXW lane2: max(100,-50) == 100");
    CHECK((int32_t)lane_w(st->gpr[3], 3) == 200,  "PMAXW lane3: max(-100,200) == 200 (signed, not bit-pattern)");

    /* --- PCGTH: signed 16-bit lanes, mask result. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PCGTH, FUNCT_MMI0);
    ee_core_init(&bios);
    st = ee_core_get_state();
    /* lane0: rs=10,rt=1 (10>1 true); lane1: rs=-5,rt=-5 (false) */
    wle64(st->ram + 0x1000, ((uint64_t)(uint16_t)(-5) << 16) | (uint64_t)10);
    wle64(st->ram + 0x1010, ((uint64_t)(uint16_t)(-5) << 16) | (uint64_t)1);
    run_until_break(&bios);
    CHECK(lane_h(st->gpr[3], 0) == 0xFFFFu, "PCGTH lane0: 10 > 1 -> all-1s mask");
    CHECK(lane_h(st->gpr[3], 1) == 0x0000u, "PCGTH lane1: -5 > -5 is false -> all-0s mask");

    /* --- PMAXH: signed 16-bit max, same operands. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PMAXH, FUNCT_MMI0);
    ee_core_init(&bios);
    st = ee_core_get_state();
    wle64(st->ram + 0x1000, ((uint64_t)(uint16_t)(-5) << 16) | (uint64_t)10);
    wle64(st->ram + 0x1010, ((uint64_t)(uint16_t)(-5) << 16) | (uint64_t)1);
    run_until_break(&bios);
    CHECK((int16_t)lane_h(st->gpr[3], 0) == 10, "PMAXH lane0: max(10,1) == 10");
    CHECK((int16_t)lane_h(st->gpr[3], 1) == -5, "PMAXH lane1: max(-5,-5) == -5");

    /* --- PCGTB: signed 8-bit lanes, mask result. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PCGTB, FUNCT_MMI0);
    ee_core_init(&bios);
    st = ee_core_get_state();
    st->ram[0x1000] = (uint8_t)20;  st->ram[0x1001] = (uint8_t)(-10);
    st->ram[0x1010] = (uint8_t)5;   st->ram[0x1011] = (uint8_t)(-10);
    run_until_break(&bios);
    CHECK(lane_b(st->gpr[3], 0) == 0xFFu, "PCGTB lane0: 20 > 5 -> all-1s mask");
    CHECK(lane_b(st->gpr[3], 1) == 0x00u, "PCGTB lane1: -10 > -10 is false -> all-0s mask");

    /* --- PABSW: 32-bit absolute value, including the INT32_MIN
     * clamp-to-INT32_MAX quirk (0x80000000 has no positive 32-bit
     * representation - real hardware clamps instead of overflowing). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PABSW, FUNCT_MMI1);
    ee_core_init(&bios);
    st = ee_core_get_state();
    wle32(st->ram + 0x1010, (uint32_t)(-7));         /* ft lane0 = -7 (rt is the source for PABSW) */
    wle32(st->ram + 0x1014, 0x80000000u);            /* ft lane1 = INT32_MIN */
    wle32(st->ram + 0x1018, (uint32_t)12);           /* ft lane2 = 12 (already positive) */
    run_until_break(&bios);
    CHECK((int32_t)lane_w(st->gpr[3], 0) == 7,          "PABSW lane0: abs(-7) == 7");
    CHECK(lane_w(st->gpr[3], 1) == 0x7FFFFFFFu,         "PABSW lane1: abs(INT32_MIN) clamps to INT32_MAX (real hardware quirk)");
    CHECK((int32_t)lane_w(st->gpr[3], 2) == 12,         "PABSW lane2: abs(12) == 12 (already positive, unchanged)");

    /* --- PCEQW: 32-bit equality compare, mask result. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PCEQW, FUNCT_MMI1);
    ee_core_init(&bios);
    st = ee_core_get_state();
    wle32(st->ram + 0x1000, (uint32_t)42); wle32(st->ram + 0x1004, (uint32_t)7);
    wle32(st->ram + 0x1010, (uint32_t)42); wle32(st->ram + 0x1014, (uint32_t)8);
    run_until_break(&bios);
    CHECK(lane_w(st->gpr[3], 0) == 0xFFFFFFFFu, "PCEQW lane0: 42 == 42 -> all-1s mask");
    CHECK(lane_w(st->gpr[3], 1) == 0x00000000u, "PCEQW lane1: 7 != 8 -> all-0s mask");

    /* --- PMINW: signed 32-bit min. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PMINW, FUNCT_MMI1);
    ee_core_init(&bios);
    st = ee_core_get_state();
    wle32(st->ram + 0x1000, (uint32_t)5);    wle32(st->ram + 0x1004, (uint32_t)-100);
    wle32(st->ram + 0x1010, (uint32_t)-3);   wle32(st->ram + 0x1014, (uint32_t)-2);
    run_until_break(&bios);
    CHECK((int32_t)lane_w(st->gpr[3], 0) == -3,   "PMINW lane0: min(5,-3) == -3");
    CHECK((int32_t)lane_w(st->gpr[3], 1) == -100, "PMINW lane1: min(-100,-2) == -100");

    /* --- PADSBH: asymmetric op - low 4 halfword lanes get PSUBH
     * (rs-rt), high 4 lanes get PADDH (rs+rt). Deliberately different
     * from a uniform 8-lane op. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PADSBH, FUNCT_MMI1);
    ee_core_init(&bios);
    st = ee_core_get_state();
    /* rs lanes 0..7 = 10,20,30,40,50,60,70,80 ; rt lanes 0..7 = 1,2,3,4,5,6,7,8 */
    for (int n = 0; n < 8; n++) {
        uint16_t rsv = (uint16_t)(10 * (n + 1));
        uint16_t rtv = (uint16_t)(n + 1);
        int off_rs = 0x1000 + n * 2, off_rt = 0x1010 + n * 2;
        st->ram[off_rs] = rsv & 0xFF; st->ram[off_rs+1] = (rsv >> 8) & 0xFF;
        st->ram[off_rt] = rtv & 0xFF; st->ram[off_rt+1] = (rtv >> 8) & 0xFF;
    }
    run_until_break(&bios);
    CHECK((int16_t)lane_h(st->gpr[3], 0) == (10 - 1),  "PADSBH lane0 (low half): PSUBH -> 10-1=9");
    CHECK((int16_t)lane_h(st->gpr[3], 3) == (40 - 4),  "PADSBH lane3 (low half): PSUBH -> 40-4=36");
    CHECK((int16_t)lane_h(st->gpr[3], 4) == (50 + 5),  "PADSBH lane4 (high half): PADDH -> 50+5=55");
    CHECK((int16_t)lane_h(st->gpr[3], 7) == (80 + 8),  "PADSBH lane7 (high half): PADDH -> 80+8=88");

    /* --- PABSH: 16-bit absolute value, INT16_MIN clamp quirk. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PABSH, FUNCT_MMI1);
    ee_core_init(&bios);
    st = ee_core_get_state();
    {
        uint16_t v0 = (uint16_t)(-9), v1 = 0x8000u, v2 = (uint16_t)33;
        st->ram[0x1010] = v0 & 0xFF; st->ram[0x1011] = (v0>>8)&0xFF;
        st->ram[0x1012] = v1 & 0xFF; st->ram[0x1013] = (v1>>8)&0xFF;
        st->ram[0x1014] = v2 & 0xFF; st->ram[0x1015] = (v2>>8)&0xFF;
    }
    run_until_break(&bios);
    CHECK((int16_t)lane_h(st->gpr[3], 0) == 9,     "PABSH lane0: abs(-9) == 9");
    CHECK(lane_h(st->gpr[3], 1) == 0x7FFFu,        "PABSH lane1: abs(INT16_MIN) clamps to INT16_MAX (real hardware quirk)");
    CHECK((int16_t)lane_h(st->gpr[3], 2) == 33,    "PABSH lane2: abs(33) == 33 (already positive)");

    /* --- PCEQH: 16-bit equality compare. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PCEQH, FUNCT_MMI1);
    ee_core_init(&bios);
    st = ee_core_get_state();
    wle64(st->ram + 0x1000, ((uint64_t)9 << 16) | (uint64_t)15);
    wle64(st->ram + 0x1010, ((uint64_t)10 << 16) | (uint64_t)15);
    run_until_break(&bios);
    CHECK(lane_h(st->gpr[3], 0) == 0xFFFFu, "PCEQH lane0: 15 == 15 -> all-1s mask");
    CHECK(lane_h(st->gpr[3], 1) == 0x0000u, "PCEQH lane1: 9 != 10 -> all-0s mask");

    /* --- PMINH: signed 16-bit min. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PMINH, FUNCT_MMI1);
    ee_core_init(&bios);
    st = ee_core_get_state();
    wle64(st->ram + 0x1000, ((uint64_t)(uint16_t)(-20) << 16) | (uint64_t)7);
    wle64(st->ram + 0x1010, ((uint64_t)(uint16_t)(-5)  << 16) | (uint64_t)3);
    run_until_break(&bios);
    CHECK((int16_t)lane_h(st->gpr[3], 0) == 3,   "PMINH lane0: min(7,3) == 3");
    CHECK((int16_t)lane_h(st->gpr[3], 1) == -20, "PMINH lane1: min(-20,-5) == -20");

    /* --- PCEQB: 8-bit equality compare. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PCEQB, FUNCT_MMI1);
    ee_core_init(&bios);
    st = ee_core_get_state();
    st->ram[0x1000] = 0x7F; st->ram[0x1001] = 0x11;
    st->ram[0x1010] = 0x7F; st->ram[0x1011] = 0x22;
    run_until_break(&bios);
    CHECK(lane_b(st->gpr[3], 0) == 0xFFu, "PCEQB lane0: 0x7F == 0x7F -> all-1s mask");
    CHECK(lane_b(st->gpr[3], 1) == 0x00u, "PCEQB lane1: 0x11 != 0x22 -> all-0s mask");

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
