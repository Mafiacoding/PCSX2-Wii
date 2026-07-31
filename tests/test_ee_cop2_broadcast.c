/* test_ee_cop2_broadcast.c - host-native test for Round 29 continued's
 * 18th change: the FT-lane-broadcast forms of the already-implemented
 * full-vector arithmetic row - VADDx/y/z/w (funct 0x00-0x03),
 * VSUBx/y/z/w (0x04-0x07), VMAXx/y/z/w (0x10-0x13), VMINIx/y/z/w
 * (0x14-0x17), VMULx/y/z/w (0x18-0x1B). Confirmed against a real
 * PCSX2 upstream reference clone's R5900OpcodeTables.cpp (SPECIAL1
 * table's first 4 rows) and VUops.cpp's applyBinaryMACOpBroadcast:
 * FD[lane] = FS[lane] OP FT.<bc-lane> for every lane selected by
 * destmask, where <bc-lane> is fixed by the specific opcode (not by
 * destmask) - e.g. VADDy always broadcasts FT's Y component,
 * regardless of which lanes of FD get written.
 *
 * VMADDx/y/z/w, VMSUBx/y/z/w (ACC-based broadcast forms) and
 * VMULq/VMAXi/VMULi/VMINIi (Q/I-register broadcast forms) are
 * explicitly out of scope - not covered here, a separate follow-up.
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
/* base op x bc-lane -> funct, per the confirmed row layout:
 * base 0=VADD(0x00+lane), 1=VSUB(0x04+lane), 4=VMAX(0x10+lane),
 * 5=VMINI(0x14+lane), 6=VMUL(0x18+lane). */
static uint32_t enc_bc(uint32_t destmask, int fd, int fs, int ft, uint32_t base, uint32_t bc_lane)
{
    return enc_co(destmask, ft, fs, fd, (base << 2) | bc_lane);
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

    /* r4 = 0x80001000 (RAM base pointer, Round 363: KSEG0 direct-mapped, not raw KUSEG) */
    wle32(prog + (i++)*4, enc_lui(4, 0x8000));
    wle32(prog + (i++)*4, enc_ori(4, 4, 0x1000));

    /* LQ r5 <- RAM[0x1000] (VF1: 2,3,4,5), LQ r6 <- RAM[0x1010] (VF2: 10,20,30,40) */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (6 << 16) | 0x0010);
    wle32(prog + (i++)*4, enc_qmtc2(5, 1));
    wle32(prog + (i++)*4, enc_qmtc2(6, 2));

    /* VADDy.xyzw VF3,VF1,VF2 -> FD[lane] = VF1[lane] + VF2.y(=20) */
    wle32(prog + (i++)*4, enc_bc(0xF, 3, 1, 2, 0, 1));
    wle32(prog + (i++)*4, enc_qmfc2(7, 3));

    /* VSUBx.xyzw VF4,VF1,VF2 -> FD[lane] = VF1[lane] - VF2.x(=10) */
    wle32(prog + (i++)*4, enc_bc(0xF, 4, 1, 2, 1, 0));
    wle32(prog + (i++)*4, enc_qmfc2(8, 4));

    /* VMULz.xyzw VF9,VF1,VF2 -> FD[lane] = VF1[lane] * VF2.z(=30) */
    wle32(prog + (i++)*4, enc_bc(0xF, 9, 1, 2, 6, 2));
    wle32(prog + (i++)*4, enc_qmfc2(10, 9));

    /* VMAXw.xyzw VF11,VF1,VF2 -> FD[lane] = max(VF1[lane], VF2.w(=40)) */
    wle32(prog + (i++)*4, enc_bc(0xF, 11, 1, 2, 4, 3));
    wle32(prog + (i++)*4, enc_qmfc2(12, 11));

    /* VMINIx.xyzw VF13,VF1,VF2 -> FD[lane] = min(VF1[lane], VF2.x(=10)) */
    wle32(prog + (i++)*4, enc_bc(0xF, 13, 1, 2, 5, 0));
    wle32(prog + (i++)*4, enc_qmfc2(14, 13));

    /* VADDy.x VF15,VF1,VF2 (single-lane destmask; VF15 starts at 0) */
    wle32(prog + (i++)*4, enc_bc(0x8, 15, 1, 2, 0, 1));
    wle32(prog + (i++)*4, enc_qmfc2(16, 15));

    wle32(prog + (i++)*4, enc_break());

    if (ee_core_init(&bios) != 0) { printf("ee_core_init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    /* VF1 = (2, 3, 4, 5) */
    wle32(st->ram + 0x1000, f2bits(2.0f));
    wle32(st->ram + 0x1004, f2bits(3.0f));
    wle32(st->ram + 0x1008, f2bits(4.0f));
    wle32(st->ram + 0x100C, f2bits(5.0f));
    /* VF2 = (10, 20, 30, 40) */
    wle32(st->ram + 0x1010, f2bits(10.0f));
    wle32(st->ram + 0x1014, f2bits(20.0f));
    wle32(st->ram + 0x1018, f2bits(30.0f));
    wle32(st->ram + 0x101C, f2bits(40.0f));

    run_until_break(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK (broadcast-form ops recognized)");

    uint32_t vf3x = (uint32_t)(st->gpr[7].ud0 & 0xFFFFFFFFu);
    uint32_t vf3y = (uint32_t)(st->gpr[7].ud0 >> 32);
    uint32_t vf3z = (uint32_t)(st->gpr[7].ud1 & 0xFFFFFFFFu);
    uint32_t vf3w = (uint32_t)(st->gpr[7].ud1 >> 32);
    CHECK(bits2f(vf3x) == 22.0f && bits2f(vf3y) == 23.0f && bits2f(vf3z) == 24.0f && bits2f(vf3w) == 25.0f,
          "VADDy.xyzw VF3,VF1,VF2 broadcasts VF2.y(20) to every lane: (22,23,24,25)");

    uint32_t vf4x = (uint32_t)(st->gpr[8].ud0 & 0xFFFFFFFFu);
    uint32_t vf4y = (uint32_t)(st->gpr[8].ud0 >> 32);
    uint32_t vf4z = (uint32_t)(st->gpr[8].ud1 & 0xFFFFFFFFu);
    uint32_t vf4w = (uint32_t)(st->gpr[8].ud1 >> 32);
    CHECK(bits2f(vf4x) == -8.0f && bits2f(vf4y) == -7.0f && bits2f(vf4z) == -6.0f && bits2f(vf4w) == -5.0f,
          "VSUBx.xyzw VF4,VF1,VF2 broadcasts VF2.x(10) to every lane: (-8,-7,-6,-5)");

    uint32_t vf9x = (uint32_t)(st->gpr[10].ud0 & 0xFFFFFFFFu);
    uint32_t vf9y = (uint32_t)(st->gpr[10].ud0 >> 32);
    uint32_t vf9z = (uint32_t)(st->gpr[10].ud1 & 0xFFFFFFFFu);
    uint32_t vf9w = (uint32_t)(st->gpr[10].ud1 >> 32);
    CHECK(bits2f(vf9x) == 60.0f && bits2f(vf9y) == 90.0f && bits2f(vf9z) == 120.0f && bits2f(vf9w) == 150.0f,
          "VMULz.xyzw VF9,VF1,VF2 broadcasts VF2.z(30) to every lane: (60,90,120,150)");

    uint32_t vf11x = (uint32_t)(st->gpr[12].ud0 & 0xFFFFFFFFu);
    uint32_t vf11y = (uint32_t)(st->gpr[12].ud0 >> 32);
    uint32_t vf11z = (uint32_t)(st->gpr[12].ud1 & 0xFFFFFFFFu);
    uint32_t vf11w = (uint32_t)(st->gpr[12].ud1 >> 32);
    CHECK(bits2f(vf11x) == 40.0f && bits2f(vf11y) == 40.0f && bits2f(vf11z) == 40.0f && bits2f(vf11w) == 40.0f,
          "VMAXw.xyzw VF11,VF1,VF2 broadcasts max(VF1[lane], VF2.w(40)): (40,40,40,40)");

    uint32_t vf13x = (uint32_t)(st->gpr[14].ud0 & 0xFFFFFFFFu);
    uint32_t vf13y = (uint32_t)(st->gpr[14].ud0 >> 32);
    uint32_t vf13z = (uint32_t)(st->gpr[14].ud1 & 0xFFFFFFFFu);
    uint32_t vf13w = (uint32_t)(st->gpr[14].ud1 >> 32);
    CHECK(bits2f(vf13x) == 2.0f && bits2f(vf13y) == 3.0f && bits2f(vf13z) == 4.0f && bits2f(vf13w) == 5.0f,
          "VMINIx.xyzw VF13,VF1,VF2 broadcasts min(VF1[lane], VF2.x(10)): (2,3,4,5), VF1 unchanged since it's always smaller");

    uint32_t vf15x = (uint32_t)(st->gpr[16].ud0 & 0xFFFFFFFFu);
    uint32_t vf15y = (uint32_t)(st->gpr[16].ud0 >> 32);
    CHECK(bits2f(vf15x) == 22.0f && bits2f(vf15y) == 0.0f,
          "VADDy.x VF15,VF1,VF2 only writes the X lane (destmask honored, Y stays 0)");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
