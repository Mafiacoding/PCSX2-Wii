/* test_ee_cop2_div.c - host-native test for Round 29 continued's 25th
 * change: VDIV (idx56), VSQRT (idx57), VRSQRT (idx58), VWAITQ
 * (idx59) - the division/sqrt family that produces the Q register
 * value (this project's cop2_ctrl[22]), ported from a real PCSX2
 * upstream reference clone's VUops.cpp _vuDIV/_vuSQRT/_vuRSQRT/
 * _vuWAITQ.
 *
 * Fsf/Ftf are independent 2-bit lane selectors living in destmask's
 * low/high 2 bits respectively (destmask&3 = Fsf, (destmask>>2)&3 =
 * Ftf). VSQRT uses only Ftf (no FS operand). Divide-by-zero produces
 * a signed FLT_MAX bit pattern (no true IEEE infinity on real PS2
 * hardware).
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
static uint32_t enc_cfc2(int rt, int vi)  { return (0x12u << 26) | (0x02u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vi << 11); }

/* SPECIAL2 idx = (instr&0x3) | ((instr>>4)&0x7C); bits 2-5 forced to
 * 0xF for the outer funct dispatch. "lane_field" packs Fsf (low 2
 * bits) and Ftf (high 2 bits) into the same destmask slot. */
static uint32_t enc_special2(uint32_t lane_field, int ft_field, int fs_field, uint32_t idx)
{
    uint32_t f6_10 = (idx >> 2) & 0x1Fu;
    uint32_t f0_1 = idx & 0x3u;
    return (0x12u << 26) | ((0x10u | lane_field) << 21) | ((uint32_t)ft_field << 16)
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

    /* r4 = 0x80001000 (RAM base pointer, Round 363: KSEG0 direct-mapped, not raw KUSEG) */
    wle32(prog + (i++)*4, enc_lui(4, 0x8000));
    wle32(prog + (i++)*4, enc_ori(4, 4, 0x1000));

    /* LQ r5 <- RAM[0x1000] (VF1: 20.0, 0.0, 4.0, -9.0), LQ r6 <- RAM[0x1010] (VF2: 4.0, 5.0, 0.0, 0.0) */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (6 << 16) | 0x0010);
    wle32(prog + (i++)*4, enc_qmtc2(5, 1));
    wle32(prog + (i++)*4, enc_qmtc2(6, 2));

    /* VDIV Q,VF1x,VF2x (Fsf=0=X,Ftf=0=X): Q = 20.0/4.0 = 5.0. lane_field = Fsf|(Ftf<<2) = 0 */
    wle32(prog + (i++)*4, enc_special2(0, 2, 1, 56));
    wle32(prog + (i++)*4, enc_cfc2(7, 22));

    /* VDIV Q,VF1y,VF2z (Fsf=1=Y(0.0),Ftf=2=Z(0.0)): 0/0 divide-by-zero, same sign (both positive) -> +FLT_MAX (0x7F7FFFFF) */
    wle32(prog + (i++)*4, enc_special2(1 | (2 << 2), 2, 1, 56));
    wle32(prog + (i++)*4, enc_cfc2(8, 22));

    /* VSQRT Q,VF1w (Ftf=3=W(-9.0)): Q = sqrt(|-9.0|) = 3.0. lane_field = Ftf<<2 = 3<<2=12 */
    wle32(prog + (i++)*4, enc_special2(3 << 2, 1, 0, 57));
    wle32(prog + (i++)*4, enc_cfc2(9, 22));

    /* VRSQRT Q,VF1x,VF2x (Fsf=0=X(20.0),Ftf=0=X(4.0)): Q = 20.0/sqrt(4.0) = 20.0/2.0 = 10.0 */
    wle32(prog + (i++)*4, enc_special2(0, 2, 1, 58));
    wle32(prog + (i++)*4, enc_cfc2(10, 22));

    /* VRSQRT Q,VF1x,VF2z (Fsf=0=X(20.0),Ftf=2=Z(0.0)): ft==0, fs!=0 -> +FLT_MAX (both positive) */
    wle32(prog + (i++)*4, enc_special2(0 | (2 << 2), 2, 1, 58));
    wle32(prog + (i++)*4, enc_cfc2(11, 22));

    /* VWAITQ - true no-op, Q must be unchanged from the previous VRSQRT result */
    wle32(prog + (i++)*4, enc_special2(0, 0, 0, 59));
    wle32(prog + (i++)*4, enc_cfc2(12, 22));

    wle32(prog + (i++)*4, enc_break());

    if (ee_core_init(&bios) != 0) { printf("ee_core_init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    /* VF1 = (20.0, 0.0, 4.0, -9.0) */
    wle32(st->ram + 0x1000, f2bits(20.0f));
    wle32(st->ram + 0x1004, f2bits(0.0f));
    wle32(st->ram + 0x1008, f2bits(4.0f));
    wle32(st->ram + 0x100C, f2bits(-9.0f));
    /* VF2 = (4.0, 5.0, 0.0, 0.0) */
    wle32(st->ram + 0x1010, f2bits(4.0f));
    wle32(st->ram + 0x1014, f2bits(5.0f));
    wle32(st->ram + 0x1018, f2bits(0.0f));
    wle32(st->ram + 0x101C, f2bits(0.0f));

    run_until_break(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK (VDIV/VSQRT/VRSQRT/VWAITQ recognized)");

    CHECK(bits2f((uint32_t)st->gpr[7].ud0) == 5.0f,
          "VDIV Q,VF1x,VF2x computes 20.0/4.0 = 5.0");

    CHECK((uint32_t)st->gpr[8].ud0 == 0x7F7FFFFFu,
          "VDIV Q,VF1y,VF2z (0/0, same sign) clamps to signed +FLT_MAX (0x7F7FFFFF)");

    CHECK(bits2f((uint32_t)st->gpr[9].ud0) == 3.0f,
          "VSQRT Q,VF1w computes sqrt(|-9.0|) = 3.0");

    CHECK(bits2f((uint32_t)st->gpr[10].ud0) == 10.0f,
          "VRSQRT Q,VF1x,VF2x computes 20.0/sqrt(4.0) = 10.0");

    CHECK((uint32_t)st->gpr[11].ud0 == 0x7F7FFFFFu,
          "VRSQRT Q,VF1x,VF2z (ft=0, fs!=0, same sign) clamps to signed +FLT_MAX");

    CHECK((uint32_t)st->gpr[12].ud0 == 0x7F7FFFFFu,
          "VWAITQ is a true no-op - Q is unchanged from the preceding VRSQRT result");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
