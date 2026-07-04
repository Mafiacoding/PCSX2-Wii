/* Host-native unit test for ee_core.c opcode semantics (not part of
 * the Wii build - compiled with the host gcc for fast iteration).
 * Encodes tiny MIPS/EE programs by hand and checks register results. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

/* memalign isn't in standard host libc under this name in all cases;
 * ee_core.c calls memalign(align, size). glibc provides it via malloc.h. */
#include <malloc.h>

#include "core/ee/ee_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

/* --- MIPS/EE instruction encoders for the test program --- */
static uint32_t enc_lui(int rt, uint16_t imm)  { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_mmi(int rs, int rt, int rd, int sa, int funct) {
    return (0x1C << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}
static uint32_t enc_break(void) { return (0x00 << 26) | 0x0D; } /* SPECIAL/BREAK -> halts */

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

    uint8_t *prog = bios.data; /* reset vector maps to offset 0 of bios.data */
    int pc = 0;

    /* r1 = 0x00010002, r2 = 0x00030004 */
    write_le32(prog + pc, enc_lui(1, 0x0001)); pc += 4;
    write_le32(prog + pc, enc_ori(1, 1, 0x0002)); pc += 4;
    write_le32(prog + pc, enc_lui(2, 0x0003)); pc += 4;
    write_le32(prog + pc, enc_ori(2, 2, 0x0004)); pc += 4;
    /* r3 = PADDW(r1, r2)  [MMI funct=0x1C, MMI0 top funct=0x08, sub sa=0x00] */
    write_le32(prog + pc, enc_mmi(1, 2, 3, 0x00, 0x08)); pc += 4;
    /* r4 = PAND(r1, r2)   [MMI2 top funct=0x09, sub sa=0x12] */
    write_le32(prog + pc, enc_mmi(1, 2, 4, 0x12, 0x09)); pc += 4;
    /* r5 = POR(r1, r2)    [MMI3 top funct=0x29->0x09? no: 0x29 hex, wait funct field is 6 bits: 0x29 = 41 decimal] */
    write_le32(prog + pc, enc_mmi(1, 2, 5, 0x12, 0x29)); pc += 4;
    /* r6 = PEXTLW(r1, r2) [MMI0, sub sa=0x12] rd=6 */
    write_le32(prog + pc, enc_mmi(1, 2, 6, 0x12, 0x08)); pc += 4;
    write_le32(prog + pc, enc_break()); pc += 4;

    ee_core_init(&bios);
    ee_core_run(&bios);

    ee_state_t *st = ee_core_get_state();

    CHECK(st->gpr[1].ud0 == 0x00010002, "LUI+ORI built r1 = 0x00010002");
    CHECK(st->gpr[2].ud0 == 0x00030004, "LUI+ORI built r2 = 0x00030004");
    CHECK(st->gpr[3].ud0 == 0x00040006, "PADDW lane0: 0x10002+0x30004=0x40006");
    CHECK(st->gpr[3].ud1 == 0, "PADDW upper lanes (both zero inputs) = 0");
    CHECK(st->gpr[4].ud0 == (0x00010002ULL & 0x00030004ULL), "PAND matches bitwise AND");
    CHECK(st->gpr[5].ud0 == (0x00010002ULL | 0x00030004ULL), "POR matches bitwise OR");
    /* PEXTLW(rs=r1,rt=r2): out.lane0=rt.lane0, lane1=rs.lane0, lane2=rt.lane1(0), lane3=rs.lane1(0) */
    uint64_t expect_pextlw_lo = (uint64_t)0x00030004u | ((uint64_t)0x00010002u << 32);
    CHECK(st->gpr[6].ud0 == expect_pextlw_lo, "PEXTLW interleaves rt/rs into lanes 0/1");
    CHECK(st->gpr[6].ud1 == 0, "PEXTLW lanes 2/3 are zero (rt/rs lane1 both zero)");

    CHECK(st->halted == 1, "core halted (hit BREAK) instead of crashing/looping");
    CHECK(strstr(st->halt_reason, "BREAK") != NULL, "halt reason correctly reports BREAK");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
