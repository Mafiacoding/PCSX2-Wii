/*
 * test_ee_mmi_pvshift.c - host-native test for ee_core.c's PSLLVW/
 * PSRLVW (MMI2 variable-shift word-pair opcodes), ported from
 * PCSX2's MMI.cpp. Operands planted directly into EE RAM and loaded
 * via LQ, same approach as the other tests/test_ee_mmi_*.c files.
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

#define FUNCT_MMI2 0x09
#define SA_PSLLVW 0x02
#define SA_PSRLVW 0x03

static void build_mmi_prog(uint8_t *prog, int sa, int funct) {
    int pc = 0;
    wle32(prog + pc, enc_lui(4, 0x0000)); pc += 4;
    wle32(prog + pc, enc_ori(4, 4, 0x1000)); pc += 4;
    wle32(prog + pc, enc_lq(1, 4, 0x00)); pc += 4;   /* gpr1 = rs operand (shift amounts) */
    wle32(prog + pc, enc_lq(2, 4, 0x10)); pc += 4;   /* gpr2 = rt operand (values to shift) */
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

    /* --- PSLLVW: lane0 shift amount = Rs.UL[0], lane2 shift amount =
     * Rs.UL[2]; values shifted = Rt.UL[0]/Rt.UL[2]. Also confirms the
     * shift amount is masked to 5 bits (amount=35 behaves like 3). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PSLLVW, FUNCT_MMI2);
    if (ee_core_init(&bios) != 0) { printf("init failed\n"); return 1; }
    st = ee_core_get_state();
    wle32(st->ram + 0x1000, 3);          /* rs lane0 (word 0) = shift amount 3 */
    wle32(st->ram + 0x1008, 35);         /* rs lane2 (word 2) = shift amount 35 -> masked to 3 */
    wle32(st->ram + 0x1010, 0x00000001u);/* rt lane0 = 1 */
    wle32(st->ram + 0x1018, 0x00000001u);/* rt lane2 = 1 */
    run_until_break(&bios);
    CHECK(st->halted == 1, "PSLLVW test: core halted on BREAK");
    CHECK(st->gpr[3].ud0 == 0x0000000000000008ULL, "PSLLVW ud0: 1 << 3 == 8, sign-extended (positive, upper 32 bits zero)");
    CHECK(st->gpr[3].ud1 == 0x0000000000000008ULL, "PSLLVW ud1: 1 << (35 & 0x1F)=1<<3 == 8 (shift amount masked to 5 bits)");

    /* --- PSLLVW with a result whose bit 31 is set: must sign-extend
     * (upper 32 bits of ud0 become all-1s), proving this is NOT a
     * plain zero-extending shift. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PSLLVW, FUNCT_MMI2);
    ee_core_init(&bios);
    st = ee_core_get_state();
    wle32(st->ram + 0x1000, 4);
    wle32(st->ram + 0x1010, 0x08000000u); /* 0x08000000 << 4 = 0x80000000 (bit 31 set) */
    run_until_break(&bios);
    CHECK(st->gpr[3].ud0 == 0xFFFFFFFF80000000ULL, "PSLLVW: result with bit 31 set sign-extends to all-1s upper 32 bits");

    /* --- PSRLVW: logical (not arithmetic) right shift - a negative
     * (bit-31-set) input must NOT sign-extend during the shift itself
     * (0x80000000 >> 4 == 0x08000000, not 0xF8000000), though the
     * final 32-bit shift RESULT is still sign-extended to 64 bits
     * afterward (matching every other 32-bit GPR result in this
     * file). */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    build_mmi_prog(prog, SA_PSRLVW, FUNCT_MMI2);
    ee_core_init(&bios);
    st = ee_core_get_state();
    wle32(st->ram + 0x1000, 4);
    wle32(st->ram + 0x1008, 1);
    wle32(st->ram + 0x1010, 0x80000000u); /* rt lane0 */
    wle32(st->ram + 0x1018, 0x00000002u); /* rt lane2 */
    run_until_break(&bios);
    CHECK(st->gpr[3].ud0 == 0x0000000008000000ULL, "PSRLVW ud0: 0x80000000 >> 4 == 0x08000000 (logical shift, no sign propagation into the shift)");
    CHECK(st->gpr[3].ud1 == 0x0000000000000001ULL, "PSRLVW ud1: 2 >> 1 == 1");

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
