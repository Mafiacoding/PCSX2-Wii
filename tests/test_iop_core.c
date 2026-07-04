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
static uint32_t enc_addiu(int rt, int rs, int16_t imm) { return (0x09 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_sw(int rt, int rs, int16_t imm) { return (0x2B << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_lwl(int rt, int rs, int16_t imm) { return (0x22 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_lwr(int rt, int rs, int16_t imm) { return (0x26 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
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
    /* r1 = 0x11223344 (RAM base addr holder), store into RAM at addr 0x100 via r2=0 base */
    wle32(p+pc, enc_lui(1, 0x1122)); pc+=4;
    wle32(p+pc, enc_ori(1, 1, 0x3344)); pc+=4;
    wle32(p+pc, enc_sw(1, 0, 0x100)); pc+=4;       /* MEM[0x100] = 0x11223344 (LE bytes: 44 33 22 11) */
    wle32(p+pc, enc_lwl(2, 0, 0x103)); pc+=4;       /* LWL r2, 0x103(r0): addr=0x103, unaligned */
    wle32(p+pc, enc_lwr(2, 0, 0x100)); pc+=4;       /* LWR r2, 0x100(r0) completes it -> r2 should == 0x11223344 */
    wle32(p+pc, enc_break()); pc+=4;

    iop_core_init(&bios);
    iop_core_run();
    iop_state_t *st = iop_core_get_state();

    CHECK(st->gpr[1] == 0x11223344u, "LUI+ORI built r1 = 0x11223344");
    uint32_t stored = iop_mem_read32(st, 0x100);
    CHECK(stored == 0x11223344u, "SW stored r1 correctly (little-endian roundtrip)");
    CHECK(st->gpr[2] == 0x11223344u, "LWL+LWR reconstruct unaligned word correctly");
    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL, "halted cleanly on BREAK");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
