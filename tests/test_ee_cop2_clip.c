/* test_ee_cop2_clip.c - host-native test for Round 29 continued's
 * 26th change: VCLIPw (idx31) - the last remaining VU0 macro-mode
 * gap this session. Judges |VF[fs].x|,|VF[fs].y|,|VF[fs].z| against
 * |VF[ft].w| via a raw-bit signed-integer sign-flip XOR trick (NOT a
 * float comparison), ported bit-exact from a real PCSX2 upstream
 * reference clone's VUops.cpp _vuCLIP. Result is shifted into the
 * CLIP flag register (this project's cop2_ctrl[18], REG_CLIP_FLAG=18
 * in PCSX2's VU.h), readable back via CFC2 like R(20)/I(21)/Q(22).
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
 * 0xF for the outer funct dispatch. destmask is unused by VCLIPw
 * (no Fsf/Ftf field - xyz vs w is hardwired) but still must be
 * present in the "rs" field position for the outer CO-format check. */
static uint32_t enc_special2(uint32_t destmask, int ft_field, int fs_field, uint32_t idx)
{
    uint32_t f6_10 = (idx >> 2) & 0x1Fu;
    uint32_t f0_1 = idx & 0x3u;
    return (0x12u << 26) | ((0x10u | destmask) << 21) | ((uint32_t)ft_field << 16)
         | ((uint32_t)fs_field << 11) | (f6_10 << 6) | (0xFu << 2) | f0_1;
}

static void wle32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
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

    /* r4 = 0x1000 (RAM base pointer) */
    wle32(prog + (i++)*4, enc_lui(4, 0x0000));
    wle32(prog + (i++)*4, enc_ori(4, 4, 0x1000));

    /* LQ r5 <- RAM[0x1000] (VF1: x=10.0, y=-2.0, z=3.0, w=unused) */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
    wle32(prog + (i++)*4, enc_qmtc2(5, 1));
    /* LQ r6 <- RAM[0x1010] (VF2: w=5.0 at lane W, others unused) */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (6 << 16) | 0x0010);
    wle32(prog + (i++)*4, enc_qmtc2(6, 2));

    /* VCLIPw VF1xyz, VF2w: fs=1 (VF1, xyz), ft=2 (VF2, w=5.0)
     * x=10.0 > 5.0  -> bit0 (pos) set, bit1 (neg) not
     * y=-2.0: |y|=2.0 <= 5.0 -> neither bit2 nor bit3 set
     * z=3.0 <= 5.0 -> neither bit4 nor bit5 set
     * expected new 6 bits = 0b000001 = 0x01, clipflag was 0 -> 0x01 */
    wle32(prog + (i++)*4, enc_special2(0, 2, 1, 31));
    wle32(prog + (i++)*4, enc_cfc2(7, 18));

    /* Second VCLIPw call with different values to prove the 6-bit
     * shift-in history behavior: VF1 x=10.0,y=-2.0,z=3.0 vs VF2 w
     * changed to 1.0 (LQ fresh word into VF2.w via a second load) */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (8 << 16) | 0x0020);
    wle32(prog + (i++)*4, enc_qmtc2(8, 3)); /* VF3 = RAM[0x1020]: w=1.0 */
    /* VCLIPw VF1xyz, VF3w: x=10.0>1.0(bit0), y=-2.0 |y|=2.0>1.0(bit3, neg), z=3.0>1.0(bit4)
     * new bits = bit0|bit3|bit4 = 0x01|0x08|0x10 = 0x19
     * clipflag = (0x01 << 6) | 0x19 = 0x40 | 0x19 = 0x59 */
    wle32(prog + (i++)*4, enc_special2(0, 3, 1, 31));
    wle32(prog + (i++)*4, enc_cfc2(9, 18));

    wle32(prog + (i++)*4, enc_break());

    if (ee_core_init(&bios) != 0) { printf("ee_core_init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    /* VF1 = (10.0, -2.0, 3.0, 0.0) */
    wle32(st->ram + 0x1000, f2bits(10.0f));
    wle32(st->ram + 0x1004, f2bits(-2.0f));
    wle32(st->ram + 0x1008, f2bits(3.0f));
    wle32(st->ram + 0x100C, f2bits(0.0f));
    /* VF2 = (0,0,0, 5.0) - only W matters */
    wle32(st->ram + 0x1010, f2bits(0.0f));
    wle32(st->ram + 0x1014, f2bits(0.0f));
    wle32(st->ram + 0x1018, f2bits(0.0f));
    wle32(st->ram + 0x101C, f2bits(5.0f));
    /* VF3 = (0,0,0, 1.0) - only W matters */
    wle32(st->ram + 0x1020, f2bits(0.0f));
    wle32(st->ram + 0x1024, f2bits(0.0f));
    wle32(st->ram + 0x1028, f2bits(0.0f));
    wle32(st->ram + 0x102C, f2bits(1.0f));

    run_until_break(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK (VCLIPw recognized)");

    CHECK((uint32_t)st->gpr[7].ud0 == 0x01u,
          "VCLIPw VF1xyz,VF2w: x=10.0>5.0 sets bit0 only, clipflag = 0x01");

    CHECK((uint32_t)st->gpr[9].ud0 == 0x59u,
          "second VCLIPw shifts previous 0x01 left by 6 and ORs in new bits 0x19 -> 0x59");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
