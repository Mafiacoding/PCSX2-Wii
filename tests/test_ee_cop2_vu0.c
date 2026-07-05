/* test_ee_cop2_vu0.c - host-native test for round 13's VU0 vector
 * datapath additions: QMFC2/QMTC2 (128-bit GPR<->VF transfers),
 * VSUB.xyzw (3-operand vector subtract), VISWR (integer store word to
 * VU0 mem) and VSQI (store-quadword-increment to VU0 mem).
 *
 * These specific ops were found in a live PCSX2 disassembly of a real
 * BIOS VU0 init/self-test sequence that halted this project's EE
 * interpreter with "unimplemented COP2 sub-opcode" right after round
 * 12's control-register transfers (MFC2/CFC2/MTC2/CTC2) let boot
 * progress far enough to reach it. Field encodings (rs=destmask|0x10,
 * FT/FS/FD bit positions, the SPECIAL2 sub-index formula for VISWR/
 * VSQI) were derived by decoding the raw instruction words from that
 * disassembly and cross-checked against PCSX2's own
 * R5900OpcodeTables.cpp decode tables - see docs/STATUS.md's
 * "round 13" section.
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
static uint32_t enc_lq(int rt, int rs, int16_t imm)  { return (0x1E << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_break(void)                       { return 0x0D; }

/* COP2 32-bit transfer family: op=0x12, rs=sub-op, rt=GPR, rd=control
 * reg (VI0-15 for rd 0-15, special regs like FBRST=28 for rd 16-31). */
static uint32_t enc_cop2_xfer(int sub, int rt, int rd) { return (0x12u << 26) | ((uint32_t)sub << 21) | ((uint32_t)rt << 16) | ((uint32_t)rd << 11); }

/* COP2 128-bit transfer family: QMFC2 (rs=0x01), QMTC2 (rs=0x05). rt=GPR, rd=VF reg. */
static uint32_t enc_qmfc2(int rt, int vf) { return (0x12u << 26) | (0x01u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vf << 11); }
static uint32_t enc_qmtc2(int rt, int vf) { return (0x12u << 26) | (0x05u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vf << 11); }

/* CO-format vector op: rs = 0x10 | destmask, funct in low 6 bits. */
static uint32_t enc_co(uint32_t destmask, int ft, int fs, int fd, uint32_t funct)
{
    return (0x12u << 26) | ((0x10u | destmask) << 21) | ((uint32_t)ft << 16) | ((uint32_t)fs << 11) | ((uint32_t)fd << 6) | funct;
}
static uint32_t enc_vsub(uint32_t destmask, int fd, int fs, int ft) { return enc_co(destmask, ft, fs, fd, 0x2C); }
/* VISWR.<lane>: data=VI[ft("is")], addr=VI[fs("it")], destmask picks the
 * single lane. fd is NOT a free operand field here - the real VU macro
 * ISA reuses it as extra opcode-select bits for the memory-access/misc
 * "SPECIAL2" instruction family (which only needs 2 register operands,
 * not 3), so fd must be the exact fixed value (15) a real assembler
 * emits for VISWR - confirmed via a live PCSX2 disassembly's raw
 * instruction word (0x4b000bff) and cross-checked against
 * R5900OpcodeTables.cpp's SPECIAL2 index formula
 * (idx = fd*4 + (funct&0x3); VISWR sits at idx 63 = 15*4+3). */
static uint32_t enc_viswr(uint32_t destmask, int data_vi, int addr_vi) { return enc_co(destmask, data_vi, addr_vi, 15, 0x3F); }
/* VSQI: data=VF[fs], addr=VI[ft] (post-incremented). destmask=0xF (all
 * lanes) for the real mnemonic form. fd=13 is likewise the fixed
 * opcode-select value real VSQI encodes with (idx 53 = 13*4+1, funct
 * 0x3D whose bottom 2 bits are 1) - confirmed the same way. */
static uint32_t enc_vsqi(int data_vf, int addr_vi) { return enc_co(0xF, addr_vi, data_vf, 13, 0x3D); }

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }
static void wle64(uint8_t *p, uint64_t v) { for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

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
    /* r5 = LQ *(r4+0x20) - a known 128-bit pattern planted below */
    wle32(prog + (i++)*4, enc_lq(5, 4, 0x20));
    /* QMTC2 r5 -> VF1 (128-bit raw copy) */
    wle32(prog + (i++)*4, enc_qmtc2(5, 1));
    /* QMFC2 VF1 -> r6 (round-trip readback) */
    wle32(prog + (i++)*4, enc_qmfc2(6, 1));

    /* VSUB.xyzw VF2, VF1, VF1 -> VF2 = 0 in every lane regardless of VF1's value */
    wle32(prog + (i++)*4, enc_vsub(0xF, 2, 1, 1));
    /* QMFC2 VF2 -> r7 */
    wle32(prog + (i++)*4, enc_qmfc2(7, 2));

    /* QMFC2 VF0 -> r8: real hardware hardwires VF00 to (0,0,0,1.0f) */
    wle32(prog + (i++)*4, enc_qmfc2(8, 0));
    /* QMTC2 r5 -> VF0 (should be silently discarded - VF0 is read-only) */
    wle32(prog + (i++)*4, enc_qmtc2(5, 0));
    /* QMFC2 VF0 -> r9: must still read the same hardwired pattern as r8 */
    wle32(prog + (i++)*4, enc_qmfc2(9, 0));

    /* r1 = 3 (VU0 mem quadword address), r2 = 0xABCD1234 (data) */
    wle32(prog + (i++)*4, enc_lui(1, 0x0000));
    wle32(prog + (i++)*4, enc_ori(1, 1, 0x0003));
    wle32(prog + (i++)*4, enc_lui(2, 0xABCD));
    wle32(prog + (i++)*4, enc_ori(2, 2, 0x1234));
    /* MTC2 r1 -> VI10 (address reg), MTC2 r2 -> VI11 (data reg) */
    wle32(prog + (i++)*4, enc_cop2_xfer(0x04, 1, 10));
    wle32(prog + (i++)*4, enc_cop2_xfer(0x04, 2, 11));
    /* VISWR.x vi11,(vi10) - store VI11 into VU0 mem[VI10].x lane */
    wle32(prog + (i++)*4, enc_viswr(0x8 /* X */, 11, 10));
    /* VISWR.z vi11,(vi10) - same address, Z lane this time */
    wle32(prog + (i++)*4, enc_viswr(0x2 /* Z */, 11, 10));

    /* r3 = 7 (a second VU0 mem quadword address, for VSQI) */
    wle32(prog + (i++)*4, enc_lui(3, 0x0000));
    wle32(prog + (i++)*4, enc_ori(3, 3, 0x0007));
    /* MTC2 r3 -> VI12 (address reg for VSQI) */
    wle32(prog + (i++)*4, enc_cop2_xfer(0x04, 3, 12));
    /* VSQI VF1xyzw,(VI12++) - store all 4 lanes of VF1 (already loaded above) */
    wle32(prog + (i++)*4, enc_vsqi(1, 12));
    /* CFC2 VI12 -> r10 (prove post-increment happened: should read 8) */
    wle32(prog + (i++)*4, enc_cop2_xfer(0x02, 10, 12));

    wle32(prog + (i++)*4, enc_break());

    if (ee_core_init(&bios) != 0) { printf("ee_core_init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    /* Plant a known, non-trivial 128-bit pattern at RAM 0x1020 */
    wle64(st->ram + 0x1020, 0x1122334455667788ULL);
    wle64(st->ram + 0x1028, 0x99AABBCCDDEEFF00ULL);

    ee_core_run(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK (no unimplemented-opcode walls hit)");

    CHECK(st->gpr[5].ud0 == 0x1122334455667788ULL && st->gpr[5].ud1 == 0x99AABBCCDDEEFF00ULL,
          "LQ loaded the planted 128-bit pattern into r5");

    CHECK(st->gpr[6].ud0 == st->gpr[5].ud0 && st->gpr[6].ud1 == st->gpr[5].ud1,
          "QMTC2 r5->VF1 then QMFC2 VF1->r6 round-trips the full 128 bits exactly");

    CHECK(st->gpr[7].ud0 == 0ULL && st->gpr[7].ud1 == 0ULL,
          "VSUB.xyzw VF2,VF1,VF1 yields exactly zero in all 4 lanes (self-subtract)");

    /* VF00 hardwired to (0,0,0,1.0f): lane0(x)=ud0 low 32, lane1(y)=ud0 high 32,
     * lane2(z)=ud1 low 32, lane3(w)=ud1 high 32 = 0x3F800000 (1.0f). */
    uint64_t expect_vf0_ud0 = 0ULL;
    uint64_t expect_vf0_ud1 = ((uint64_t)0x3F800000u << 32);
    CHECK(st->gpr[8].ud0 == expect_vf0_ud0 && st->gpr[8].ud1 == expect_vf0_ud1,
          "QMFC2 VF0 reads the real-hardware-hardwired (0,0,0,1.0f) pattern");
    CHECK(st->gpr[9].ud0 == expect_vf0_ud0 && st->gpr[9].ud1 == expect_vf0_ud1,
          "QMTC2 into VF0 is silently discarded - VF0 still reads (0,0,0,1.0f) afterward");

    /* VU0 mem address 3 => byte offset 3*16=48. X lane = bytes 48-51, Z lane = bytes 56-59. */
    uint32_t viswr_x, viswr_z;
    memcpy(&viswr_x, st->vu0_mem + 48 + 0, 4);
    memcpy(&viswr_z, st->vu0_mem + 48 + 8, 4);
    CHECK(viswr_x == 0xABCD1234u, "VISWR.x stored VI11's value into VU0 mem[3].x");
    CHECK(viswr_z == 0xABCD1234u, "VISWR.z stored the same VI11 value into VU0 mem[3].z (different lane, same word)");

    /* VU0 mem address 7 => byte offset 7*16=112. All 4 lanes should hold VF1's
     * (== the LQ'd pattern's) x,y,z,w 32-bit words. */
    uint32_t sqi_lanes[4];
    for (int lane = 0; lane < 4; lane++)
        memcpy(&sqi_lanes[lane], st->vu0_mem + 112 + lane*4, 4);
    uint32_t expect_x = (uint32_t)(st->gpr[5].ud0 & 0xFFFFFFFFu);
    uint32_t expect_y = (uint32_t)(st->gpr[5].ud0 >> 32);
    uint32_t expect_z = (uint32_t)(st->gpr[5].ud1 & 0xFFFFFFFFu);
    uint32_t expect_w = (uint32_t)(st->gpr[5].ud1 >> 32);
    CHECK(sqi_lanes[0] == expect_x && sqi_lanes[1] == expect_y &&
          sqi_lanes[2] == expect_z && sqi_lanes[3] == expect_w,
          "VSQI stored all 4 VF1 lanes into VU0 mem[7] correctly");
    CHECK(st->gpr[10].ud0 == 8ULL, "VSQI post-incremented VI12 from 7 to 8");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
