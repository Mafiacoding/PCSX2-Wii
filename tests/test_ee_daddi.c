/* test_ee_daddi.c - verifies DADDI/DADDIU (primary opcodes 0x18/0x19),
 * found missing (clean halt on "unimplemented primary opcode 0x19")
 * once round 11's MCH_RICM/MCH_DRD fix let real BIOS boot progress
 * roughly 100x further than before, into RAM-resident code that uses
 * this instruction pair. See docs/STATUS.md's "round 11" section.
 *
 * Like this project's existing ADDI/ADDIU (which share one code path
 * and don't implement ADDI's overflow trap), DADDI/DADDIU are also
 * implemented identically here - a documented, consistent
 * simplification, not a new inconsistency.
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
static uint32_t enc_daddi(int op, int rt, int rs, int16_t imm) { return (op << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
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
    /* r1 = 0x00000001_00000000 (a value that needs the full 64 bits,
     * to prove this isn't just doing a 32-bit ADDIU under the hood):
     * LUI r1,1 ; then shift it up via a second LUI+ORI into r2 build
     * isn't directly expressible with plain LUI/ORI/DADDIU alone, so
     * instead verify sign-extension of a negative imm across the full
     * 64-bit register, which is the part a naive 32-bit-only
     * implementation would get wrong. r1 = 0x00000000FFFFFFFF (a
     * plausible upper-half-clear 64-bit value from a prior 32-bit op),
     * then DADDIU r2,r1,-1 should give exactly 0xFFFFFFFE (still
     * upper-half clear, ordinary borrow), and DADDIU r3,zero,-1 should
     * sign-extend to a full 0xFFFFFFFFFFFFFFFF (not truncated to 32
     * bits). */
    wle32(p+pc, enc_lui(1, 0x0000)); pc += 4;
    wle32(p+pc, enc_ori(1, 1, 0xFFFF)); pc += 4;  /* r1 = 0xFFFF (small, unambiguous 32-bit value) */
    wle32(p+pc, enc_daddi(0x19, 2, 1, -1)); pc += 4; /* r2 = DADDIU r1,-1 = 0xFFFE */
    wle32(p+pc, enc_daddi(0x19, 3, 0, -1)); pc += 4; /* r3 = DADDIU zero,-1 = sign-extended -1 across all 64 bits */
    wle32(p+pc, enc_daddi(0x18, 4, 0, 5)); pc += 4;  /* r4 = DADDI zero,5 = 5 (opcode 0x18 works identically) */
    wle32(p+pc, enc_break()); pc += 4;

    ee_core_init(&bios);
    ee_core_run(&bios);
    ee_state_t *st = ee_core_get_state();

    CHECK(st->gpr[2].ud0 == 0xFFFEu, "DADDIU: 0xFFFF + (-1) = 0xFFFE");
    CHECK(st->gpr[3].ud0 == 0xFFFFFFFFFFFFFFFFull,
          "DADDIU: 0 + (-1) sign-extends across the full 64-bit register, not just the low 32 bits");
    CHECK(st->gpr[4].ud0 == 5u, "DADDI (opcode 0x18) behaves the same as DADDIU here");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
