/* test_ee_ldl_ldr_sdl_sdr.c - host-native test for round 13's LDL/LDR/
 * SDL/SDR (64-bit unaligned load/store-left/right, primary opcodes
 * 0x1A/0x1B/0x2C/0x2D). Standard MIPS III instructions, the 8-byte-
 * doubleword analog of this project's existing LWL/LWR/SWL/SWR.
 * Found missing once round 13's VU0 fixes let real BIOS boot advance
 * past the VU0 init/self-test sequence into new code.
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
static uint32_t enc_ldl(int rt, int rs, int16_t imm) { return (0x1A << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_ldr(int rt, int rs, int16_t imm) { return (0x1B << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_sdl(int rt, int rs, int16_t imm) { return (0x2C << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_sdr(int rt, int rs, int16_t imm) { return (0x2D << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_break(void) { return 0x0D; }

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }
static void wle64(uint8_t *p, uint64_t v) { for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint64_t rle64(uint8_t *p) { uint64_t v=0; for (int i=0;i<8;i++) v |= ((uint64_t)p[i])<<(8*i); return v; }

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = calloc(1, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    uint8_t *prog = bios.data;
    int i = 0;

    /* r4 = 0x1000 (base pointer) */
    wle32(prog+(i++)*4, enc_lui(4, 0x0000));
    wle32(prog+(i++)*4, enc_ori(4, 4, 0x1000));

    /* Canonical MIPS unaligned-load idiom (matches real GCC-generated
     * code, and this project's existing LWL/LWR usage): the "Left"
     * variant targets offset+7 (the far end of the doubleword) FIRST,
     * the "Right" variant targets the base offset SECOND. */
    wle32(prog+(i++)*4, enc_ldl(5, 4, 0x27));
    wle32(prog+(i++)*4, enc_ldr(5, 4, 0x20));

    /* Same idiom again at a genuinely misaligned base (offset 0x23,
     * i.e. 3 bytes into the aligned block) to actually exercise the
     * cross-boundary byte-merge logic, not just the trivial shift=0/
     * shift=7 cases. */
    wle32(prog+(i++)*4, enc_ldl(6, 4, 0x10A));
    wle32(prog+(i++)*4, enc_ldr(6, 4, 0x103));

    /* SDL/SDR round-trip: store r5 (the known pattern) to a fresh
     * location via the same canonical idiom, then read it back with
     * LDL+LDR to verify. */
    wle32(prog+(i++)*4, enc_sdl(5, 4, 0x47));
    wle32(prog+(i++)*4, enc_sdr(5, 4, 0x40));
    wle32(prog+(i++)*4, enc_ldl(7, 4, 0x47));
    wle32(prog+(i++)*4, enc_ldr(7, 4, 0x40));

    wle32(prog+(i++)*4, enc_break());

    if (ee_core_init(&bios) != 0) { printf("ee_core_init failed\n"); return 1; }
    ee_state_t *st = ee_core_get_state();

    wle64(st->ram + 0x1020, 0x1122334455667788ULL);
    /* A second, independent 8-byte pattern planted at a genuinely
     * misaligned address (0x1103, i.e. 3 bytes into an 8-aligned
     * block) so the LDL(addr+7)+LDR(addr) idiom is forced to combine
     * bytes from two different aligned 8-byte blocks - actually
     * exercising the cross-boundary merge logic. */
    wle64(st->ram + 0x1103, 0xCAFEBABE0BADF00DULL); /* separate, non-overlapping region from the 0x1020 pattern above */
    /* Poison the SDL/SDR destination so we can prove the store actually happened. */
    wle64(st->ram + 0x1040, 0xDEADDEADDEADDEADULL);

    ee_core_run(&bios);

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK (no unimplemented-opcode walls hit)");

    CHECK(st->gpr[5].ud0 == 0x1122334455667788ULL,
          "canonical LDL(addr+7)+LDR(addr) idiom reconstructs the planted doubleword exactly");
    CHECK(st->gpr[6].ud0 == 0xCAFEBABE0BADF00DULL,
          "the same idiom at a genuinely misaligned base (0x1103, crossing an 8-byte block boundary) reconstructs the pattern correctly");

    CHECK(rle64(st->ram + 0x1040) == 0x1122334455667788ULL,
          "SDL+SDR (unaligned-store idiom) wrote the doubleword to RAM correctly");
    CHECK(st->gpr[7].ud0 == 0x1122334455667788ULL,
          "reading back the SDL/SDR-written doubleword via LDL+LDR round-trips exactly");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
