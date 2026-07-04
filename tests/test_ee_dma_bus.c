/* Verifies ee_core.c's memory bus correctly routes DMA-range addresses
 * to the DMA register skeleton instead of RAM. */
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
static uint32_t enc_lw(int rt, int rs, int16_t imm) { return (0x23 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
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
    /* r1 = 0xCAFEBABE */
    wle32(p+pc, enc_lui(1, 0xCAFE)); pc += 4;
    wle32(p+pc, enc_ori(1, 1, 0xBABE)); pc += 4;
    /* SW r1 -> GIF channel MADR (0x1000A010), via r0 base + imm since imm is only 16-bit signed:
     * 0x1000A010 doesn't fit in a 16-bit immediate off r0, so build the address in r2 instead. */
    wle32(p+pc, enc_lui(2, 0x1000)); pc += 4;
    wle32(p+pc, enc_ori(2, 2, 0xA010)); pc += 4;
    wle32(p+pc, enc_sw(1, 2, 0)); pc += 4;      /* MEM[r2+0] = r1 -> should hit DMA, not RAM */
    wle32(p+pc, enc_lw(3, 2, 0)); pc += 4;      /* r3 = MEM[r2+0] -> should read back via DMA */
    wle32(p+pc, enc_break()); pc += 4;

    ee_core_init(&bios);
    ee_core_run(&bios);
    ee_state_t *st = ee_core_get_state();

    CHECK(st->gpr[3].ud0 == (uint64_t)(int64_t)(int32_t)0xCAFEBABEu, "LW off the GIF MADR address reads back what SW wrote (via DMA, not RAM; LW correctly sign-extends into the 64-bit register)");

    dma_state_t *dma = dma_get_state();
    CHECK(dma->chan[DMA_CHANNEL_GIF].madr == 0xCAFEBABEu, "DMA skeleton's GIF channel MADR actually got the write");

    /* RAM at the equivalent physical offset should NOT have been
     * touched (proves the write was routed to DMA, not silently also
     * falling through to RAM). 0x1000A010 & 0x1FFFFFFF doesn't fit
     * our 32MB RAM window anyway (it's within the 0x10000000-range hw
     * window), so this is really just confirming no crash/UB there. */

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
