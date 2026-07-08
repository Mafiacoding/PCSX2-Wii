/* test_ee_cop2_unary.c - host-native test for Round 29 continued's
 * 20th change: the COP2SPECIAL2 unary/data-movement cluster - VABS,
 * VITOF0/4/12/15, VFTOI0/4/12/15, VMOVE, VMR32.
 *
 * IMPORTANT field-role note (confirmed against a real PCSX2 upstream
 * reference clone's DisR5900asm.cpp/VUops.cpp): unlike the arithmetic
 * row (dest=FD), these ops write their DESTINATION into the FT field
 * position and read their SOURCE from FS - e.g. "vabs.xyzw vf2,vf1"
 * decodes as FT=vf2 (dest), FS=vf1 (source), FD unused.
 *
 * VITOF0/4/12/15 and VFTOI0/4/12/15 are ported bit-exact from PCSX2's
 * intToFloat<Offset>/floatToInt<Offset> templates (scale by a
 * bit-constructed power-of-two float constant, floatToInt also
 * saturates above a fixed exponent threshold) rather than a plain C
 * cast.
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

/* SPECIAL2 encoding: idx = (instr & 0x3) | ((instr >> 4) & 0x7C).
 * We need to construct an instr word whose low bits produce the
 * target idx, with rs=0x10|destmask, rt=dest(FT-role), rs(field
 * bits15-11)=source(FS-role, confusingly reusing our decoder's "fs"
 * variable name for the bits-15-11 field regardless of semantic
 * role). Bits 0-1 of instr = idx & 0x3; bits 4-9 = (idx >> 2) & 0x3F
 * (since (instr>>4)&0x7C means bits 4-10 shifted, i.e. idx bits 2-6
 * live in instr bits 6-10... let's just directly solve: idx =
 * (instr&3) | ((instr>>4)&0x7C). (instr>>4)&0x7C isolates instr bits
 * 6-10 (after removing bits 0-3 via >>4, then &0x7C keeps bits 2-6 of
 * the shifted value = instr bits 6-10) shifted down by... 0x7C =
 * 0b1111100, so bits 2-6 of (instr>>4) = instr bits 6-10. So idx bits
 * 2-6 = instr bits 6-10, and idx bits 0-1 = instr bits 0-1. Since our
 * arithmetic ops already place FD at instr bits 6-10 and funct at
 * bits 0-5, for these idx>=16 values we just need instr bits 6-10 to
 * equal (idx>>2) and instr bits 0-1 to equal (idx&3), with bits 2-5
 * (the rest of "funct") free - set to 0. */
static uint32_t enc_special2(uint32_t destmask, int dest_ft_role, int src_fs_role, uint32_t idx)
{
    /* idx = (instr&0x3) | ((instr>>4)&0x7C) uses instr bits 0-1 and
     * bits 6-10 only. But the TOP-LEVEL dispatch that routes into the
     * SPECIAL2 sub-decoder in the first place checks the raw 6-bit
     * "funct" field (instr bits 0-5) for (funct & 0x3C) == 0x3C - so
     * bits 2-5 must ALSO be forced to 0xF (as real VISWR/VSQI
     * encodings naturally have, confirmed by decoding their real funct
     * values 0x3F/0x3D in test_ee_cop2_vu0.c), independent of idx. */
    uint32_t funct_field6_10 = (idx >> 2) & 0x1Fu; /* instr bits 6-10 */
    uint32_t funct_field0_1 = idx & 0x3u;           /* instr bits 0-1 */
    return (0x12u << 26) | ((0x10u | destmask) << 21) | ((uint32_t)dest_ft_role << 16)
         | ((uint32_t)src_fs_role << 11) | (funct_field6_10 << 6) | (0xFu << 2) | funct_field0_1;
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

    /* r4 = 0x1000 (RAM base pointer) */
    wle32(prog + (i++)*4, enc_lui(4, 0x0000));
    wle32(prog + (i++)*4, enc_ori(4, 4, 0x1000));

    /* LQ r5 <- RAM[0x1000] (VF1: -2.5, 3.0, -4.0, 5.0) */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
    wle32(prog + (i++)*4, enc_qmtc2(5, 1));

    /* VABS.xyzw VF2,VF1 (idx=29): FT=vf2(dest), FS=vf1(source) */
    wle32(prog + (i++)*4, enc_special2(0xF, 2, 1, 29));
    wle32(prog + (i++)*4, enc_qmfc2(7, 2));

    /* VMOVE.xyzw VF3,VF1 (idx=48) */
    wle32(prog + (i++)*4, enc_special2(0xF, 3, 1, 48));
    wle32(prog + (i++)*4, enc_qmfc2(8, 3));

    /* VMR32.xyzw VF4,VF1 (idx=49): FT.x=FS.y,FT.y=FS.z,FT.z=FS.w,FT.w=FS.x */
    wle32(prog + (i++)*4, enc_special2(0xF, 4, 1, 49));
    wle32(prog + (i++)*4, enc_qmfc2(9, 4));

    /* Load VF5 = (16, 4096, -32768, 100) as raw ints for VITOF tests */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (6 << 16) | 0x0010);
    wle32(prog + (i++)*4, enc_qmtc2(6, 5));

    /* VITOF4.xyzw VF6,VF5 (idx=17): float = int / 16 */
    wle32(prog + (i++)*4, enc_special2(0xF, 6, 5, 17));
    wle32(prog + (i++)*4, enc_qmfc2(10, 6));

    /* VITOF12.xyzw VF8,VF5 (idx=18): float = int / 4096 */
    wle32(prog + (i++)*4, enc_special2(0xF, 8, 5, 18));
    wle32(prog + (i++)*4, enc_qmfc2(11, 8));

    /* Load VF9 = (2.0, -2.0, 100.5, 0.0625) as floats for VFTOI tests */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (12 << 16) | 0x0020);
    wle32(prog + (i++)*4, enc_qmtc2(12, 9));

    /* VFTOI0.xyzw VF10,VF9 (idx=20): int = truncate(float) */
    wle32(prog + (i++)*4, enc_special2(0xF, 10, 9, 20));
    wle32(prog + (i++)*4, enc_qmfc2(13, 10));

    /* VFTOI4.xyzw VF11,VF9 (idx=21): int = truncate(float * 16) */
    wle32(prog + (i++)*4, enc_special2(0xF, 11, 9, 21));
    wle32(prog + (i++)*4, enc_qmfc2(14, 11));

    /* VABS.x VF16,VF1 (single-lane destmask; VF16 starts at 0) */
    wle32(prog + (i++)*4, enc_special2(0x8, 16, 1, 29));
    wle32(prog + (i++)*4, enc_qmfc2(15, 16));

    wle32(prog + (i++)*4, enc_break());

    if (ee_core_init(&bios) != 0) { printf("ee_core_init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    /* VF1 = (-2.5, 3.0, -4.0, 5.0) */
    wle32(st->ram + 0x1000, f2bits(-2.5f));
    wle32(st->ram + 0x1004, f2bits(3.0f));
    wle32(st->ram + 0x1008, f2bits(-4.0f));
    wle32(st->ram + 0x100C, f2bits(5.0f));

    /* VF5 = (16, 4096, -32768, 100) as raw int32 bit patterns */
    wle32(st->ram + 0x1010, (uint32_t)16);
    wle32(st->ram + 0x1014, (uint32_t)4096);
    wle32(st->ram + 0x1018, (uint32_t)(-32768));
    wle32(st->ram + 0x101C, (uint32_t)100);

    /* VF9 = (2.0, -2.0, 100.5, 0.0625) */
    wle32(st->ram + 0x1020, f2bits(2.0f));
    wle32(st->ram + 0x1024, f2bits(-2.0f));
    wle32(st->ram + 0x1028, f2bits(100.5f));
    wle32(st->ram + 0x102C, f2bits(0.0625f));

    ee_core_run(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK (VABS/VITOF/VFTOI/VMOVE/VMR32 recognized)");

    uint32_t vf2x = (uint32_t)(st->gpr[7].ud0 & 0xFFFFFFFFu);
    uint32_t vf2y = (uint32_t)(st->gpr[7].ud0 >> 32);
    uint32_t vf2z = (uint32_t)(st->gpr[7].ud1 & 0xFFFFFFFFu);
    uint32_t vf2w = (uint32_t)(st->gpr[7].ud1 >> 32);
    CHECK(bits2f(vf2x) == 2.5f && bits2f(vf2y) == 3.0f && bits2f(vf2z) == 4.0f && bits2f(vf2w) == 5.0f,
          "VABS.xyzw VF2,VF1 (dest=FT,src=FS) computes |VF1|: (2.5,3,4,5)");

    uint32_t vf3x = (uint32_t)(st->gpr[8].ud0 & 0xFFFFFFFFu);
    uint32_t vf3y = (uint32_t)(st->gpr[8].ud0 >> 32);
    uint32_t vf3z = (uint32_t)(st->gpr[8].ud1 & 0xFFFFFFFFu);
    uint32_t vf3w = (uint32_t)(st->gpr[8].ud1 >> 32);
    CHECK(bits2f(vf3x) == -2.5f && bits2f(vf3y) == 3.0f && bits2f(vf3z) == -4.0f && bits2f(vf3w) == 5.0f,
          "VMOVE.xyzw VF3,VF1 copies VF1 unchanged: (-2.5,3,-4,5)");

    uint32_t vf4x = (uint32_t)(st->gpr[9].ud0 & 0xFFFFFFFFu);
    uint32_t vf4y = (uint32_t)(st->gpr[9].ud0 >> 32);
    uint32_t vf4z = (uint32_t)(st->gpr[9].ud1 & 0xFFFFFFFFu);
    uint32_t vf4w = (uint32_t)(st->gpr[9].ud1 >> 32);
    CHECK(bits2f(vf4x) == 3.0f && bits2f(vf4y) == -4.0f && bits2f(vf4z) == 5.0f && bits2f(vf4w) == -2.5f,
          "VMR32.xyzw VF4,VF1 rotates lanes: FT.x=FS.y,FT.y=FS.z,FT.z=FS.w,FT.w=FS.x -> (3,-4,5,-2.5)");

    uint32_t vf6x = (uint32_t)(st->gpr[10].ud0 & 0xFFFFFFFFu);
    uint32_t vf6y = (uint32_t)(st->gpr[10].ud0 >> 32);
    uint32_t vf6z = (uint32_t)(st->gpr[10].ud1 & 0xFFFFFFFFu);
    uint32_t vf6w = (uint32_t)(st->gpr[10].ud1 >> 32);
    CHECK(bits2f(vf6x) == 1.0f && bits2f(vf6y) == 256.0f && bits2f(vf6z) == -2048.0f && bits2f(vf6w) == 6.25f,
          "VITOF4.xyzw VF6,VF5 = int/16: (16/16=1, 4096/16=256, -32768/16=-2048, 100/16=6.25)");

    uint32_t vf8x = (uint32_t)(st->gpr[11].ud0 & 0xFFFFFFFFu);
    uint32_t vf8y = (uint32_t)(st->gpr[11].ud0 >> 32);
    uint32_t vf8z = (uint32_t)(st->gpr[11].ud1 & 0xFFFFFFFFu);
    CHECK(bits2f(vf8x) == (16.0f/4096.0f) && bits2f(vf8y) == 1.0f && bits2f(vf8z) == -8.0f,
          "VITOF12.xyzw VF8,VF5 = int/4096: (16/4096, 4096/4096=1, -32768/4096=-8)");

    uint32_t vf10x = (uint32_t)(st->gpr[13].ud0 & 0xFFFFFFFFu);
    uint32_t vf10y = (uint32_t)(st->gpr[13].ud0 >> 32);
    uint32_t vf10z = (uint32_t)(st->gpr[13].ud1 & 0xFFFFFFFFu);
    int32_t ivf10x, ivf10y, ivf10z;
    memcpy(&ivf10x, &vf10x, 4); memcpy(&ivf10y, &vf10y, 4); memcpy(&ivf10z, &vf10z, 4);
    CHECK(ivf10x == 2 && ivf10y == -2 && ivf10z == 100,
          "VFTOI0.xyzw VF10,VF9 = truncate(float): (2,-2,100)");

    uint32_t vf11w = (uint32_t)(st->gpr[14].ud1 >> 32);
    int32_t ivf11w; memcpy(&ivf11w, &vf11w, 4);
    CHECK(ivf11w == 1, "VFTOI4.xyzw VF11,VF9 = truncate(float*16): 0.0625*16=1.0 -> 1");

    uint32_t vf16x = (uint32_t)(st->gpr[15].ud0 & 0xFFFFFFFFu);
    uint32_t vf16y = (uint32_t)(st->gpr[15].ud0 >> 32);
    CHECK(bits2f(vf16x) == 2.5f && bits2f(vf16y) == 0.0f,
          "VABS.x VF16,VF1 only writes the X lane (destmask honored), Y stays 0");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
