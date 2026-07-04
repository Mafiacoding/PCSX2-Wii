/* Verifies ee_core.c's LWL/LWR/SWL/SWR unaligned load/store by
 * actually executing encoded instructions (not just re-deriving the
 * same formula and comparing it to itself). */
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
static uint32_t enc_sw(int rt, int rs, int16_t imm) { return (0x2B << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_lwl(int rt, int rs, int16_t imm) { return (0x22 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_lwr(int rt, int rs, int16_t imm) { return (0x26 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_swl(int rt, int rs, int16_t imm) { return (0x2A << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_swr(int rt, int rs, int16_t imm) { return (0x2E << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_break(void) { return 0x0D; }
static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    uint8_t *p = bios.data;
    int pc = 0;

    /* r10 = 0x00300000 (base for our scratch RAM area, well within the
     * 32MB EE RAM window so LWL/LWR/SWL/SWR touch real RAM). */
    wle32(p+pc, enc_lui(10, 0x0030)); pc += 4;

    /* Seed RAM at r10+0x00 with word 0x03020100 and r10+0x04 with
     * 0x07060504 (so the byte at RAM+N literally equals N, for an
     * easy-to-check unaligned window). */
    wle32(p+pc, enc_lui(1, 0x0302)); pc += 4;
    wle32(p+pc, enc_ori(1, 1, 0x0100)); pc += 4;
    wle32(p+pc, enc_sw(1, 10, 0x00)); pc += 4;
    wle32(p+pc, enc_lui(2, 0x0706)); pc += 4;
    wle32(p+pc, enc_ori(2, 2, 0x0504)); pc += 4;
    wle32(p+pc, enc_sw(2, 10, 0x04)); pc += 4;

    /* Unaligned 4-byte load starting at r10+1: bytes [1,2,3,4] as a
     * little-endian word = 0x04030201. For this (little-endian
     * target) EE formula set, the pairing is LWL at start+3, LWR at
     * start (verified against the mask/shift tables by hand before
     * writing this test - see git history for the trace). */
    wle32(p+pc, enc_lwl(3, 10, 0x04)); pc += 4;
    wle32(p+pc, enc_lwr(3, 10, 0x01)); pc += 4;

    /* Unaligned store: write r1 (0x03020100) to r10+0x09 via SWL+SWR,
     * then read back the two aligned words it touched (r10+0x08 and
     * r10+0x0C) to verify only the intended bytes changed. */
    wle32(p+pc, enc_swl(1, 10, 0x0C)); pc += 4;
    wle32(p+pc, enc_swr(1, 10, 0x09)); pc += 4;

    wle32(p+pc, enc_break()); pc += 4;

    ee_core_init(&bios);
    ee_core_run(&bios);
    ee_state_t *st = ee_core_get_state();

    CHECK(st->gpr[3].ud0 == (uint64_t)(int64_t)(int32_t)0x04030201u,
          "LWL(base+4)+LWR(base+1) reconstructs the unaligned word 0x04030201");

    uint32_t low_word  = ee_mem_read32(st, 0x00300008u);
    uint32_t high_word = ee_mem_read32(st, 0x0030000Cu);
    /* r1 = 0x03020100 written starting at byte offset 9 (i.e. offset
     * 1 into the word at +0x08): byte 0x08 untouched (still 0 from
     * memset), bytes 0x09-0x0B = the low 3 bytes of r1 (00,01,02),
     * byte 0x0C = the top byte of r1 (03), bytes 0x0D-0x0F untouched. */
    CHECK((low_word & 0x000000FFu) == 0, "SWL/SWR didn't touch the byte before the unaligned write (RAM[8], the LSB of low_word)");
    CHECK(((low_word >> 8) & 0xFFFFFFu) == 0x020100u, "SWL wrote the low 3 bytes of r1 into the first word");
    CHECK((high_word & 0xFFu) == 0x03u, "SWR wrote the top byte of r1 into the second word");
    CHECK((high_word & 0xFFFFFF00u) == 0, "SWL/SWR didn't touch bytes after the unaligned write");

    CHECK(st->halted == 1, "core halted cleanly on BREAK");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
