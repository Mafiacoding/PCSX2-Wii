/* test_ee_cop2_mtir.c - host-native test for Round 29 continued's
 * 23rd change: VMTIR (idx60), VMFIR (idx61), VILWR (idx62) - the
 * integer<->float raw-bit-move family and single-lane integer load,
 * ported from a real PCSX2 upstream reference clone's VUops.cpp
 * _vuMTIR/_vuMFIR/_vuILWR.
 *
 * VMTIR: VI[ft] = low 16 bits of the RAW 32-bit bit pattern of
 * VF[fs][Fsf] (a plain truncation, not a numeric conversion). Fsf is
 * NOT a separate field - confirmed via DisR5900asm.cpp's dest_fsf()
 * macro ("(disasmOpcode>>21)&3") that it lives in the exact same two
 * bits as this decoder's destmask value's low 2 bits.
 *
 * VMFIR: broadcasts the sign-extended 16-bit VI[fs] value (raw bits,
 * not a float conversion) into every destmask-selected lane of
 * VF[ft].
 *
 * VILWR: VI[ft] = the low 16 bits of VU0 mem at quadword index
 * VI[fs], single lane selected by destmask (same single-bit-only
 * convention as VISWR).
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
static uint32_t enc_mtc2(int rt, int vi)  { return (0x12u << 26) | (0x04u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vi << 11); }
static uint32_t enc_cfc2(int rt, int vi)  { return (0x12u << 26) | (0x02u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vi << 11); }
static uint32_t enc_qmfc2(int rt, int vf) { return (0x12u << 26) | (0x01u << 21) | ((uint32_t)rt << 16) | ((uint32_t)vf << 11); }

/* SPECIAL2 idx = (instr&0x3) | ((instr>>4)&0x7C); bits 2-5 forced to
 * 0xF for the outer funct dispatch. For VMTIR, destmask's low 2 bits
 * ARE the Fsf lane selector (not a bitmask) - callers pass the raw
 * 2-bit lane index 0-3 directly as "destmask". */
static uint32_t enc_special2(uint32_t destmask, int ft_field, int fs_field, uint32_t idx)
{
    uint32_t f6_10 = (idx >> 2) & 0x1Fu;
    uint32_t f0_1 = idx & 0x3u;
    return (0x12u << 26) | ((0x10u | destmask) << 21) | ((uint32_t)ft_field << 16)
         | ((uint32_t)fs_field << 11) | (f6_10 << 6) | (0xFu << 2) | f0_1;
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

    /* r4 = 0x80001000 (RAM base pointer, Round 363: KSEG0 direct-mapped, not raw KUSEG) */
    wle32(prog + (i++)*4, enc_lui(4, 0x8000));
    wle32(prog + (i++)*4, enc_ori(4, 4, 0x1000));

    /* LQ r5 <- RAM[0x1000] (VF1: bit patterns 0x12340001, 0x56780002, 0x9ABC0003, 0xDEF00004) */
    wle32(prog + (i++)*4, (0x1E << 26) | (4 << 21) | (5 << 16) | 0x0000);
    wle32(prog + (i++)*4, enc_qmtc2(5, 1));

    /* VMTIR VI10, VF1y (Fsf lane=1=Y): VI10 = low 16 bits of VF1.y's raw pattern (0x56780002 -> 0x0002) */
    wle32(prog + (i++)*4, enc_special2(1, 10, 1, 60));
    wle32(prog + (i++)*4, enc_cfc2(6, 10));

    /* r7 = 0xBEEF -> MTC2 into VI11 (source for VMFIR) */
    wle32(prog + (i++)*4, enc_lui(7, 0x0000));
    wle32(prog + (i++)*4, enc_ori(7, 7, 0xBEEF));
    wle32(prog + (i++)*4, enc_mtc2(7, 11));
    /* VMFIR.xz VF2,VI11 (destmask=0xA=X,Z lanes): broadcasts sign-extended 0xBEEF into VF2.x and VF2.z only */
    wle32(prog + (i++)*4, enc_special2(0xA, 2, 11, 61));
    wle32(prog + (i++)*4, enc_qmfc2(8, 2));

    /* r9 = 3 -> MTC2 into VI12 (address reg for VILWR, quadword index 3) */
    wle32(prog + (i++)*4, enc_ori(9, 0, 3));
    wle32(prog + (i++)*4, enc_mtc2(9, 12));
    /* VILWR.z VI13,(VI12) (destmask=0x2=Z lane only): VI13 = low 16 bits of VU0mem[quadword3].z */
    wle32(prog + (i++)*4, enc_special2(0x2, 13, 12, 62));
    wle32(prog + (i++)*4, enc_cfc2(14, 13));

    wle32(prog + (i++)*4, enc_break());

    if (ee_core_init(&bios) != 0) { printf("ee_core_init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    /* VF1 = raw bit patterns (not meaningful floats, chosen for easy bit inspection) */
    wle32(st->ram + 0x1000, 0x12340001u);
    wle32(st->ram + 0x1004, 0x56780002u);
    wle32(st->ram + 0x1008, 0x9ABC0003u);
    wle32(st->ram + 0x100C, 0xDEF00004u);

    /* Plant a known quadword at VU0 mem index 3: Z lane (byte offset 8-9) = 0x7777 */
    uint32_t z_off = vu0_mem_addr(3, 2); /* lane 2 = Z */
    st->vu0_mem[z_off] = 0x77;
    st->vu0_mem[z_off + 1] = 0x77;
    st->vu0_mem[z_off + 2] = 0x00;
    st->vu0_mem[z_off + 3] = 0x00;

    run_until_break(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK (VMTIR/VMFIR/VILWR recognized)");

    CHECK(st->gpr[6].ud0 == 0x0002ULL,
          "VMTIR VI10,VF1y truncates VF1.y's raw pattern (0x56780002) to its low 16 bits: 0x0002");

    uint32_t vf2x = (uint32_t)(st->gpr[8].ud0 & 0xFFFFFFFFu);
    uint32_t vf2y = (uint32_t)(st->gpr[8].ud0 >> 32);
    uint32_t vf2z = (uint32_t)(st->gpr[8].ud1 & 0xFFFFFFFFu);
    /* 0xBEEF as signed 16-bit is negative -> sign-extends to 0xFFFFBEEF */
    CHECK(vf2x == 0xFFFFBEEFu && vf2y == 0u && vf2z == 0xFFFFBEEFu,
          "VMFIR.xz VF2,VI11 broadcasts sign-extended 0xBEEF (-> 0xFFFFBEEF) into X and Z only, Y untouched (0)");

    CHECK(st->gpr[14].ud0 == 0x7777ULL,
          "VILWR.z VI13,(VI12) reads back the planted 0x7777 from VU0 mem quadword 3's Z lane, low 16 bits");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
