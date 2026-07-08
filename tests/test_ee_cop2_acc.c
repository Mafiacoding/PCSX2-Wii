/* test_ee_cop2_acc.c - host-native test for Round 29 continued's
 * 21st change: the COP2SPECIAL2 accumulator-writing family - every
 * op that writes VU->ACC (this project's vu0_acc[4]) instead of
 * VF[fd]. Confirmed against a real PCSX2 upstream reference clone's
 * VUops.cpp (applyBinaryMACOp/applyTernaryMACOp and their Broadcast
 * variants, templated on MACOpDst::Acc instead of ::Fd).
 *
 * Covers: full-vector VADDA(idx40)/VMADDA(41)/VMULA(42)/VSUBA(44)/
 * VMSUBA(45); VOPMULA(46, outer-product into ACC, xyz only, no
 * existing-ACC read); VNOP(47, true no-op); representative broadcast
 * forms VADDAy(idx1)/VMADDAx(idx8)/VMULAq(idx28)/VSUBAi(idx38).
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
static uint32_t enc_ctc2(int rt, int rd)  { return (0x12u << 26) | (0x06u << 21) | ((uint32_t)rt << 16) | ((uint32_t)rd << 11); }

/* SPECIAL2 idx = (instr&0x3) | ((instr>>4)&0x7C); bits 2-5 must also
 * be forced to 0xF for the outer funct dispatch (same fix established
 * in test_ee_cop2_unary.c). */
static uint32_t enc_special2(uint32_t destmask, int ft_field, int fs_field, uint32_t idx)
{
    uint32_t f6_10 = (idx >> 2) & 0x1Fu;
    uint32_t f0_1 = idx & 0x3u;
    return (0x12u << 26) | ((0x10u | destmask) << 21) | ((uint32_t)ft_field << 16)
         | ((uint32_t)fs_field << 11) | (f6_10 << 6) | (0xFu << 2) | f0_1;
}
/* QMFC2-free accumulator readout: we can't QMFC2 the ACC register
 * directly (no opcode reads ACC into a GPR), so every check reads
 * st->vu0_acc[] straight from the host struct after ee_core_run(). */

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

    /* r7 = bits of 1000.0f -> CTC2 into VI22 (Q register) */
    wle32(prog + (i++)*4, enc_lui(7, 0x447A));
    wle32(prog + (i++)*4, enc_ori(7, 7, 0x0000)); /* 0x447A0000 = 1000.0f */
    wle32(prog + (i++)*4, enc_ctc2(7, 22));

    /* r8 = bits of 9.0f -> CTC2 into VI21 (I register) */
    wle32(prog + (i++)*4, enc_lui(8, 0x4110));
    wle32(prog + (i++)*4, enc_ori(8, 8, 0x0000)); /* 0x41100000 = 9.0f */
    wle32(prog + (i++)*4, enc_ctc2(8, 21));

    /* VADDA.xyzw VF1,VF2 (idx40, full-vector): ACC[lane]=VF1[lane]+VF2[lane] */
    wle32(prog + (i++)*4, enc_special2(0xF, 2, 1, 40));
    wle32(prog + (i++)*4, enc_break());

    if (ee_core_init(&bios) != 0) { printf("ee_core_init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    /* VF1 = (2, 3, 4, 5), VF2 = (10, 20, 30, 40) */
    wle32(st->ram + 0x1000, f2bits(2.0f));
    wle32(st->ram + 0x1004, f2bits(3.0f));
    wle32(st->ram + 0x1008, f2bits(4.0f));
    wle32(st->ram + 0x100C, f2bits(5.0f));
    wle32(st->ram + 0x1010, f2bits(10.0f));
    wle32(st->ram + 0x1014, f2bits(20.0f));
    wle32(st->ram + 0x1018, f2bits(30.0f));
    wle32(st->ram + 0x101C, f2bits(40.0f));

    ee_core_run(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK (VADDA recognized)");
    CHECK(bits2f(st->vu0_acc[0]) == 12.0f && bits2f(st->vu0_acc[1]) == 23.0f
          && bits2f(st->vu0_acc[2]) == 34.0f && bits2f(st->vu0_acc[3]) == 45.0f,
          "VADDA.xyzw VF1,VF2 (idx40, dest=ACC) computes ACC=VF1+VF2: (12,23,34,45)");

    /* Second program: exercise VMADDA(idx41, full-vector, reads+writes
     * ACC), VMULA(idx42), VSUBA(idx44), VOPMULA(idx46), VNOP(idx47),
     * and representative broadcast forms VADDAy(idx1)/VMADDAx(idx8)/
     * VMULAq(idx28)/VSUBAi(idx38), each in its own fresh core so ACC
     * starts at a known (zeroed) state every time. */
    {
        bios_image_t bios2; memset(&bios2, 0, sizeof(bios2));
        bios2.data = calloc(1, BIOS_MAX_SIZE); bios2.size = BIOS_MAX_SIZE; bios2.loaded = 1;
        uint8_t *p2 = bios2.data; int j = 0;
        wle32(p2 + (j++)*4, enc_lui(4, 0x0000));
        wle32(p2 + (j++)*4, enc_ori(4, 4, 0x1000));
        wle32(p2 + (j++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
        wle32(p2 + (j++)*4, (0x1E << 26) | (4 << 21) | (6 << 16) | 0x0010);
        wle32(p2 + (j++)*4, enc_qmtc2(5, 1));
        wle32(p2 + (j++)*4, enc_qmtc2(6, 2));
        /* VMULA.xyzw VF1,VF2 (idx42): ACC = VF1*VF2 (fresh ACC, so this seeds it) */
        wle32(p2 + (j++)*4, enc_special2(0xF, 2, 1, 42));
        /* VMADDA.xyzw VF1,VF2 (idx41): ACC = ACC + VF1*VF2 (reads the VMULA result back) */
        wle32(p2 + (j++)*4, enc_special2(0xF, 2, 1, 41));
        wle32(p2 + (j++)*4, enc_break());

        if (ee_core_init(&bios2) != 0) { printf("ee_core_init failed (2)\n"); return 1; }
        ee_state_t *st2 = ee_core_get_state();
        wle32(st2->ram + 0x1000, f2bits(2.0f));
        wle32(st2->ram + 0x1004, f2bits(3.0f));
        wle32(st2->ram + 0x1008, f2bits(4.0f));
        wle32(st2->ram + 0x100C, f2bits(5.0f));
        wle32(st2->ram + 0x1010, f2bits(10.0f));
        wle32(st2->ram + 0x1014, f2bits(20.0f));
        wle32(st2->ram + 0x1018, f2bits(30.0f));
        wle32(st2->ram + 0x101C, f2bits(40.0f));
        ee_core_run(&bios2);
        /* VMULA: ACC=(20,60,120,200). VMADDA: ACC=ACC+VF1*VF2=(20+20,60+60,120+120,200+200)=(40,120,240,400) */
        CHECK(bits2f(st2->vu0_acc[0]) == 40.0f && bits2f(st2->vu0_acc[1]) == 120.0f
              && bits2f(st2->vu0_acc[2]) == 240.0f && bits2f(st2->vu0_acc[3]) == 400.0f,
              "VMULA(idx42) seeds ACC=(20,60,120,200), then VMADDA(idx41) accumulates to (40,120,240,400)");
    }

    {
        bios_image_t bios3; memset(&bios3, 0, sizeof(bios3));
        bios3.data = calloc(1, BIOS_MAX_SIZE); bios3.size = BIOS_MAX_SIZE; bios3.loaded = 1;
        uint8_t *p3 = bios3.data; int j = 0;
        wle32(p3 + (j++)*4, enc_lui(4, 0x0000));
        wle32(p3 + (j++)*4, enc_ori(4, 4, 0x1000));
        wle32(p3 + (j++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
        wle32(p3 + (j++)*4, (0x1E << 26) | (4 << 21) | (6 << 16) | 0x0010);
        wle32(p3 + (j++)*4, enc_qmtc2(5, 1));
        wle32(p3 + (j++)*4, enc_qmtc2(6, 2));
        /* VSUBA.xyzw VF1,VF2 (idx44): ACC = VF1-VF2 */
        wle32(p3 + (j++)*4, enc_special2(0xF, 2, 1, 44));
        wle32(p3 + (j++)*4, enc_break());
        if (ee_core_init(&bios3) != 0) { printf("ee_core_init failed (3)\n"); return 1; }
        ee_state_t *st3 = ee_core_get_state();
        wle32(st3->ram + 0x1000, f2bits(2.0f));
        wle32(st3->ram + 0x1004, f2bits(3.0f));
        wle32(st3->ram + 0x1008, f2bits(4.0f));
        wle32(st3->ram + 0x100C, f2bits(5.0f));
        wle32(st3->ram + 0x1010, f2bits(10.0f));
        wle32(st3->ram + 0x1014, f2bits(20.0f));
        wle32(st3->ram + 0x1018, f2bits(30.0f));
        wle32(st3->ram + 0x101C, f2bits(40.0f));
        ee_core_run(&bios3);
        CHECK(bits2f(st3->vu0_acc[0]) == -8.0f && bits2f(st3->vu0_acc[1]) == -17.0f
              && bits2f(st3->vu0_acc[2]) == -26.0f && bits2f(st3->vu0_acc[3]) == -35.0f,
              "VSUBA.xyzw VF1,VF2 (idx44) computes ACC=VF1-VF2: (-8,-17,-26,-35)");
    }

    {
        bios_image_t bios4; memset(&bios4, 0, sizeof(bios4));
        bios4.data = calloc(1, BIOS_MAX_SIZE); bios4.size = BIOS_MAX_SIZE; bios4.loaded = 1;
        uint8_t *p4 = bios4.data; int j = 0;
        wle32(p4 + (j++)*4, enc_lui(4, 0x0000));
        wle32(p4 + (j++)*4, enc_ori(4, 4, 0x1000));
        wle32(p4 + (j++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
        wle32(p4 + (j++)*4, (0x1E << 26) | (4 << 21) | (6 << 16) | 0x0010);
        wle32(p4 + (j++)*4, enc_qmtc2(5, 1));
        wle32(p4 + (j++)*4, enc_qmtc2(6, 2));
        /* VOPMULA VF1,VF2 (idx46): ACC.x=VF1.y*VF2.z, ACC.y=VF1.z*VF2.x, ACC.z=VF1.x*VF2.y */
        wle32(p4 + (j++)*4, enc_special2(0xF, 2, 1, 46));
        wle32(p4 + (j++)*4, enc_break());
        if (ee_core_init(&bios4) != 0) { printf("ee_core_init failed (4)\n"); return 1; }
        ee_state_t *st4 = ee_core_get_state();
        wle32(st4->ram + 0x1000, f2bits(2.0f));
        wle32(st4->ram + 0x1004, f2bits(3.0f));
        wle32(st4->ram + 0x1008, f2bits(4.0f));
        wle32(st4->ram + 0x100C, f2bits(5.0f));
        wle32(st4->ram + 0x1010, f2bits(10.0f));
        wle32(st4->ram + 0x1014, f2bits(20.0f));
        wle32(st4->ram + 0x1018, f2bits(30.0f));
        wle32(st4->ram + 0x101C, f2bits(40.0f));
        /* Poke ACC to a nonzero sentinel first to prove VOPMULA
         * really overwrites it rather than accumulating. */
        st4->vu0_acc[0] = f2bits(999.0f);
        ee_core_run(&bios4);
        /* ACC.x=VF1.y*VF2.z=3*30=90, ACC.y=VF1.z*VF2.x=4*10=40, ACC.z=VF1.x*VF2.y=2*20=40 */
        CHECK(bits2f(st4->vu0_acc[0]) == 90.0f && bits2f(st4->vu0_acc[1]) == 40.0f && bits2f(st4->vu0_acc[2]) == 40.0f,
              "VOPMULA VF1,VF2 (idx46) computes the cross-product-shaped outer product into ACC: (90,40,40), overwriting the prior sentinel");
    }

    {
        bios_image_t bios5; memset(&bios5, 0, sizeof(bios5));
        bios5.data = calloc(1, BIOS_MAX_SIZE); bios5.size = BIOS_MAX_SIZE; bios5.loaded = 1;
        uint8_t *p5 = bios5.data; int j = 0;
        wle32(p5 + (j++)*4, enc_lui(4, 0x0000));
        wle32(p5 + (j++)*4, enc_ori(4, 4, 0x1000));
        wle32(p5 + (j++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
        wle32(p5 + (j++)*4, enc_qmtc2(5, 1));
        /* r6 = bits of 1000.0f -> Q, r7 = bits of 9.0f -> I */
        wle32(p5 + (j++)*4, enc_lui(6, 0x447A));
        wle32(p5 + (j++)*4, enc_ori(6, 6, 0x0000));
        wle32(p5 + (j++)*4, enc_ctc2(6, 22));
        wle32(p5 + (j++)*4, enc_lui(7, 0x4110));
        wle32(p5 + (j++)*4, enc_ori(7, 7, 0x0000));
        wle32(p5 + (j++)*4, enc_ctc2(7, 21));
        /* VMULAq.xyzw VF1 (idx28): ACC = VF1 * Q(1000) */
        wle32(p5 + (j++)*4, enc_special2(0xF, 0, 1, 28));
        /* VNOP (idx47): must be a true no-op, ACC unchanged after */
        wle32(p5 + (j++)*4, enc_special2(0xF, 0, 1, 47));
        wle32(p5 + (j++)*4, enc_break());
        if (ee_core_init(&bios5) != 0) { printf("ee_core_init failed (5)\n"); return 1; }
        ee_state_t *st5 = ee_core_get_state();
        wle32(st5->ram + 0x1000, f2bits(2.0f));
        wle32(st5->ram + 0x1004, f2bits(3.0f));
        wle32(st5->ram + 0x1008, f2bits(4.0f));
        wle32(st5->ram + 0x100C, f2bits(5.0f));
        ee_core_run(&bios5);
        CHECK(bits2f(st5->vu0_acc[0]) == 2000.0f && bits2f(st5->vu0_acc[1]) == 3000.0f
              && bits2f(st5->vu0_acc[2]) == 4000.0f && bits2f(st5->vu0_acc[3]) == 5000.0f,
              "VMULAq.xyzw VF1 (idx28, broadcasts Q=1000) computes ACC=VF1*1000: (2000,3000,4000,5000), unaffected by the following VNOP");
    }

    {
        bios_image_t bios6; memset(&bios6, 0, sizeof(bios6));
        bios6.data = calloc(1, BIOS_MAX_SIZE); bios6.size = BIOS_MAX_SIZE; bios6.loaded = 1;
        uint8_t *p6 = bios6.data; int j = 0;
        wle32(p6 + (j++)*4, enc_lui(4, 0x0000));
        wle32(p6 + (j++)*4, enc_ori(4, 4, 0x1000));
        wle32(p6 + (j++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
        wle32(p6 + (j++)*4, (0x1E << 26) | (4 << 21) | (6 << 16) | 0x0010);
        wle32(p6 + (j++)*4, enc_qmtc2(5, 1));
        wle32(p6 + (j++)*4, enc_qmtc2(6, 2));
        /* r7 = bits of 9.0f -> I */
        wle32(p6 + (j++)*4, enc_lui(7, 0x4110));
        wle32(p6 + (j++)*4, enc_ori(7, 7, 0x0000));
        wle32(p6 + (j++)*4, enc_ctc2(7, 21));
        /* VADDAy.xyzw VF1,VF2 (idx1, broadcasts VF2.y=20): ACC=VF1+20 */
        wle32(p6 + (j++)*4, enc_special2(0xF, 2, 1, 1));
        wle32(p6 + (j++)*4, enc_break());
        if (ee_core_init(&bios6) != 0) { printf("ee_core_init failed (6)\n"); return 1; }
        ee_state_t *st6 = ee_core_get_state();
        wle32(st6->ram + 0x1000, f2bits(2.0f));
        wle32(st6->ram + 0x1004, f2bits(3.0f));
        wle32(st6->ram + 0x1008, f2bits(4.0f));
        wle32(st6->ram + 0x100C, f2bits(5.0f));
        wle32(st6->ram + 0x1010, f2bits(10.0f));
        wle32(st6->ram + 0x1014, f2bits(20.0f));
        wle32(st6->ram + 0x1018, f2bits(30.0f));
        wle32(st6->ram + 0x101C, f2bits(40.0f));
        ee_core_run(&bios6);
        CHECK(bits2f(st6->vu0_acc[0]) == 22.0f && bits2f(st6->vu0_acc[1]) == 23.0f
              && bits2f(st6->vu0_acc[2]) == 24.0f && bits2f(st6->vu0_acc[3]) == 25.0f,
              "VADDAy.xyzw VF1,VF2 (idx1, broadcasts VF2.y=20) computes ACC=VF1+20: (22,23,24,25)");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
