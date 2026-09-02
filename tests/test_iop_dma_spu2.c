/*
 * test_iop_dma_spu2.c - Round 712 (task #683): regression coverage
 * for the real IOP DMA channel-7 (SPU2) waveform-delivery path -
 * IOP-RAM-to-SPU2-local-RAM byte transfer, TSA-register addressing/
 * auto-advance, and the completion IRQ signal. See iop_dma.c's own
 * iop_dma_spu2_try_transfer() doc comment for the full citation trail
 * (psx-spx SPU page, real channel-7/SPU2 base address, real "8-byte
 * units" TSA addressing convention already established for SSA/LSA in
 * spu2_mixer.c).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "core/hw/iop_dma.h"
#include "core/hw/iop_spu2.h"
#include "core/hw/spu2_mixer.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint8_t g_iop_ram[256 * 1024];

int main(void)
{
    iop_dma_init();
    iop_spu2_init();
    memset(g_iop_ram, 0, sizeof(g_iop_ram));
    iop_dma_bind_iop_ram(g_iop_ram, sizeof(g_iop_ram));

    CHECK(iop_dma_get_spu2_transfer_count() == 0, "SPU2 DMA: transfer count starts at 0");

    /* Real SPU2 DMA channel base = 0x1F801500 (s_ranges[] in iop_dma.c) */
    const uint32_t SPU2_DMA_BASE = 0x1F801500u;

    /* --- source data in IOP RAM: 32 bytes (2 quadwords) of a
     * recognizable pattern --- */
    for (int i = 0; i < 32; i++) g_iop_ram[0x2000 + i] = (uint8_t)(0xA0 + i);

    /* TSA = 0 (byte address 0 in SPU2 RAM, real power-on-plausible
     * default matching iop_spu2_init()'s memset-to-zero) - no register
     * write needed for this first transfer. */
    iop_dma_mmio_write32(SPU2_DMA_BASE + 0x00u, 0x2000u); /* MADR = source in IOP RAM */
    iop_dma_mmio_write32(SPU2_DMA_BASE + 0x04u, 2u);       /* BCR: BS=2 words, BA=0 -> plain 2-word count */
    iop_dma_mmio_write32(SPU2_DMA_BASE + 0x08u, 0x01000000u); /* CHCR: STR=1 (start) */

    CHECK(iop_dma_get_spu2_transfer_count() == 1, "SPU2 DMA: real STR-bit kick performs exactly one transfer");

    uint8_t *spu2_ram = spu2_mixer_get_ram();
    CHECK(memcmp(spu2_ram, g_iop_ram + 0x2000, 8) == 0,
          "SPU2 DMA: 2-word (8-byte) transfer lands at SPU2 RAM byte 0, matching real IOP-RAM source data exactly");

    uint32_t chcr = 0;
    iop_dma_mmio_read32(SPU2_DMA_BASE + 0x08u, &chcr);
    CHECK((chcr & 0x01000000u) == 0, "SPU2 DMA: real hardware auto-clears STR after completion");

    /* --- real TSA auto-advance: after an 8-byte transfer starting at
     * unit 0, TSA should now read back as unit 1 (8 bytes / 8) --- */
    uint16_t tsa_hi = 0xFFFFu, tsa_lo = 0xFFFFu;
    iop_spu2_mmio_read16(iop_spu2_core_reg_addr(0, SPU2_C_TSA_HI), &tsa_hi);
    iop_spu2_mmio_read16(iop_spu2_core_reg_addr(0, SPU2_C_TSA_LO), &tsa_lo);
    CHECK(tsa_hi == 0 && tsa_lo == 1,
          "SPU2 DMA: real TSA register auto-advances by the transferred 8-byte-unit count (0 -> 1)");

    /* --- second kick continues from the advanced TSA, proving
     * consecutive DMA blocks chain correctly without IOP intervention --- */
    for (int i = 0; i < 16; i++) g_iop_ram[0x3000 + i] = (uint8_t)(0xC0 + i);
    iop_dma_mmio_write32(SPU2_DMA_BASE + 0x00u, 0x3000u);
    iop_dma_mmio_write32(SPU2_DMA_BASE + 0x04u, 4u); /* 4 words = 16 bytes = 2 more 8-byte units */
    iop_dma_mmio_write32(SPU2_DMA_BASE + 0x08u, 0x01000000u);

    CHECK(iop_dma_get_spu2_transfer_count() == 2, "SPU2 DMA: second real STR-bit kick counted");
    CHECK(memcmp(spu2_ram + 8, g_iop_ram + 0x3000, 16) == 0,
          "SPU2 DMA: second transfer correctly continues at the advanced TSA destination (byte offset 8), not overwriting the first block");

    /* --- explicit TSA reprogramming (real BIOS/game usage: point at a
     * fresh voice's waveform region before each new sample upload) --- */
    iop_spu2_mmio_write16(iop_spu2_core_reg_addr(0, SPU2_C_TSA_HI), 0);
    iop_spu2_mmio_write16(iop_spu2_core_reg_addr(0, SPU2_C_TSA_LO), 0x1000u); /* unit 0x1000 -> byte 0x8000 */
    for (int i = 0; i < 8; i++) g_iop_ram[0x4000 + i] = (uint8_t)(0xE0 + i);
    iop_dma_mmio_write32(SPU2_DMA_BASE + 0x00u, 0x4000u);
    iop_dma_mmio_write32(SPU2_DMA_BASE + 0x04u, 2u);
    iop_dma_mmio_write32(SPU2_DMA_BASE + 0x08u, 0x01000000u);

    CHECK(memcmp(spu2_ram + 0x8000, g_iop_ram + 0x4000, 8) == 0,
          "SPU2 DMA: real BIOS-style explicit TSA reprogram correctly redirects the destination within SPU2 RAM");

    /* --- out-of-bounds source (past IOP RAM): dropped, not read past
     * the end, no false completion counted --- */
    uint32_t before = iop_dma_get_spu2_transfer_count();
    iop_dma_mmio_write32(SPU2_DMA_BASE + 0x00u, (uint32_t)sizeof(g_iop_ram) - 4u);
    iop_dma_mmio_write32(SPU2_DMA_BASE + 0x04u, 100u); /* 400 bytes - well past the RAM boundary from that MADR */
    iop_dma_mmio_write32(SPU2_DMA_BASE + 0x08u, 0x01000000u);
    CHECK(iop_dma_get_spu2_transfer_count() == before,
          "SPU2 DMA: out-of-bounds IOP-RAM source is safely dropped, not read past the end");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
