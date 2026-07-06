/* test_iop_pc_guard.c - host-native test for round 14's IOP PC
 * fetch-sanity guard (source/core/iop/iop_core.c's iop_step()).
 *
 * Round 14 found a live BIOS boot path where the IOP executes a
 * genuine JALR $ra,$s1 with $s1 holding an address (0x03400008) that
 * doesn't correspond to any code this project's simplified IOP HLE
 * model ever loads (no real IOP module/IRX loader - see
 * iop_hle_modules.c's scope note). Before this round, fetching from
 * such an address silently read back 0 (a NOP) forever, letting
 * execution "wander" through effectively unmapped memory for tens of
 * millions of steps before coincidentally halting on an unrelated-
 * looking illegal opcode. This test verifies the new guard halts
 * immediately and cleanly instead, and that normal control flow
 * (JALR to a real, valid address) still works exactly as before.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include "core/iop/iop_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t enc_lui(int rt, uint16_t imm) { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_jalr(int rd, int rs) { return (0x00 << 26) | (rs << 21) | (rd << 11) | 0x09; }
static uint32_t enc_nop(void) { return 0x00000000; }
static uint32_t enc_break(void) { return 0x0D; }

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

int main(void) {
    /* --- Case 1: JALR to a WILD, unfetchable address must halt
     * immediately with the new diagnostic, not silently continue. --- */
    {
        bios_image_t bios;
        memset(&bios, 0, sizeof(bios));
        bios.data = memalign(32, BIOS_MAX_SIZE);
        memset(bios.data, 0, BIOS_MAX_SIZE);
        bios.size = BIOS_MAX_SIZE;
        bios.loaded = 1;

        uint8_t *p = bios.data;
        int pc = 0;
        /* $17 = 0x03400008 - outside both IOP RAM (2MB) and BIOS ROM range */
        wle32(p+pc, enc_lui(17, 0x0340)); pc += 4;
        wle32(p+pc, enc_ori(17, 17, 0x0008)); pc += 4;
        wle32(p+pc, enc_jalr(31, 17)); pc += 4;
        wle32(p+pc, enc_nop()); pc += 4; /* delay slot */
        wle32(p+pc, enc_break()); pc += 4; /* must NEVER be reached */

        iop_core_init(&bios);
        iop_state_t *st = iop_core_get_state();

        uint64_t steps = 0;
        while (!st->halted && steps < 1000000ULL) { iop_core_step(); steps++; }

        CHECK(st->halted == 1, "core halted (didn't run away into unmapped memory)");
        CHECK(strstr(st->halt_reason, "escaped") != NULL, "halt reason names the PC escape, not a generic/unrelated opcode error");
        CHECK(strstr(st->halt_reason, "0x03400008") != NULL, "halt reason names the actual offending address");
        CHECK((uint32_t)st->pc == 0x03400008u, "halted with pc exactly at the wild JALR target");
        CHECK(steps < 100, "halted within a handful of steps, not after wandering through unmapped memory");
    }

    /* --- Case 2: JALR to a REAL, valid address (back into BIOS ROM)
     * must still work exactly as before - the guard must not be
     * overly strict and break legitimate control flow. --- */
    {
        bios_image_t bios;
        memset(&bios, 0, sizeof(bios));
        bios.data = memalign(32, BIOS_MAX_SIZE);
        memset(bios.data, 0, BIOS_MAX_SIZE);
        bios.size = BIOS_MAX_SIZE;
        bios.loaded = 1;

        uint8_t *p = bios.data;
        /* $16 = 0xBFC00010 (a real BIOS ROM address, just past this
         * setup code) - JALR there, delay slot NOP, then land on a
         * BREAK planted at offset 0x10. */
        int pc = 0;
        wle32(p+pc, enc_lui(16, 0xBFC0)); pc += 4;
        wle32(p+pc, enc_ori(16, 16, 0x0010)); pc += 4;
        wle32(p+pc, enc_jalr(31, 16)); pc += 4;
        wle32(p+pc, enc_nop()); pc += 4; /* delay slot */
        /* offset 0x10: */
        wle32(p+0x10, enc_break());

        iop_core_init(&bios);
        iop_state_t *st = iop_core_get_state();
        iop_core_run();

        CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
              "JALR to a real, valid BIOS ROM address still works and reaches BREAK cleanly");
        CHECK(st->gpr[31] == 0xBFC00010u, "JALR correctly linked ra = pc+8 (jalr executes at 0xBFC00008, links to 0xBFC00010)");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
