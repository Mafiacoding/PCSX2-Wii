/* Host-native unit test for ee_core.c's MFSA/MTSA (task #177).
 *
 * Real R5900-specific instructions (SPECIAL opcode, funct 0x28/0x29 -
 * NOT standard MIPS III, where that funct range is reserved; see
 * ps2tek's SPECIAL opcode table), added because a real EE
 * interrupt-handler prologue - reached for the first time ever once
 * task #176 implemented real Cause.IP3 delivery - uses MTSA/MFSA to
 * save/restore the CPU's dedicated 32-bit "SA" (Shift Amount) control
 * register as part of full context save, alongside HI/LO/HI1/LO1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <malloc.h>

#include "core/ee/ee_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t enc_lui(int rt, uint16_t imm)  { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_special(int rs, int rt, int rd, int sa, int funct) {
    return (0x00 << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}
static uint32_t enc_break(void) { return (0x00 << 26) | 0x0D; }

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

int main(void)
{
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    uint8_t *prog = bios.data;
    int pc = 0;

    /* r1 = 0x12345678 */
    write_le32(prog + pc, enc_lui(1, 0x1234)); pc += 4;
    write_le32(prog + pc, enc_ori(1, 1, 0x5678)); pc += 4;
    /* r5 = 0xDEADBEEF - a second, different value, so we can prove
     * MFSA reads back what MTSA wrote, not just some other register */
    write_le32(prog + pc, enc_lui(5, 0xDEAD)); pc += 4;
    write_le32(prog + pc, enc_ori(5, 5, 0xBEEF)); pc += 4;
    /* MTSA $1  (funct 0x29, rs=1) */
    write_le32(prog + pc, enc_special(1, 0, 0, 0, 0x29)); pc += 4;
    /* r2 = MFSA (funct 0x28, rd=2) - should read back r1's value */
    write_le32(prog + pc, enc_special(0, 0, 2, 0, 0x28)); pc += 4;
    /* MTSA $5  - overwrite SA with a different value */
    write_le32(prog + pc, enc_special(5, 0, 0, 0, 0x29)); pc += 4;
    /* r3 = MFSA - should now read back r5's value, proving MTSA
     * actually re-writes (not append/OR) */
    write_le32(prog + pc, enc_special(0, 0, 3, 0, 0x28)); pc += 4;
    /* MFSA with rd=$0 - must stay hardwired zero (real MIPS rule: any
     * write to $0 is discarded) */
    write_le32(prog + pc, enc_special(0, 0, 0, 0, 0x28)); pc += 4;
    write_le32(prog + pc, enc_break()); pc += 4;

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    /* Step exactly the 9 real instructions and stop before the
     * trailing BREAK. Task #178 made BREAK raise a genuine Breakpoint
     * exception instead of unconditionally halting; since Status.BEV
     * is untouched here (stays 1, the reset value), the exception
     * would vector into the same zero-filled bios.data buffer and spin
     * as harmless NOPs until ee_core_run()'s safety step cap - the
     * useful instructions are the ones actually under test. */
    ee_core_step(); /* LUI r1 */
    ee_core_step(); /* ORI r1 */
    ee_core_step(); /* LUI r5 */
    ee_core_step(); /* ORI r5 */
    ee_core_step(); /* MTSA $1 */
    ee_core_step(); /* MFSA -> r2 */
    ee_core_step(); /* MTSA $5 */
    ee_core_step(); /* MFSA -> r3 */
    ee_core_step(); /* MFSA -> $0 (discarded) */

    CHECK(st->gpr[1].ud0 == 0x12345678u, "LUI+ORI built r1 = 0x12345678");
    /* Real 64-bit MIPS LUI sign-extends its 32-bit result into the
     * full 64-bit register; 0xDEADBEEF has its top bit set, so the
     * register reads back as 0xFFFFFFFFDEADBEEF, not the bare 32-bit
     * pattern (r1's 0x12345678 above has its top bit clear, so this
     * distinction didn't matter for that check). */
    CHECK(st->gpr[5].ud0 == 0xFFFFFFFFDEADBEEFull, "LUI+ORI built r5 = sign-extended 0xDEADBEEF");
    CHECK(st->sa_reg == 0xDEADBEEFu, "SA register holds the last value MTSA wrote (0xDEADBEEF, not 0x12345678)");
    CHECK(st->gpr[2].ud0 == 0x12345678u, "first MFSA read back exactly what the first MTSA $1 wrote");
    CHECK(st->gpr[3].ud0 == 0xDEADBEEFu, "second MFSA read back exactly what the second MTSA $5 wrote (re-write, not OR/append)");
    CHECK(st->gpr[0].ud0 == 0, "MFSA with rd=$0 left $0 hardwired at zero");

    CHECK(st->halted == 0, "ran all 9 instructions without any spurious halt");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
