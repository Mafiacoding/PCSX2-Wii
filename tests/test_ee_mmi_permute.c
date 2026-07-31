/*
 * test_ee_mmi_permute.c - host-native test for ee_core.c's MMI2/MMI3
 * permute/interleave opcode family: PINTH, PINTEH, PEXEH, PEXCH,
 * PEXEW, PEXCW, PREVH, PCPYH, PROT3W. All ported from PCSX2's
 * MMI.cpp. Operands are planted directly into EE RAM and loaded via
 * LQ, same approach as tests/test_ee_mmi_compare.c and
 * tests/test_ee_mmi_sat.c. Each 128-bit operand uses distinct,
 * position-identifiable lane values (lane N = 0x10+N for halfwords,
 * 0x100+N for words) so a wrong permutation shows up immediately as
 * the wrong lane value landing in the wrong place.
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
static void wle16(uint8_t *p, uint16_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

#define FUNCT_MMI2 0x09
#define FUNCT_MMI3 0x29
#define SA_PINTH  0x0A
#define SA_PEXEH  0x1A
#define SA_PREVH  0x1B
#define SA_PEXEW  0x1E
#define SA_PROT3W 0x1F
#define SA_PINTEH 0x0A
#define SA_PEXCH  0x1A
#define SA_PCPYH  0x1B
#define SA_PEXCW  0x1E

static void build_mmi_prog(uint8_t *prog, int sa, int funct) {
    int pc = 0;
    wle32(prog + pc, enc_lui(4, 0x8000)); pc += 4;
    wle32(prog + pc, enc_ori(4, 4, 0x1000)); pc += 4;
    wle32(prog + pc, enc_lq(1, 4, 0x00)); pc += 4;   /* gpr1 = rs operand */
    wle32(prog + pc, enc_lq(2, 4, 0x10)); pc += 4;   /* gpr2 = rt operand */
    wle32(prog + pc, enc_mmi(1, 2, 3, sa, funct)); pc += 4; /* gpr3 = op(gpr1, gpr2) */
    wle32(prog + pc, enc_break()); pc += 4;
}

/* Plants distinct, position-identifiable halfword values: rs lanes =
 * 0x50..0x57, rt lanes = 0x60..0x67. */
static void plant_h_operands(ee_state_t *st) {
    for (int n = 0; n < 8; n++) {
        wle16(st->ram + 0x1000 + n * 2, (uint16_t)(0x50 + n));
        wle16(st->ram + 0x1010 + n * 2, (uint16_t)(0x60 + n));
    }
}
/* Plants distinct word values: rs lanes = 0x500..0x503, rt lanes = 0x600..0x603. */
static void plant_w_operands(ee_state_t *st) {
    for (int n = 0; n < 4; n++) {
        wle32(st->ram + 0x1000 + n * 4, (uint32_t)(0x500 + n));
        wle32(st->ram + 0x1010 + n * 4, (uint32_t)(0x600 + n));
    }
}

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;
    uint8_t *prog = bios.data;
    ee_state_t *st;

    /* --- PINTH: US[0]=Rt0,US[1]=Rs4,US[2]=Rt1,US[3]=Rs5,
     *            US[4]=Rt2,US[5]=Rs6,US[6]=Rt3,US[7]=Rs7 */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PINTH, FUNCT_MMI2);
    if (ee_core_init(&bios) != 0) { printf("init failed\n"); return 1; }
    st = ee_core_get_state();
    plant_h_operands(st);
    run_until_break(&bios);
    CHECK(st->halted == 1, "PINTH test: core halted on BREAK");
    CHECK(lane_h(st->gpr[3], 0) == 0x60, "PINTH US[0] == Rt.US[0]");
    CHECK(lane_h(st->gpr[3], 1) == 0x54, "PINTH US[1] == Rs.US[4] (upper half of Rs, not lower)");
    CHECK(lane_h(st->gpr[3], 2) == 0x61, "PINTH US[2] == Rt.US[1]");
    CHECK(lane_h(st->gpr[3], 3) == 0x55, "PINTH US[3] == Rs.US[5]");
    CHECK(lane_h(st->gpr[3], 6) == 0x63, "PINTH US[6] == Rt.US[3]");
    CHECK(lane_h(st->gpr[3], 7) == 0x57, "PINTH US[7] == Rs.US[7]");

    /* --- PINTEH: US[0]=Rt0,US[1]=Rs0,US[2]=Rt2,US[3]=Rs2,
     *             US[4]=Rt4,US[5]=Rs4,US[6]=Rt6,US[7]=Rs6 */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PINTEH, FUNCT_MMI3);
    ee_core_init(&bios);
    st = ee_core_get_state();
    plant_h_operands(st);
    run_until_break(&bios);
    CHECK(lane_h(st->gpr[3], 0) == 0x60, "PINTEH US[0] == Rt.US[0]");
    CHECK(lane_h(st->gpr[3], 1) == 0x50, "PINTEH US[1] == Rs.US[0] (even lane, not lane 4 like PINTH)");
    CHECK(lane_h(st->gpr[3], 2) == 0x62, "PINTEH US[2] == Rt.US[2]");
    CHECK(lane_h(st->gpr[3], 3) == 0x52, "PINTEH US[3] == Rs.US[2]");
    CHECK(lane_h(st->gpr[3], 6) == 0x66, "PINTEH US[6] == Rt.US[6]");
    CHECK(lane_h(st->gpr[3], 7) == 0x56, "PINTEH US[7] == Rs.US[6]");

    /* --- PEXEH: swaps halfword lanes 0/2 within each 64-bit half (Rt only). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PEXEH, FUNCT_MMI2);
    ee_core_init(&bios);
    st = ee_core_get_state();
    plant_h_operands(st);
    run_until_break(&bios);
    CHECK(lane_h(st->gpr[3], 0) == 0x62, "PEXEH US[0] == Rt.US[2] (swapped with lane 2)");
    CHECK(lane_h(st->gpr[3], 1) == 0x61, "PEXEH US[1] == Rt.US[1] (unchanged)");
    CHECK(lane_h(st->gpr[3], 2) == 0x60, "PEXEH US[2] == Rt.US[0] (swapped with lane 0)");
    CHECK(lane_h(st->gpr[3], 3) == 0x63, "PEXEH US[3] == Rt.US[3] (unchanged)");

    /* --- PEXCH: swaps halfword lanes 1/2 within each 64-bit half (Rt only) - different pair than PEXEH. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PEXCH, FUNCT_MMI3);
    ee_core_init(&bios);
    st = ee_core_get_state();
    plant_h_operands(st);
    run_until_break(&bios);
    CHECK(lane_h(st->gpr[3], 0) == 0x60, "PEXCH US[0] == Rt.US[0] (unchanged, unlike PEXEH)");
    CHECK(lane_h(st->gpr[3], 1) == 0x62, "PEXCH US[1] == Rt.US[2] (swapped with lane 2)");
    CHECK(lane_h(st->gpr[3], 2) == 0x61, "PEXCH US[2] == Rt.US[1] (swapped with lane 1)");
    CHECK(lane_h(st->gpr[3], 3) == 0x63, "PEXCH US[3] == Rt.US[3] (unchanged)");

    /* --- PREVH: fully reverses halfword lanes within each 64-bit half (Rt only). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PREVH, FUNCT_MMI2);
    ee_core_init(&bios);
    st = ee_core_get_state();
    plant_h_operands(st);
    run_until_break(&bios);
    CHECK(lane_h(st->gpr[3], 0) == 0x63, "PREVH US[0] == Rt.US[3]");
    CHECK(lane_h(st->gpr[3], 1) == 0x62, "PREVH US[1] == Rt.US[2]");
    CHECK(lane_h(st->gpr[3], 2) == 0x61, "PREVH US[2] == Rt.US[1]");
    CHECK(lane_h(st->gpr[3], 3) == 0x60, "PREVH US[3] == Rt.US[0]");
    CHECK(lane_h(st->gpr[3], 4) == 0x67, "PREVH US[4] == Rt.US[7]");
    CHECK(lane_h(st->gpr[3], 7) == 0x64, "PREVH US[7] == Rt.US[4]");

    /* --- PCPYH: broadcasts lane 0 across the low half, lane 4 across the high half (Rt only). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PCPYH, FUNCT_MMI3);
    ee_core_init(&bios);
    st = ee_core_get_state();
    plant_h_operands(st);
    run_until_break(&bios);
    CHECK(lane_h(st->gpr[3], 0) == 0x60 && lane_h(st->gpr[3], 1) == 0x60 &&
          lane_h(st->gpr[3], 2) == 0x60 && lane_h(st->gpr[3], 3) == 0x60,
          "PCPYH: low 4 lanes all broadcast Rt.US[0]");
    CHECK(lane_h(st->gpr[3], 4) == 0x64 && lane_h(st->gpr[3], 5) == 0x64 &&
          lane_h(st->gpr[3], 6) == 0x64 && lane_h(st->gpr[3], 7) == 0x64,
          "PCPYH: high 4 lanes all broadcast Rt.US[4]");

    /* --- PEXEW: swaps word lanes 0/2 (Rt only). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PEXEW, FUNCT_MMI2);
    ee_core_init(&bios);
    st = ee_core_get_state();
    plant_w_operands(st);
    run_until_break(&bios);
    CHECK(lane_w(st->gpr[3], 0) == 0x602, "PEXEW UL[0] == Rt.UL[2] (swapped with lane 0)");
    CHECK(lane_w(st->gpr[3], 1) == 0x601, "PEXEW UL[1] == Rt.UL[1] (unchanged)");
    CHECK(lane_w(st->gpr[3], 2) == 0x600, "PEXEW UL[2] == Rt.UL[0] (swapped with lane 2)");
    CHECK(lane_w(st->gpr[3], 3) == 0x603, "PEXEW UL[3] == Rt.UL[3] (unchanged)");

    /* --- PEXCW: swaps word lanes 1/2 (Rt only) - different pair than PEXEW. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PEXCW, FUNCT_MMI3);
    ee_core_init(&bios);
    st = ee_core_get_state();
    plant_w_operands(st);
    run_until_break(&bios);
    CHECK(lane_w(st->gpr[3], 0) == 0x600, "PEXCW UL[0] == Rt.UL[0] (unchanged, unlike PEXEW)");
    CHECK(lane_w(st->gpr[3], 1) == 0x602, "PEXCW UL[1] == Rt.UL[2] (swapped with lane 2)");
    CHECK(lane_w(st->gpr[3], 2) == 0x601, "PEXCW UL[2] == Rt.UL[1] (swapped with lane 1)");
    CHECK(lane_w(st->gpr[3], 3) == 0x603, "PEXCW UL[3] == Rt.UL[3] (unchanged)");

    /* --- PROT3W: rotates word lanes 0,1,2 left by one; lane 3 untouched (Rt only). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PROT3W, FUNCT_MMI2);
    ee_core_init(&bios);
    st = ee_core_get_state();
    plant_w_operands(st);
    run_until_break(&bios);
    CHECK(lane_w(st->gpr[3], 0) == 0x601, "PROT3W UL[0] == Rt.UL[1]");
    CHECK(lane_w(st->gpr[3], 1) == 0x602, "PROT3W UL[1] == Rt.UL[2]");
    CHECK(lane_w(st->gpr[3], 2) == 0x600, "PROT3W UL[2] == Rt.UL[0]");
    CHECK(lane_w(st->gpr[3], 3) == 0x603, "PROT3W UL[3] == Rt.UL[3] (untouched by the rotation)");

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
