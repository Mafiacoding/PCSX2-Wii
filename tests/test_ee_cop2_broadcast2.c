/* test_ee_cop2_broadcast2.c - host-native test for Round 29
 * continued's 19th change: completing the funct 0x00-0x1F broadcast
 * row (Round 29 continued's 18th change covered VADDx/y/z/w,
 * VSUBx/y/z/w, VMAXx/y/z/w, VMINIx/y/z/w, VMULx/y/z/w) with the
 * remaining two op families:
 *
 *  - VMADDx/y/z/w (funct 0x08-0x0B) / VMSUBx/y/z/w (funct 0x0C-0x0F):
 *    the ACC-based broadcast forms - FD[lane] = ACC[lane] +-
 *    FS[lane]*FT.<bc-lane>, ported from PCSX2's VUops.cpp
 *    applyTernaryMACOpBroadcast.
 *
 *  - VMULq (funct 0x1C) / VMAXi (0x1D) / VMULi (0x1E) / VMINIi (0x1F):
 *    the Q/I-register broadcast forms - these have NO FT operand at
 *    all (confirmed against a real PCSX2 upstream reference clone's
 *    DisR5900asm.cpp P_VMULq/P_VMAXi/P_VMULi/P_VMINIi, which only
 *    print FD/FS, e.g. "vmulq.xyzw vf1,vf2,Q") - instead broadcasting
 *    the scalar Q (cop2_ctrl[22], PCSX2's VU.h REG_Q) or I
 *    (cop2_ctrl[21], REG_I) control register to every destmask lane.
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
static uint32_t enc_ctc2(int rt, int rd)  { return (0x12u << 26) | (0x06u << 21) | ((uint32_t)rt << 16) | ((uint32_t)rd << 11); }

static uint32_t enc_co(uint32_t destmask, int ft, int fs, int fd, uint32_t funct)
{
    return (0x12u << 26) | ((0x10u | destmask) << 21) | ((uint32_t)ft << 16) | ((uint32_t)fs << 11) | ((uint32_t)fd << 6) | funct;
}
/* VMADDx/y/z/w = funct 0x08+lane, VMSUBx/y/z/w = funct 0x0C+lane */
static uint32_t enc_vmaddbc(uint32_t destmask, int fd, int fs, int ft, uint32_t bc_lane) { return enc_co(destmask, ft, fs, fd, 0x08u | bc_lane); }
static uint32_t enc_vmsubbc(uint32_t destmask, int fd, int fs, int ft, uint32_t bc_lane) { return enc_co(destmask, ft, fs, fd, 0x0Cu | bc_lane); }
/* VMULq/VMAXi/VMULi/VMINIi - FT field unused, pass 0 */
static uint32_t enc_vmulq(uint32_t destmask, int fd, int fs)  { return enc_co(destmask, 0, fs, fd, 0x1C); }
static uint32_t enc_vmaxi(uint32_t destmask, int fd, int fs)  { return enc_co(destmask, 0, fs, fd, 0x1D); }
static uint32_t enc_vmuli(uint32_t destmask, int fd, int fs)  { return enc_co(destmask, 0, fs, fd, 0x1E); }
static uint32_t enc_vminii(uint32_t destmask, int fd, int fs) { return enc_co(destmask, 0, fs, fd, 0x1F); }

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

    /* LQ r5 <- RAM[0x1000] (VF1: 2,3,4,5), LQ r6 <- RAM[0x1010] (VF2: 10,20,30,40) */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (6 << 16) | 0x0010);
    wle32(prog + (i++)*4, enc_qmtc2(5, 1));
    wle32(prog + (i++)*4, enc_qmtc2(6, 2));

    /* r8 = bits of 100.0f -> CTC2 into VI22 (Q register) */
    wle32(prog + (i++)*4, enc_lui(8, 0x42C8));
    wle32(prog + (i++)*4, enc_ori(8, 8, 0x0000)); /* 0x42C80000 = 100.0f */
    wle32(prog + (i++)*4, enc_ctc2(8, 22));

    /* r9 = bits of 7.0f -> CTC2 into VI21 (I register) */
    wle32(prog + (i++)*4, enc_lui(9, 0x40E0));
    wle32(prog + (i++)*4, enc_ori(9, 9, 0x0000)); /* 0x40E00000 = 7.0f */
    wle32(prog + (i++)*4, enc_ctc2(9, 21));

    /* VMADDy.xyzw VF3,VF1,VF2 (ACC will be poked to (100,100,100,100)) -> FD[lane]=ACC+VF1[lane]*VF2.y(20) */
    wle32(prog + (i++)*4, enc_vmaddbc(0xF, 3, 1, 2, 1));
    wle32(prog + (i++)*4, enc_qmfc2(10, 3));

    /* VMSUBx.xyzw VF4,VF1,VF2 -> FD[lane]=ACC-VF1[lane]*VF2.x(10) */
    wle32(prog + (i++)*4, enc_vmsubbc(0xF, 4, 1, 2, 0));
    wle32(prog + (i++)*4, enc_qmfc2(11, 4));

    /* VMULq.xyzw VF5,VF1 -> FD[lane]=VF1[lane]*Q(100) */
    wle32(prog + (i++)*4, enc_vmulq(0xF, 5, 1));
    wle32(prog + (i++)*4, enc_qmfc2(12, 5));

    /* VMAXi.xyzw VF7,VF1 -> FD[lane]=max(VF1[lane], I(7)) */
    wle32(prog + (i++)*4, enc_vmaxi(0xF, 7, 1));
    wle32(prog + (i++)*4, enc_qmfc2(13, 7));

    /* VMULi.xyzw VF9,VF1 -> FD[lane]=VF1[lane]*I(7) */
    wle32(prog + (i++)*4, enc_vmuli(0xF, 9, 1));
    wle32(prog + (i++)*4, enc_qmfc2(14, 9));

    /* VMINIi.xyzw VF16,VF1 -> FD[lane]=min(VF1[lane], I(7)) */
    wle32(prog + (i++)*4, enc_vminii(0xF, 16, 1));
    wle32(prog + (i++)*4, enc_qmfc2(15, 16));

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

    /* Poke ACC = (100,100,100,100) - no macro-mode "write ACC" opcode
     * exists yet, same approach as test_ee_cop2_arith4.c. */
    st->vu0_acc[0] = f2bits(100.0f);
    st->vu0_acc[1] = f2bits(100.0f);
    st->vu0_acc[2] = f2bits(100.0f);
    st->vu0_acc[3] = f2bits(100.0f);

    ee_core_run(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK (VMADD/VMSUB-broadcast and VMULq/VMAXi/VMULi/VMINIi recognized)");

    uint32_t vf3x = (uint32_t)(st->gpr[10].ud0 & 0xFFFFFFFFu);
    uint32_t vf3y = (uint32_t)(st->gpr[10].ud0 >> 32);
    uint32_t vf3z = (uint32_t)(st->gpr[10].ud1 & 0xFFFFFFFFu);
    uint32_t vf3w = (uint32_t)(st->gpr[10].ud1 >> 32);
    CHECK(bits2f(vf3x) == 140.0f && bits2f(vf3y) == 160.0f && bits2f(vf3z) == 180.0f && bits2f(vf3w) == 200.0f,
          "VMADDy.xyzw VF3,VF1,VF2 = ACC(100)+VF1[lane]*VF2.y(20): (140,160,180,200)");

    uint32_t vf4x = (uint32_t)(st->gpr[11].ud0 & 0xFFFFFFFFu);
    uint32_t vf4y = (uint32_t)(st->gpr[11].ud0 >> 32);
    uint32_t vf4z = (uint32_t)(st->gpr[11].ud1 & 0xFFFFFFFFu);
    uint32_t vf4w = (uint32_t)(st->gpr[11].ud1 >> 32);
    CHECK(bits2f(vf4x) == 80.0f && bits2f(vf4y) == 70.0f && bits2f(vf4z) == 60.0f && bits2f(vf4w) == 50.0f,
          "VMSUBx.xyzw VF4,VF1,VF2 = ACC(100)-VF1[lane]*VF2.x(10): (80,70,60,50)");

    uint32_t vf5x = (uint32_t)(st->gpr[12].ud0 & 0xFFFFFFFFu);
    uint32_t vf5y = (uint32_t)(st->gpr[12].ud0 >> 32);
    uint32_t vf5z = (uint32_t)(st->gpr[12].ud1 & 0xFFFFFFFFu);
    uint32_t vf5w = (uint32_t)(st->gpr[12].ud1 >> 32);
    CHECK(bits2f(vf5x) == 200.0f && bits2f(vf5y) == 300.0f && bits2f(vf5z) == 400.0f && bits2f(vf5w) == 500.0f,
          "VMULq.xyzw VF5,VF1 = VF1[lane]*Q(100): (200,300,400,500)");

    uint32_t vf7x = (uint32_t)(st->gpr[13].ud0 & 0xFFFFFFFFu);
    uint32_t vf7y = (uint32_t)(st->gpr[13].ud0 >> 32);
    uint32_t vf7z = (uint32_t)(st->gpr[13].ud1 & 0xFFFFFFFFu);
    uint32_t vf7w = (uint32_t)(st->gpr[13].ud1 >> 32);
    CHECK(bits2f(vf7x) == 7.0f && bits2f(vf7y) == 7.0f && bits2f(vf7z) == 7.0f && bits2f(vf7w) == 7.0f,
          "VMAXi.xyzw VF7,VF1 = max(VF1[lane], I(7)): (7,7,7,7) since VF1's lanes are all <7 except none >7");

    uint32_t vf9x = (uint32_t)(st->gpr[14].ud0 & 0xFFFFFFFFu);
    uint32_t vf9y = (uint32_t)(st->gpr[14].ud0 >> 32);
    uint32_t vf9z = (uint32_t)(st->gpr[14].ud1 & 0xFFFFFFFFu);
    uint32_t vf9w = (uint32_t)(st->gpr[14].ud1 >> 32);
    CHECK(bits2f(vf9x) == 14.0f && bits2f(vf9y) == 21.0f && bits2f(vf9z) == 28.0f && bits2f(vf9w) == 35.0f,
          "VMULi.xyzw VF9,VF1 = VF1[lane]*I(7): (14,21,28,35)");

    uint32_t vf16x = (uint32_t)(st->gpr[15].ud0 & 0xFFFFFFFFu);
    uint32_t vf16y = (uint32_t)(st->gpr[15].ud0 >> 32);
    uint32_t vf16z = (uint32_t)(st->gpr[15].ud1 & 0xFFFFFFFFu);
    uint32_t vf16w = (uint32_t)(st->gpr[15].ud1 >> 32);
    CHECK(bits2f(vf16x) == 2.0f && bits2f(vf16y) == 3.0f && bits2f(vf16z) == 4.0f && bits2f(vf16w) == 5.0f,
          "VMINIi.xyzw VF16,VF1 = min(VF1[lane], I(7)): (2,3,4,5) since VF1's lanes are all <7");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
