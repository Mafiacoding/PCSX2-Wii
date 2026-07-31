/* test_ee_cop2_arith3.c - host-native test for Round 29 continued's
 * 16th change: extending the VADD/VMUL/VSUB row (Round 13's VSUB,
 * Round 29 continued's 10th change's VADD/VMUL) with VMAX (funct
 * 0x2B) and VMINI (funct 0x2F) - the same 3-operand full-vector
 * shape, ported from PCSX2's own VUops.cpp _vuMAX/_vuMINI (a plain
 * float max/min comparison per lane, no NaN/signed-zero special
 * handling - consistent with this project's existing float datapath
 * elsewhere).
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
static uint32_t enc_break(void)                       { return 0x0D; }

static uint32_t enc_qmtc2(int rt, int vf) { return (0x12u << 26) | (0x05u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vf << 11); }
static uint32_t enc_qmfc2(int rt, int vf) { return (0x12u << 26) | (0x01u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vf << 11); }

static uint32_t enc_co(uint32_t destmask, int ft, int fs, int fd, uint32_t funct)
{
    return (0x12u << 26) | ((0x10u | destmask) << 21) | ((uint32_t)ft << 16) | ((uint32_t)fs << 11) | ((uint32_t)fd << 6) | funct;
}
static uint32_t enc_vmax(uint32_t destmask, int fd, int fs, int ft)  { return enc_co(destmask, ft, fs, fd, 0x2B); }
static uint32_t enc_vmini(uint32_t destmask, int fd, int fs, int ft) { return enc_co(destmask, ft, fs, fd, 0x2F); }

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

    /* r4 = 0x80001000 (RAM base pointer, Round 363: KSEG0 direct-mapped, not raw KUSEG) */
    wle32(prog + (i++)*4, enc_lui(4, 0x8000));
    wle32(prog + (i++)*4, enc_ori(4, 4, 0x1000));

    /* LQ r5 <- RAM[0x1000] (VF1: 2.0,-3.0,4.0,-5.0), LQ r6 <- RAM[0x1010] (VF2: 1.0,1.0,10.0,-10.0) */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (6 << 16) | 0x0010);
    wle32(prog + (i++)*4, enc_qmtc2(5, 1));
    wle32(prog + (i++)*4, enc_qmtc2(6, 2));

    /* VMAX.xyzw VF3,VF1,VF2 */
    wle32(prog + (i++)*4, enc_vmax(0xF, 3, 1, 2));
    wle32(prog + (i++)*4, enc_qmfc2(7, 3));

    /* VMINI.xyzw VF4,VF1,VF2 */
    wle32(prog + (i++)*4, enc_vmini(0xF, 4, 1, 2));
    wle32(prog + (i++)*4, enc_qmfc2(8, 4));

    /* VMAX.x VF5,VF1,VF2 (X lane only - VF5 starts at 0, so only lane0 changes) */
    wle32(prog + (i++)*4, enc_vmax(0x8, 5, 1, 2));
    wle32(prog + (i++)*4, enc_qmfc2(9, 5));

    wle32(prog + (i++)*4, enc_break());

    if (ee_core_init(&bios) != 0) { printf("ee_core_init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    /* VF1 = (2.0, -3.0, 4.0, -5.0) */
    wle32(st->ram + 0x1000, f2bits(2.0f));
    wle32(st->ram + 0x1004, f2bits(-3.0f));
    wle32(st->ram + 0x1008, f2bits(4.0f));
    wle32(st->ram + 0x100C, f2bits(-5.0f));
    /* VF2 = (1.0, 1.0, 10.0, -10.0) */
    wle32(st->ram + 0x1010, f2bits(1.0f));
    wle32(st->ram + 0x1014, f2bits(1.0f));
    wle32(st->ram + 0x1018, f2bits(10.0f));
    wle32(st->ram + 0x101C, f2bits(-10.0f));

    run_until_break(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK (VMAX/VMINI recognized)");

    /* VMAX(VF1,VF2) = max(2,1)=2, max(-3,1)=1, max(4,10)=10, max(-5,-10)=-5 */
    uint32_t vf3x = (uint32_t)(st->gpr[7].ud0 & 0xFFFFFFFFu);
    uint32_t vf3y = (uint32_t)(st->gpr[7].ud0 >> 32);
    uint32_t vf3z = (uint32_t)(st->gpr[7].ud1 & 0xFFFFFFFFu);
    uint32_t vf3w = (uint32_t)(st->gpr[7].ud1 >> 32);
    CHECK(bits2f(vf3x) == 2.0f && bits2f(vf3y) == 1.0f && bits2f(vf3z) == 10.0f && bits2f(vf3w) == -5.0f,
          "VMAX.xyzw VF3,VF1,VF2 computes (2,1,10,-5)");

    /* VMINI(VF1,VF2) = min(2,1)=1, min(-3,1)=-3, min(4,10)=4, min(-5,-10)=-10 */
    uint32_t vf4x = (uint32_t)(st->gpr[8].ud0 & 0xFFFFFFFFu);
    uint32_t vf4y = (uint32_t)(st->gpr[8].ud0 >> 32);
    uint32_t vf4z = (uint32_t)(st->gpr[8].ud1 & 0xFFFFFFFFu);
    uint32_t vf4w = (uint32_t)(st->gpr[8].ud1 >> 32);
    CHECK(bits2f(vf4x) == 1.0f && bits2f(vf4y) == -3.0f && bits2f(vf4z) == 4.0f && bits2f(vf4w) == -10.0f,
          "VMINI.xyzw VF4,VF1,VF2 computes (1,-3,4,-10)");

    /* VMAX.x only: x=max(2,1)=2, y stays 0 (VF5 started at 0) */
    uint32_t vf5x = (uint32_t)(st->gpr[9].ud0 & 0xFFFFFFFFu);
    uint32_t vf5y = (uint32_t)(st->gpr[9].ud0 >> 32);
    CHECK(bits2f(vf5x) == 2.0f && bits2f(vf5y) == 0.0f,
          "VMAX.x VF5,VF1,VF2 only writes the X lane (destmask honored), Y stays 0");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
