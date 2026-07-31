/* test_ee_cop2_arith2.c - host-native test for Round 29 continued's
 * 10th change: extending Round 13's VU0 macro-mode vector datapath
 * with VADD (funct 0x28) and VMUL (funct 0x2A) - the same 3-operand
 * full-vector shape already implemented and tested for VSUB (funct
 * 0x2C) - plus VIADDI (funct 0x32), an immediate-integer add closing
 * a gap explicitly flagged next to VIADD/VISUB/VIAND/VIOR's own
 * comment in source/core/ee/ee_core.c. This file also adds first-time
 * coverage for VIADD/VISUB/VIAND/VIOR themselves (implemented in
 * Round 13 but, on inspection, never covered by a host-native test
 * until now).
 *
 * All funct codes and operand-field roles (dest=FD for VIADD/VISUB/
 * VIAND/VIOR, dest=FT for VIADDI, imm=SA/bits6-10 for VIADDI) are
 * cited against a real PCSX2 upstream reference clone
 * (R5900OpcodeTables.cpp's Int_COP2SPECIAL1PrintTable and
 * DisR5900asm.cpp's P_VIADD/P_VIADDI/etc disassembly formatters), not
 * guessed - see docs/STATUS.md's "Round 29 continued (10th change)"
 * section for the full derivation.
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

/* COP2 32-bit control-register transfer family (Round 12): op=0x12,
 * rs=sub-op (0x04=MTC2, 0x02=CFC2), rt=GPR, rd=VI reg. */
static uint32_t enc_cop2_xfer(int sub, int rt, int rd) { return (0x12u << 26) | ((uint32_t)sub << 21) | ((uint32_t)rt << 16) | ((uint32_t)rd << 11); }

/* COP2 128-bit transfer family: QMTC2 (rs=0x05)/QMFC2 (rs=0x01). rt=GPR, rd=VF reg. */
static uint32_t enc_qmtc2(int rt, int vf) { return (0x12u << 26) | (0x05u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vf << 11); }
static uint32_t enc_qmfc2(int rt, int vf) { return (0x12u << 26) | (0x01u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vf << 11); }

/* CO-format vector op: rs = 0x10 | destmask, funct in low 6 bits. */
static uint32_t enc_co(uint32_t destmask, int ft, int fs, int fd, uint32_t funct)
{
    return (0x12u << 26) | ((0x10u | destmask) << 21) | ((uint32_t)ft << 16) | ((uint32_t)fs << 11) | ((uint32_t)fd << 6) | funct;
}
/* VADD/VMUL.xyzw FD,FS,FT - full-vector 3-operand float arithmetic. */
static uint32_t enc_vadd(uint32_t destmask, int fd, int fs, int ft) { return enc_co(destmask, ft, fs, fd, 0x28); }
static uint32_t enc_vmul(uint32_t destmask, int fd, int fs, int ft) { return enc_co(destmask, ft, fs, fd, 0x2A); }

/* VIADD/VISUB/VIAND/VIOR FD,FS,FT - integer VI-register ALU
 * (dest=FD, confirmed via PCSX2's own P_VIADD format string). */
static uint32_t enc_viadd(int fd, int fs, int ft) { return enc_co(0, ft, fs, fd, 0x30); }
static uint32_t enc_visub(int fd, int fs, int ft) { return enc_co(0, ft, fs, fd, 0x31); }
static uint32_t enc_viand(int fd, int fs, int ft) { return enc_co(0, ft, fs, fd, 0x34); }
static uint32_t enc_vior(int fd, int fs, int ft)  { return enc_co(0, ft, fs, fd, 0x35); }
/* VIADDI FT,FS,imm5 - dest=FT (NOT FD, unlike the plain-ALU family
 * above), imm is the raw 5-bit field at the SAME bit position as FD. */
static uint32_t enc_viaddi(int ft, int fs, uint32_t imm5) { return enc_co(0, ft, fs, (int)(imm5 & 0x1Fu), 0x32); }

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

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

    /* --- Set up VF1 = (2.0, 3.0, 4.0, 5.0) and VF2 = (10.0, 20.0, 30.0, 40.0)
     * via QMTC2 from GPRs we build with LUI/ORI, LQ-free: instead we
     * plant the 128-bit patterns directly in RAM and LQ them, same
     * technique as test_ee_cop2_vu0.c. */
    (void)0;

    /* r4 = 0x80001000 (RAM base pointer, Round 363: KSEG0 direct-mapped, not raw KUSEG) */
    wle32(prog + (i++)*4, enc_lui(4, 0x8000));
    wle32(prog + (i++)*4, enc_ori(4, 4, 0x1000));

    /* LQ r5 <- RAM[0x1000+0x00] (VF1 pattern), LQ r6 <- RAM[0x1000+0x10] (VF2 pattern) */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000); /* lq r5,0(r4) */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (6 << 16) | 0x0010); /* lq r6,16(r4) */
    wle32(prog + (i++)*4, enc_qmtc2(5, 1)); /* VF1 = r5 */
    wle32(prog + (i++)*4, enc_qmtc2(6, 2)); /* VF2 = r6 */

    /* VADD.xyzw VF3,VF1,VF2 -> VF3 = VF1+VF2 elementwise */
    wle32(prog + (i++)*4, enc_vadd(0xF, 3, 1, 2));
    wle32(prog + (i++)*4, enc_qmfc2(7, 3));

    /* VMUL.xyzw VF4,VF1,VF2 -> VF4 = VF1*VF2 elementwise */
    wle32(prog + (i++)*4, enc_vmul(0xF, 4, 1, 2));
    wle32(prog + (i++)*4, enc_qmfc2(8, 4));

    /* VADD.x VF5,VF1,VF2 (X lane only) - VF5 starts at 0, so only lane0 changes */
    wle32(prog + (i++)*4, enc_vadd(0x8, 5, 1, 2));
    wle32(prog + (i++)*4, enc_qmfc2(9, 5));

    /* --- VI integer ALU family: VI1=5, VI2=3 via MTC2 --- */
    wle32(prog + (i++)*4, enc_lui(10, 0)); wle32(prog + (i++)*4, enc_ori(10, 10, 5));
    wle32(prog + (i++)*4, enc_lui(11, 0)); wle32(prog + (i++)*4, enc_ori(11, 11, 3));
    wle32(prog + (i++)*4, enc_cop2_xfer(0x04, 10, 1)); /* MTC2 r10->VI1 (=5) */
    wle32(prog + (i++)*4, enc_cop2_xfer(0x04, 11, 2)); /* MTC2 r11->VI2 (=3) */

    wle32(prog + (i++)*4, enc_viadd(3, 1, 2)); /* VI3 = VI1+VI2 = 8 */
    wle32(prog + (i++)*4, enc_visub(4, 1, 2)); /* VI4 = VI1-VI2 = 2 */
    wle32(prog + (i++)*4, enc_viand(5, 1, 2)); /* VI5 = VI1&VI2 = 1 */
    wle32(prog + (i++)*4, enc_vior(6, 1, 2));  /* VI6 = VI1|VI2 = 7 */
    wle32(prog + (i++)*4, enc_viaddi(7, 1, 10)); /* VI7 = VI1 + 10 = 15 (imm5=10, bit4=0 => +10) */
    wle32(prog + (i++)*4, enc_viaddi(8, 1, 0x1E)); /* VI8 = VI1 + sext(0x1E): imm5=0x1E -> bit4 set -> 0xFFF0|0xE = -2 -> VI8 = 5-2 = 3 */
    wle32(prog + (i++)*4, enc_viaddi(0, 1, 5)); /* VIADDI to VI0 (dest=0) must be a no-op - VI0 hardwired 0 */

    wle32(prog + (i++)*4, enc_cop2_xfer(0x02, 20, 3)); /* CFC2 VI3->r20 */
    wle32(prog + (i++)*4, enc_cop2_xfer(0x02, 21, 4)); /* CFC2 VI4->r21 */
    wle32(prog + (i++)*4, enc_cop2_xfer(0x02, 22, 5)); /* CFC2 VI5->r22 */
    wle32(prog + (i++)*4, enc_cop2_xfer(0x02, 23, 6)); /* CFC2 VI6->r23 */
    wle32(prog + (i++)*4, enc_cop2_xfer(0x02, 24, 7)); /* CFC2 VI7->r24 */
    wle32(prog + (i++)*4, enc_cop2_xfer(0x02, 25, 8)); /* CFC2 VI8->r25 */
    wle32(prog + (i++)*4, enc_cop2_xfer(0x02, 26, 0)); /* CFC2 VI0->r26 (must stay 0) */

    wle32(prog + (i++)*4, enc_break());

    if (ee_core_init(&bios) != 0) { printf("ee_core_init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    /* VF1 = (2.0, 3.0, 4.0, 5.0) */
    wle32(st->ram + 0x1000, f2bits(2.0f));
    wle32(st->ram + 0x1004, f2bits(3.0f));
    wle32(st->ram + 0x1008, f2bits(4.0f));
    wle32(st->ram + 0x100C, f2bits(5.0f));
    /* VF2 = (10.0, 20.0, 30.0, 40.0) */
    wle32(st->ram + 0x1010, f2bits(10.0f));
    wle32(st->ram + 0x1014, f2bits(20.0f));
    wle32(st->ram + 0x1018, f2bits(30.0f));
    wle32(st->ram + 0x101C, f2bits(40.0f));

    run_until_break(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK (VADD/VMUL/VIADD-family/VIADDI all recognized)");

    /* VF3 = VF1+VF2 = (12,23,34,45) */
    uint32_t vf3x = (uint32_t)(st->gpr[7].ud0 & 0xFFFFFFFFu);
    uint32_t vf3y = (uint32_t)(st->gpr[7].ud0 >> 32);
    uint32_t vf3z = (uint32_t)(st->gpr[7].ud1 & 0xFFFFFFFFu);
    uint32_t vf3w = (uint32_t)(st->gpr[7].ud1 >> 32);
    CHECK(bits2f(vf3x) == 12.0f && bits2f(vf3y) == 23.0f && bits2f(vf3z) == 34.0f && bits2f(vf3w) == 45.0f,
          "VADD.xyzw VF3,VF1,VF2 computes (12,23,34,45)");

    /* VF4 = VF1*VF2 = (20,60,120,200) */
    uint32_t vf4x = (uint32_t)(st->gpr[8].ud0 & 0xFFFFFFFFu);
    uint32_t vf4y = (uint32_t)(st->gpr[8].ud0 >> 32);
    uint32_t vf4z = (uint32_t)(st->gpr[8].ud1 & 0xFFFFFFFFu);
    uint32_t vf4w = (uint32_t)(st->gpr[8].ud1 >> 32);
    CHECK(bits2f(vf4x) == 20.0f && bits2f(vf4y) == 60.0f && bits2f(vf4z) == 120.0f && bits2f(vf4w) == 200.0f,
          "VMUL.xyzw VF4,VF1,VF2 computes (20,60,120,200)");

    /* VF5 = VADD.x only: x=12.0, y/z/w untouched (VF5 started at 0) */
    uint32_t vf5x = (uint32_t)(st->gpr[9].ud0 & 0xFFFFFFFFu);
    uint32_t vf5y = (uint32_t)(st->gpr[9].ud0 >> 32);
    CHECK(bits2f(vf5x) == 12.0f && bits2f(vf5y) == 0.0f,
          "VADD.x VF5,VF1,VF2 only writes the X lane (destmask honored), Y stays 0");

    CHECK(st->gpr[20].ud0 == 8ULL,  "VIADD  VI3,VI1,VI2 = 5+3 = 8");
    CHECK(st->gpr[21].ud0 == 2ULL,  "VISUB  VI4,VI1,VI2 = 5-3 = 2");
    CHECK(st->gpr[22].ud0 == 1ULL,  "VIAND  VI5,VI1,VI2 = 5&3 = 1");
    CHECK(st->gpr[23].ud0 == 7ULL,  "VIOR   VI6,VI1,VI2 = 5|3 = 7");
    CHECK(st->gpr[24].ud0 == 15ULL, "VIADDI VI7,VI1,10  = 5+10 = 15");
    CHECK(st->gpr[25].ud0 == 3ULL,  "VIADDI VI8,VI1,0x1E (sign-extends to -2) = 5-2 = 3");
    CHECK(st->gpr[26].ud0 == 0ULL,  "VIADDI to VI0 (dest=0) is a no-op - VI0 stays hardwired 0");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
