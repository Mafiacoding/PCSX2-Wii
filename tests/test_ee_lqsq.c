/*
 * test_ee_lqsq.c - host-native test for ee_core.c's LQ/SQ (128-bit
 * load/store, primary opcodes 0x1E/0x1F), ported from PCSX2's
 * R5900OpcodeImpl.cpp. Added after real BIOS testing showed the EE
 * running 53M+ real instructions before halting on exactly this gap
 * (see docs/STATUS.md).
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

static uint32_t enc_lui(int rt, uint16_t imm)          { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm)   { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_lq(int rt, int rs, int16_t imm)     { return (0x1E << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_sq(int rt, int rs, int16_t imm)     { return (0x1F << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_break(void)                          { return 0x0D; }

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }
static void wle64(uint8_t *p, uint64_t v) { for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint64_t rle64(uint8_t *p) { uint64_t v=0; for (int i=0;i<8;i++) v |= ((uint64_t)p[i])<<(8*i); return v; }

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = calloc(1, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    uint8_t *prog = bios.data; /* EE boots straight from the BIOS reset vector */

    /* Program: build r4 = 0x100 (base pointer), then plant a known
     * 128-bit pattern directly in RAM via memcpy (below) at r4+0x20,
     * LQ it into r5, then SQ r5 back out to r4+0x40, then BREAK. */
    int i = 0;
    wle32(prog + (i++)*4, enc_lui(4, 0x0000));
    wle32(prog + (i++)*4, enc_ori(4, 4, 0x1000));        /* r4 = 0x1000 (base) */
    wle32(prog + (i++)*4, enc_lq(5, 4, 0x20));           /* r5 = *(qword*)(r4+0x20), should mask to 16-byte align already */
    wle32(prog + (i++)*4, enc_sq(5, 4, 0x43));           /* SQ with an UNALIGNED offset (0x43) - must mask down to 0x40 */
    wle32(prog + (i++)*4, enc_break());

    if (ee_core_init(&bios) != 0) { printf("ee_core_init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    /* Plant the known 128-bit pattern at RAM address 0x1020 (r4+0x20) */
    wle64(st->ram + 0x1020, 0x1122334455667788ULL); /* low 64 bits (ud0) */
    wle64(st->ram + 0x1028, 0x99AABBCCDDEEFF00ULL); /* high 64 bits (ud1) */

    /* Also plant garbage at 0x1040-0x104F to prove SQ actually
     * overwrites it, and at 0x1050 to prove SQ doesn't overrun. */
    wle64(st->ram + 0x1040, 0xDEADDEADDEADDEADULL);
    wle64(st->ram + 0x1048, 0xDEADDEADDEADDEADULL);
    wle64(st->ram + 0x1050, 0xCAFECAFECAFECAFEULL);

    run_until_break(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK");

    CHECK(st->gpr[5].ud0 == 0x1122334455667788ULL,
          "LQ loaded the low 64 bits (ud0) correctly");
    CHECK(st->gpr[5].ud1 == 0x99AABBCCDDEEFF00ULL,
          "LQ loaded the high 64 bits (ud1) correctly");

    uint64_t out_lo = rle64(st->ram + 0x1040);
    uint64_t out_hi = rle64(st->ram + 0x1048);
    CHECK(out_lo == 0x1122334455667788ULL,
          "SQ wrote the low 64 bits correctly (offset masked from 0x43 down to 0x40)");
    CHECK(out_hi == 0x99AABBCCDDEEFF00ULL,
          "SQ wrote the high 64 bits correctly");

    uint64_t untouched = rle64(st->ram + 0x1050);
    CHECK(untouched == 0xCAFECAFECAFECAFEULL,
          "SQ did not overrun past its 16-byte-aligned target (byte at +0x10 untouched)");

    /* rt==$0: LQ must skip the read entirely (real PCSX2 behavior -
     * unlike other loads in this file, which still perform a
     * discarded read for its side effects). Prove it by pointing at
     * an address that would otherwise halt/crash if actually read -
     * simplest proof here is just that r0 stays exactly zero and no
     * halt/crash occurs. */
    ee_core_init(&bios);
    st = ee_core_get_state();
    i = 0;
    wle32(prog + (i++)*4, enc_lq(0, 0, 0));  /* LQ $0, 0($0) - rt=$0 */
    wle32(prog + (i++)*4, enc_break());
    run_until_break(&bios);
    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "LQ with rt=$0 still completes cleanly (read skipped per real PCSX2 behavior)");
    CHECK(st->gpr[0].ud0 == 0 && st->gpr[0].ud1 == 0,
          "$0 remains hardwired to zero after LQ $0");

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
