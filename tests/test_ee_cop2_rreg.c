/* test_ee_cop2_rreg.c - host-native test for Round 29 continued's
 * 24th change: the VU0 "R register" LFSR pseudo-random generator -
 * VRINIT (idx66), VRGET (idx65), VRNEXT (idx64), VRXOR (idx67).
 * Ported bit-exact from a real PCSX2 upstream reference clone's
 * VUops.cpp _vuRINIT/_vuRGET/AdvanceLFSR/_vuRNEXT/_vuRXOR. REG_R is
 * control register index 20 - always kept in the float-bit-pattern
 * range [1.0,2.0) (exponent/sign bits fixed at 0x3F800000, only the
 * low 23 mantissa bits vary).
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

static uint32_t enc_lui(int rt, uint16_t imm)        { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_break(void)                       { return 0x0D; }

static uint32_t enc_qmtc2(int rt, int vf) { return (0x12u << 26) | (0x05u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vf << 11); }
static uint32_t enc_qmfc2(int rt, int vf) { return (0x12u << 26) | (0x01u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vf << 11); }

/* SPECIAL2 idx = (instr&0x3) | ((instr>>4)&0x7C); bits 2-5 forced to
 * 0xF for the outer funct dispatch. destmask's low 2 bits double as
 * the Fsf lane selector for VRINIT/VRXOR (same convention as VMTIR). */
static uint32_t enc_special2(uint32_t destmask, int ft_field, int fs_field, uint32_t idx)
{
    uint32_t f6_10 = (idx >> 2) & 0x1Fu;
    uint32_t f0_1 = idx & 0x3u;
    return (0x12u << 26) | ((0x10u | destmask) << 21) | ((uint32_t)ft_field << 16)
         | ((uint32_t)fs_field << 11) | (f6_10 << 6) | (0xFu << 2) | f0_1;
}

static void wle32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }

/* Host-side reference model of AdvanceLFSR, to compute the expected
 * VRNEXT result independently rather than hand-deriving bit patterns. */
static uint32_t ref_advance_lfsr(uint32_t r)
{
    uint32_t x = (r >> 4) & 1u;
    uint32_t y = (r >> 22) & 1u;
    r <<= 1;
    r ^= (x ^ y);
    r = (r & 0x7FFFFFu) | 0x3F800000u;
    return r;
}

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

    /* LQ r5 <- RAM[0x1000] (VF1: raw bit patterns, X lane = seed) */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
    wle32(prog + (i++)*4, enc_qmtc2(5, 1));

    /* VRINIT R,VF1x (Fsf lane=0=X, destmask param=0): seeds R from VF1.x's raw pattern */
    wle32(prog + (i++)*4, enc_special2(0, 0, 1, 66));

    /* VRGET.xyzw VF2,R: broadcast R's current value (unchanged since VRINIT) into all 4 lanes */
    wle32(prog + (i++)*4, enc_special2(0xF, 2, 0, 65));
    wle32(prog + (i++)*4, enc_qmfc2(6, 2));

    /* VRNEXT.xyzw VF3,R: advance R's LFSR, then broadcast the NEW value */
    wle32(prog + (i++)*4, enc_special2(0xF, 3, 0, 64));
    wle32(prog + (i++)*4, enc_qmfc2(7, 3));

    /* A second VRNEXT to prove consecutive calls keep advancing (not idempotent) */
    wle32(prog + (i++)*4, enc_special2(0xF, 4, 0, 64));
    wle32(prog + (i++)*4, enc_qmfc2(8, 4));

    /* VRXOR R,VF1y (Fsf lane=1=Y, destmask param=1): XOR R's mantissa with VF1.y's raw pattern */
    wle32(prog + (i++)*4, enc_special2(1, 0, 1, 67));
    wle32(prog + (i++)*4, enc_special2(0xF, 5, 0, 65)); /* VRGET.xyzw VF5,R to read the XOR result */
    wle32(prog + (i++)*4, enc_qmfc2(9, 5));

    wle32(prog + (i++)*4, enc_break());

    if (ee_core_init(&bios) != 0) { printf("ee_core_init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    /* VF1.x = seed, VF1.y = a second raw pattern for the VRXOR check */
    wle32(st->ram + 0x1000, 0x00123456u);
    wle32(st->ram + 0x1004, 0x00ABCDEFu);
    wle32(st->ram + 0x1008, 0u);
    wle32(st->ram + 0x100C, 0u);

    uint32_t expected_seed = 0x3F800000u | (0x00123456u & 0x7FFFFFu);
    uint32_t expected_next1 = ref_advance_lfsr(expected_seed);
    uint32_t expected_next2 = ref_advance_lfsr(expected_next1);
    uint32_t expected_xor = 0x3F800000u | ((expected_next2 ^ 0x00ABCDEFu) & 0x7FFFFFu);

    ee_core_run(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK (VRINIT/VRGET/VRNEXT/VRXOR recognized)");

    uint32_t vf2x = (uint32_t)(st->gpr[6].ud0 & 0xFFFFFFFFu);
    CHECK(vf2x == expected_seed,
          "VRINIT seeds R to 0x3F800000|(seed&0x7FFFFF), then VRGET reads it back unchanged");

    uint32_t vf3x = (uint32_t)(st->gpr[7].ud0 & 0xFFFFFFFFu);
    uint32_t vf3y = (uint32_t)(st->gpr[7].ud0 >> 32);
    uint32_t vf3z = (uint32_t)(st->gpr[7].ud1 & 0xFFFFFFFFu);
    uint32_t vf3w = (uint32_t)(st->gpr[7].ud1 >> 32);
    CHECK(vf3x == expected_next1 && vf3y == expected_next1 && vf3z == expected_next1 && vf3w == expected_next1,
          "VRNEXT advances the LFSR once and broadcasts the new value into all 4 lanes, matching the reference AdvanceLFSR model");

    uint32_t vf4x = (uint32_t)(st->gpr[8].ud0 & 0xFFFFFFFFu);
    CHECK(vf4x == expected_next2 && vf4x != vf3x,
          "a second VRNEXT advances the LFSR again (not idempotent), matching the reference model's second step");

    uint32_t vf5x = (uint32_t)(st->gpr[9].ud0 & 0xFFFFFFFFu);
    CHECK(vf5x == expected_xor,
          "VRXOR XORs R's mantissa with VF1.y's raw pattern and re-clamps to the [1.0,2.0) bit pattern, matching the reference model");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
