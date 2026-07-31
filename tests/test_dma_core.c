#include <stdio.h>
#include <string.h>
#include "hw/dma.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    dma_init();
    uint32_t v;

    /* GIF channel MADR (0x1000A010) roundtrip */
    CHECK(dma_mmio_write32(0x1000A010u, 0xDEADBEEF) == 1, "write to GIF MADR handled");
    CHECK(dma_mmio_read32(0x1000A010u, &v) == 1 && v == 0xDEADBEEF, "GIF MADR roundtrip");

    /* SIF0 channel (0x1000C000, 0x400-byte block) CHCR roundtrip.
     * Deliberately NOT setting the STR bit (0x100) here - that now
     * triggers a real transfer via dma_channel_kick() (see
     * tests/test_dma_chain.c for that behavior), which would clear
     * STR again and break this simple register-latch check. */
    CHECK(dma_mmio_write32(0x1000C000u, 0x00000001) == 1, "write to SIF0 CHCR handled");
    CHECK(dma_mmio_read32(0x1000C000u, &v) == 1 && v == 0x00000001, "SIF0 CHCR roundtrip");

    /* toSPR channel (0x1000D400) QWC at +0x20 */
    CHECK(dma_mmio_write32(0x1000D420u, 42) == 1, "write to toSPR QWC handled");
    CHECK(dma_mmio_read32(0x1000D420u, &v) == 1 && v == 42, "toSPR QWC roundtrip");

    /* shared D_CTRL register */
    CHECK(dma_mmio_write32(0x1000E000u, 0x1) == 1, "write to D_CTRL handled");
    CHECK(dma_mmio_read32(0x1000E000u, &v) == 1 && v == 1, "D_CTRL roundtrip");

    /* address outside the DMAC range should not be claimed */
    CHECK(dma_mmio_read32(0x00100000u, &v) == 0, "address outside DMAC range correctly unclaimed");

    /* channels don't cross-contaminate */
    CHECK(dma_get_state()->chan[DMA_CHANNEL_VIF0].madr == 0, "untouched VIF0 channel stayed zero");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
