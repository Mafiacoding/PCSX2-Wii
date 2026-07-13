/* test_ee_cop2_ctrl.c - verifies COP2 (VU0 macro mode) control-
 * register transfer instructions (MFC2/CFC2/MTC2/CTC2), added round
 * 12 after real BIOS boot (unblocked by round 11's MCH_RICM/MCH_DRD
 * fix) reached a real init sequence doing a read-modify-write on
 * FBRST (control register 28) via cfc2/ori/ctc2 - halting cleanly on
 * "unimplemented primary opcode 0x12" since this project had zero
 * COP2 dispatch at all before this round.
 *
 * Scope: only the 32-bit control-register transfer family is
 * implemented (plain storage, no VU0/VU1 execution state to act on
 * real FBRST/Status/etc. side effects - see ee_core.h's cop2_ctrl[]
 * comment). The actual VU0 vector datapath (QMFC2/QMTC2 128-bit
 * moves, vector arithmetic like VISWR/VADD/VSUB/etc.) is confirmed
 * NOT implemented and out of scope - see docs/STATUS.md's "round 12"
 * section.
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

static uint32_t enc_lui(int rt, uint16_t imm) { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
/* COP2 transfer encoding: op=0x12, rs=sub-op (0=MFC2,2=CFC2,4=MTC2,6=CTC2), rt, rd=control reg, funct=0 */
static uint32_t enc_cop2_xfer(int sub, int rt, int rd) { return (0x12 << 26) | (sub << 21) | (rt << 16) | (rd << 11); }
static uint32_t enc_break(void) { return 0x0D; }
static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

int main(void)
{
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    uint8_t *p = bios.data;
    int pc = 0;
    /* r1 = 0x00000200 */
    wle32(p+pc, enc_lui(1, 0x0000)); pc += 4;
    wle32(p+pc, enc_ori(1, 1, 0x0200)); pc += 4;
    /* CTC2 r1 -> control reg 28 (FBRST) */
    wle32(p+pc, enc_cop2_xfer(0x06, 1, 28)); pc += 4;
    /* CFC2 control reg 28 -> r2 (should read back 0x200) */
    wle32(p+pc, enc_cop2_xfer(0x02, 2, 28)); pc += 4;
    /* MTC2 r1 -> control reg 5 (an arbitrary VI-range register) */
    wle32(p+pc, enc_cop2_xfer(0x04, 1, 5)); pc += 4;
    /* MFC2 control reg 5 -> r3 */
    wle32(p+pc, enc_cop2_xfer(0x00, 3, 5)); pc += 4;
    /* MFC2 control reg 0 (never written, should read 0) -> r4 */
    wle32(p+pc, enc_cop2_xfer(0x00, 4, 0)); pc += 4;
    wle32(p+pc, enc_break()); pc += 4;

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    /* Step exactly the 7 real instructions and stop before the
     * trailing BREAK. Task #178 made BREAK raise a genuine Breakpoint
     * exception instead of unconditionally halting; since Status.BEV
     * is untouched here (stays 1, the reset value), the exception
     * would vector into the same zero-filled bios.data buffer and spin
     * as harmless NOPs until ee_core_run()'s safety step cap - wasteful
     * even though it wouldn't have failed any of this test's own
     * (register-content-only) checks. Stepping explicitly avoids that
     * entirely. */
    ee_core_step(); /* LUI r1 */
    ee_core_step(); /* ORI r1 */
    ee_core_step(); /* CTC2 r1 -> FBRST */
    ee_core_step(); /* CFC2 FBRST -> r2 */
    ee_core_step(); /* MTC2 r1 -> ctrl reg 5 */
    ee_core_step(); /* MFC2 ctrl reg 5 -> r3 */
    ee_core_step(); /* MFC2 ctrl reg 0 -> r4 */

    CHECK(st->gpr[2].ud0 == (uint64_t)(int64_t)(int32_t)0x00000200u,
          "CFC2 reads back exactly what CTC2 wrote to FBRST (control reg 28)");
    CHECK(st->cop2_ctrl[28] == 0x00000200u, "cop2_ctrl[28] state actually holds the written value");
    CHECK(st->gpr[3].ud0 == (uint64_t)(int64_t)(int32_t)0x00000200u,
          "MFC2 reads back exactly what MTC2 wrote to an arbitrary control reg (5)");
    CHECK(st->gpr[4].ud0 == 0u, "MFC2 from a never-written control reg (0) reads back 0");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
