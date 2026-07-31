/*
 * test_ee_fpu4.c - host-native test for ee_core.c's BC1FL/BC1TL
 * (Round 400): the "likely" branch variants of BC1F/BC1T. Real MIPS
 * II+ semantics (matches this project's already-established integer
 * likely-branch family - BEQL/BNEL/BLEZL/BGTZL, BLTZL/BGEZL): when the
 * branch is NOT taken, the delay-slot instruction is nullified
 * (skipped entirely, never executed) rather than always running like
 * an ordinary branch's delay slot. This test proves both halves of
 * that: (a) when taken, behaves like BC1F/BC1T (delay slot DOES
 * execute, matching every other "likely" test in this project); (b)
 * when NOT taken, the delay slot's own effect (a LUI marker) must be
 * completely absent - the clearest possible proof of nullification.
 */
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include "core/ee/ee_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static void run_until_break(const bios_image_t *bios) {
    (void)bios;
    ee_state_t *st = ee_core_get_state();
    long guard;
    for (guard = 0; guard < 2000000L; guard++) {
        if (ee_core_step()) return;
        if (((st->cop0[13] >> 2) & 0x1Fu) == 9u && (st->cop0[12] & 0x2u) != 0u) {
            st->halted = 1;
            snprintf(st->halt_reason, sizeof(st->halt_reason),
                     "BREAK (real Breakpoint exception raised, ExcCode 9)");
            return;
        }
    }
    st->halted = 1;
    snprintf(st->halt_reason, sizeof(st->halt_reason),
             "run_until_break() safety cap reached without a Breakpoint exception");
}

static uint32_t enc_lui(int rt, uint16_t imm) { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_mtc1(int rt, int fs) { return (0x11 << 26) | (0x04 << 21) | (rt << 16) | (fs << 11); }
static uint32_t enc_cop1_s(int funct, int fd, int fs, int ft) {
    return (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
}
static uint32_t enc_ceq_s(int fs, int ft) { return enc_cop1_s(0x32, 0, fs, ft); }
static uint32_t enc_bc1(int cond, int16_t imm) { return (0x11 << 26) | (0x08 << 21) | (cond << 16) | (uint16_t)imm; }
static uint32_t enc_nop(void) { return 0; }
static uint32_t enc_break(void) { return 0x0D; }
static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }
static uint32_t float_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

static void load_float(uint8_t *p, int *pc, int greg, int freg, float val) {
    uint32_t bits = float_bits(val);
    wle32(p + *pc, enc_lui(greg, (uint16_t)(bits >> 16))); *pc += 4;
    wle32(p + *pc, enc_ori(greg, greg, (uint16_t)(bits & 0xFFFF))); *pc += 4;
    wle32(p + *pc, enc_mtc1(greg, freg)); *pc += 4;
}

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    uint8_t *p = bios.data;
    int pc;
    ee_state_t *st;

    /* --- Test 1: BC1TL, flag SET -> taken, delay slot DOES execute
     * (r4 gets clobbered by the delay-slot LUI before the branch
     * target's own LUI runs - both are observable in sequence, so
     * check final r4 == branch target's value, same convention as
     * the existing BC1F/BC1T test in test_ee_fpu2.c). */
    pc = 0;
    load_float(p, &pc, 1, 1, 5.0f);
    load_float(p, &pc, 2, 2, 5.0f);
    wle32(p+pc, enc_ceq_s(1, 2)); pc += 4;              /* flag SET (5.0==5.0) */
    wle32(p+pc, enc_bc1(3, 2)); pc += 4;                /* BC1TL: taken -> +2*4=+8 past delay slot */
    wle32(p+pc, enc_lui(5, 0x00AA)); pc += 4;            /* delay slot: r5 = 0x00AA0000 (MUST execute) */
    wle32(p+pc, enc_lui(4, 0x0001)); pc += 4;            /* SKIPPED (branch target) */
    wle32(p+pc, enc_lui(4, 0x0002)); pc += 4;            /* branch target: r4 = 0x00020000 */
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    run_until_break(&bios);
    CHECK((uint32_t)st->gpr[4].ud0 == 0x00020000u, "BC1TL (taken): landed on branch target");
    CHECK((uint32_t)st->gpr[5].ud0 == 0x00AA0000u, "BC1TL (taken): delay slot DID execute (r5 set)");

    /* --- Test 2: BC1TL, flag CLEAR -> NOT taken, delay slot must be
     * NULLIFIED (r5 must stay 0 - the clearest possible proof this
     * isn't just an ordinary conditional branch). Falls through to
     * the very next instruction after the delay slot. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 5.0f);
    load_float(p, &pc, 2, 2, 9.0f);
    wle32(p+pc, enc_ceq_s(1, 2)); pc += 4;              /* flag CLEAR (5.0!=9.0) */
    wle32(p+pc, enc_bc1(3, 2)); pc += 4;                /* BC1TL: NOT taken */
    wle32(p+pc, enc_lui(5, 0x00AA)); pc += 4;            /* delay slot: MUST be nullified (r5 stays 0) */
    wle32(p+pc, enc_lui(4, 0x0001)); pc += 4;            /* fallthrough: r4 = 0x00010000 */
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    run_until_break(&bios);
    CHECK((uint32_t)st->gpr[4].ud0 == 0x00010000u, "BC1TL (not taken): fell through past the nullified delay slot");
    CHECK((uint32_t)st->gpr[5].ud0 == 0u, "BC1TL (not taken): delay slot was NULLIFIED (r5 still 0)");

    /* --- Test 3: BC1FL, flag CLEAR -> taken. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 5.0f);
    load_float(p, &pc, 2, 2, 9.0f);
    wle32(p+pc, enc_ceq_s(1, 2)); pc += 4;              /* flag CLEAR */
    wle32(p+pc, enc_bc1(2, 2)); pc += 4;                /* BC1FL: taken */
    wle32(p+pc, enc_lui(5, 0x00AA)); pc += 4;            /* delay slot: MUST execute */
    wle32(p+pc, enc_lui(4, 0x0001)); pc += 4;            /* SKIPPED */
    wle32(p+pc, enc_lui(4, 0x0002)); pc += 4;            /* branch target */
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    run_until_break(&bios);
    CHECK((uint32_t)st->gpr[4].ud0 == 0x00020000u, "BC1FL (taken): landed on branch target");
    CHECK((uint32_t)st->gpr[5].ud0 == 0x00AA0000u, "BC1FL (taken): delay slot DID execute (r5 set)");

    /* --- Test 4: BC1FL, flag SET -> NOT taken, delay slot nullified. */
    memset(bios.data, 0, BIOS_MAX_SIZE);
    pc = 0;
    load_float(p, &pc, 1, 1, 5.0f);
    load_float(p, &pc, 2, 2, 5.0f);
    wle32(p+pc, enc_ceq_s(1, 2)); pc += 4;              /* flag SET */
    wle32(p+pc, enc_bc1(2, 2)); pc += 4;                /* BC1FL: NOT taken */
    wle32(p+pc, enc_lui(5, 0x00AA)); pc += 4;            /* delay slot: MUST be nullified */
    wle32(p+pc, enc_lui(4, 0x0001)); pc += 4;            /* fallthrough */
    wle32(p+pc, enc_break()); pc += 4;
    ee_core_init(&bios);
    st = ee_core_get_state();
    run_until_break(&bios);
    CHECK((uint32_t)st->gpr[4].ud0 == 0x00010000u, "BC1FL (not taken): fell through past the nullified delay slot");
    CHECK((uint32_t)st->gpr[5].ud0 == 0u, "BC1FL (not taken): delay slot was NULLIFIED (r5 still 0)");

    printf("\n%d failures\n", failures);
    return failures != 0;
}
