/* test_ee_cop2_arith4.c - host-native test for Round 29 continued's
 * 17th change: completing the VADD/VMUL/VMAX/VSUB/VMINI SPECIAL1 row
 * (funct 0x28-0x2F) with its three remaining, accumulator-based
 * siblings - VMADD (funct 0x29), VMSUB (funct 0x2D), and VOPMSUB
 * (funct 0x2E) - confirmed against a real PCSX2 upstream reference
 * clone's R5900OpcodeTables.cpp row (VADD, VMADD, VMUL, VMAX, VSUB,
 * VMSUB, VOPMSUB, VMINI = funct 0x28..0x2F sequential) and VUops.cpp's
 * _vuOpMADD/_vuOpMSUB/_vuOPMSUB semantics.
 *
 * VMADD/VMSUB read the VU0 macro-mode accumulator (st->vu0_acc[4],
 * poked directly here since there is no macro-mode "write ACC"
 * opcode implemented yet - VMADDA/VMSUBA/VMULA are a separate,
 * unimplemented family): FD[lane] = ACC[lane] +- FS[lane]*FT[lane]
 * per destmask lane, writing FD only (not ACC).
 *
 * VOPMSUB is the cross-product-shaped outer-product multiply-
 * subtract: FD.x=ACC.x-FS.y*FT.z, FD.y=ACC.y-FS.z*FT.x,
 * FD.z=ACC.z-FS.x*FT.y - always exactly xyz, no destmask, w
 * untouched.
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

static uint32_t enc_co(uint32_t destmask, int ft, int fs, int fd, uint32_t funct)
{
    return (0x12u << 26) | ((0x10u | destmask) << 21) | ((uint32_t)ft << 16) | ((uint32_t)fs << 11) | ((uint32_t)fd << 6) | funct;
}
static uint32_t enc_vmadd(uint32_t destmask, int fd, int fs, int ft)   { return enc_co(destmask, ft, fs, fd, 0x29); }
static uint32_t enc_vmsub(uint32_t destmask, int fd, int fs, int ft)   { return enc_co(destmask, ft, fs, fd, 0x2D); }
static uint32_t enc_vopmsub(int fd, int fs, int ft)                    { return enc_co(0xF, ft, fs, fd, 0x2E); }

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

    /* LQ r5 <- RAM[0x1000] (VF1: 2.0,3.0,4.0,5.0), LQ r6 <- RAM[0x1010] (VF2: 1.0,2.0,3.0,4.0) */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (6 << 16) | 0x0010);
    wle32(prog + (i++)*4, enc_qmtc2(5, 1));
    wle32(prog + (i++)*4, enc_qmtc2(6, 2));

    /* VMADD.xyzw VF3,VF1,VF2 (ACC will be poked to (10,10,10,10) before running) */
    wle32(prog + (i++)*4, enc_vmadd(0xF, 3, 1, 2));
    wle32(prog + (i++)*4, enc_qmfc2(7, 3));

    /* VMSUB.xyzw VF4,VF1,VF2 */
    wle32(prog + (i++)*4, enc_vmsub(0xF, 4, 1, 2));
    wle32(prog + (i++)*4, enc_qmfc2(8, 4));

    /* VMADD.x VF9,VF1,VF2 (single-lane destmask; VF9 starts at 0) */
    wle32(prog + (i++)*4, enc_vmadd(0x8, 9, 1, 2));
    wle32(prog + (i++)*4, enc_qmfc2(11, 9));

    /* VOPMSUB VF5,VF1,VF2 */
    wle32(prog + (i++)*4, enc_vopmsub(5, 1, 2));
    wle32(prog + (i++)*4, enc_qmfc2(9, 5));

    wle32(prog + (i++)*4, enc_break());

    if (ee_core_init(&bios) != 0) { printf("ee_core_init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    /* VF1 = (2.0, 3.0, 4.0, 5.0) */
    wle32(st->ram + 0x1000, f2bits(2.0f));
    wle32(st->ram + 0x1004, f2bits(3.0f));
    wle32(st->ram + 0x1008, f2bits(4.0f));
    wle32(st->ram + 0x100C, f2bits(5.0f));
    /* VF2 = (1.0, 2.0, 3.0, 4.0) */
    wle32(st->ram + 0x1010, f2bits(1.0f));
    wle32(st->ram + 0x1014, f2bits(2.0f));
    wle32(st->ram + 0x1018, f2bits(3.0f));
    wle32(st->ram + 0x101C, f2bits(4.0f));

    /* Poke ACC = (10,10,10,10) directly - no macro-mode "write ACC"
     * opcode is implemented yet, so this is the only way to get a
     * deterministic, non-zero accumulator for the test. */
    st->vu0_acc[0] = f2bits(10.0f);
    st->vu0_acc[1] = f2bits(10.0f);
    st->vu0_acc[2] = f2bits(10.0f);
    st->vu0_acc[3] = f2bits(10.0f);

    ee_core_run(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK (VMADD/VMSUB/VOPMSUB recognized)");

    /* VMADD(VF1,VF2) = ACC + VF1*VF2 = 10+2*1=12, 10+3*2=16, 10+4*3=22, 10+5*4=30 */
    uint32_t vf3x = (uint32_t)(st->gpr[7].ud0 & 0xFFFFFFFFu);
    uint32_t vf3y = (uint32_t)(st->gpr[7].ud0 >> 32);
    uint32_t vf3z = (uint32_t)(st->gpr[7].ud1 & 0xFFFFFFFFu);
    uint32_t vf3w = (uint32_t)(st->gpr[7].ud1 >> 32);
    CHECK(bits2f(vf3x) == 12.0f && bits2f(vf3y) == 16.0f && bits2f(vf3z) == 22.0f && bits2f(vf3w) == 30.0f,
          "VMADD.xyzw VF3,VF1,VF2 computes ACC+VF1*VF2 = (12,16,22,30)");

    /* VMSUB(VF1,VF2) = ACC - VF1*VF2 = 10-2=8, 10-6=4, 10-12=-2, 10-20=-10 */
    uint32_t vf4x = (uint32_t)(st->gpr[8].ud0 & 0xFFFFFFFFu);
    uint32_t vf4y = (uint32_t)(st->gpr[8].ud0 >> 32);
    uint32_t vf4z = (uint32_t)(st->gpr[8].ud1 & 0xFFFFFFFFu);
    uint32_t vf4w = (uint32_t)(st->gpr[8].ud1 >> 32);
    CHECK(bits2f(vf4x) == 8.0f && bits2f(vf4y) == 4.0f && bits2f(vf4z) == -2.0f && bits2f(vf4w) == -10.0f,
          "VMSUB.xyzw VF4,VF1,VF2 computes ACC-VF1*VF2 = (8,4,-2,-10)");

    /* VMADD.x only: x=12, y stays 0 (VF9 started at 0) */
    uint32_t vf9x = (uint32_t)(st->gpr[11].ud0 & 0xFFFFFFFFu);
    uint32_t vf9y = (uint32_t)(st->gpr[11].ud0 >> 32);
    CHECK(bits2f(vf9x) == 12.0f && bits2f(vf9y) == 0.0f,
          "VMADD.x VF9,VF1,VF2 only writes the X lane (destmask honored), Y stays 0");

    /* VOPMSUB(VF1,VF2) = ACC.x - VF1.y*VF2.z = 10 - 3*3 = 1
     *                    ACC.y - VF1.z*VF2.x = 10 - 4*1 = 6
     *                    ACC.z - VF1.x*VF2.y = 10 - 2*2 = 6 */
    uint32_t vf5x = (uint32_t)(st->gpr[9].ud0 & 0xFFFFFFFFu);
    uint32_t vf5y = (uint32_t)(st->gpr[9].ud0 >> 32);
    uint32_t vf5z = (uint32_t)(st->gpr[9].ud1 & 0xFFFFFFFFu);
    CHECK(bits2f(vf5x) == 1.0f && bits2f(vf5y) == 6.0f && bits2f(vf5z) == 6.0f,
          "VOPMSUB VF5,VF1,VF2 computes the cross-product-shaped outer-product multiply-subtract (1,6,6)");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
