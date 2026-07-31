/*
 * test_dma_sif2.c - Round 177 (task #343): regression coverage for
 * SIF2 (EE DMA channel 7, physical base 0x1000C800 per dma.c's
 * s_ranges[] table). Before this test, SIF2 had a channel constant
 * and an address-range table entry but ZERO test coverage - nothing
 * had ever verified that the generic dma_channel_kick() transfer
 * engine (NORMAL/CHAIN mode, tag walking, completion signaling)
 * actually works correctly when addressed via SIF2's specific base
 * address rather than one of the other 9 channels.
 *
 * This matters because task #172's 0x8000F768 EE wait loop (see
 * STATUS.md 96th/114th/115th/132nd/216th findings) polls
 * "DMAC_STAT bit 0x80 (1<<7, i.e. SIF2's completion bit) OR
 * INTC_STAT bit 0x2 (SBUS)". This test proves the first half of
 * that OR-condition is real, already-implemented, working hardware
 * behavior in this project - not a gap - confirming that IF the EE's
 * own code ever issues a real SIF2 CHCR/MADR/QWC/STR sequence
 * (which nothing in the current boot trajectory does - that is a
 * separate, still-open question, see STATUS.md 216th finding), the
 * completion signal would correctly propagate to DMAC_STAT exactly
 * like every other channel, via the SAME generic engine GIF/VIF0/
 * VIF1/SIF0/SIF1/toSPR already use. No SIF2-specific code was added
 * because none was needed - the existing generic engine is already
 * channel-agnostic (see dma_channel_kick()'s doc comment).
 */
#include <stdio.h>
#include <string.h>
#include "hw/dma.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint8_t g_ram[1024 * 1024];

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

static uint8_t g_sink_capture[4096];
static uint32_t g_sink_capture_len = 0;
static int g_sink_calls = 0;
static int g_sink_last_channel = -1;

static void test_sink(int channel, const uint8_t *data, uint32_t qwc)
{
    g_sink_last_channel = channel;
    uint32_t bytes = qwc * 16u;
    if (g_sink_capture_len + bytes <= sizeof(g_sink_capture)) {
        memcpy(g_sink_capture + g_sink_capture_len, data, bytes);
        g_sink_capture_len += bytes;
    }
    g_sink_calls++;
}

int main(void) {
    dma_init();
    memset(g_ram, 0, sizeof(g_ram));
    dma_bind_ee_ram(g_ram, sizeof(g_ram));
    dma_set_sink(DMA_CHANNEL_SIF2, test_sink);

    /* SIF2 base address confirmed against dma.c's s_ranges[] table:
     * { 0x1000C800u, 0x0400u, DMA_CHANNEL_SIF2 } */
    CHECK(DMA_CHANNEL_SIF2 == 7, "DMA_CHANNEL_SIF2 constant is 7 (matches DMAC_STAT bit 0x80 = 1<<7)");

    /* --- Register address decode: SIF2 CHCR/MADR/QWC/TADR --- */
    uint32_t v;
    CHECK(dma_mmio_write32(0x1000C820u, 99) == 1, "write to SIF2 QWC (+0x20) handled");
    CHECK(dma_mmio_read32(0x1000C820u, &v) == 1 && v == 99, "SIF2 QWC roundtrip");
    g_dma.chan[DMA_CHANNEL_SIF2].qwc = 0; /* reset for the real kick test below */

    /* --- NORMAL mode kick via SIF2's real base address --- */
    for (int i = 0; i < 16; i++) g_ram[0x5000 + i] = (uint8_t)(0x70 + i);
    g_dma.chan[DMA_CHANNEL_SIF2].madr = 0x5000;
    g_dma.chan[DMA_CHANNEL_SIF2].qwc = 1;
    dma_mmio_write32(0x1000C800u, 0x00000100u); /* CHCR: MOD=0 (normal), STR=1 */

    CHECK(g_sink_calls == 1, "SIF2 NORMAL kick: sink called exactly once");
    CHECK(g_sink_last_channel == DMA_CHANNEL_SIF2, "SIF2 NORMAL kick: sink received correct channel number (7)");
    CHECK(g_sink_capture_len == 16, "SIF2 NORMAL kick: exactly 1 quadword delivered");
    CHECK(memcmp(g_sink_capture, g_ram + 0x5000, 16) == 0, "SIF2 NORMAL kick: delivered data matches source RAM");
    CHECK((dma_get_state()->chan[DMA_CHANNEL_SIF2].chcr & 0x100u) == 0, "SIF2 NORMAL kick: STR cleared after completion");
    CHECK(dma_get_state()->chan[DMA_CHANNEL_SIF2].last_error == DMA_ERR_NONE, "SIF2 NORMAL kick: no error");

    /* --- The bit task #172's wait loop actually polls --- */
    CHECK(dma_mmio_read32(0x1000E010u, &v) == 1, "DMAC_STAT (0x1000E010) read handled");
    CHECK((v & 0x80u) != 0, "DMAC_STAT bit 0x80 (SIF2 completion, 1<<7) set after SIF2 kick - matches 0x8000F768's real OR-condition");

    /* --- write-1-to-clear semantics on that same bit (task #176 DMAC_STAT model) --- */
    dma_mmio_write32(0x1000E010u, 0x80u);
    dma_mmio_read32(0x1000E010u, &v);
    CHECK((v & 0x80u) == 0, "DMAC_STAT bit 0x80 clears on write-1 (real hardware semantics, re-verified for SIF2 specifically)");

    /* --- CHAIN mode via SIF2 (REFE tag), confirming tag-walking is
     * also channel-agnostic, not just NORMAL mode --- */
    g_sink_capture_len = 0; g_sink_calls = 0;
    memset(g_ram, 0, sizeof(g_ram));
    wle32(g_ram + 0x6000, (DMA_TAG_REFE << 28) | 1u);
    wle32(g_ram + 0x6004, 0x7000u);
    for (int i = 0; i < 16; i++) g_ram[0x7000 + i] = (uint8_t)(0x90 + i);

    g_dma.chan[DMA_CHANNEL_SIF2].tadr = 0x6000;
    dma_mmio_write32(0x1000C800u, 0x00000104u); /* CHCR: MOD=1 (chain), STR=1 */

    CHECK(g_sink_calls == 1, "SIF2 CHAIN (REFE) kick: sink called once");
    CHECK(memcmp(g_sink_capture, g_ram + 0x7000, 16) == 0, "SIF2 CHAIN (REFE) kick: delivered data matches source RAM");
    CHECK((dma_get_state()->chan[DMA_CHANNEL_SIF2].chcr & 0x100u) == 0, "SIF2 CHAIN kick: STR cleared after REFE terminates");

    /* --- channel isolation: SIF2 activity doesn't touch SIF0/SIF1/GIF --- */
    CHECK(dma_get_state()->chan[DMA_CHANNEL_SIF0].madr == 0, "SIF0 channel untouched by SIF2 activity");
    CHECK(dma_get_state()->chan[DMA_CHANNEL_SIF1].madr == 0, "SIF1 channel untouched by SIF2 activity");
    CHECK(dma_get_state()->chan[DMA_CHANNEL_GIF].madr == 0, "GIF channel untouched by SIF2 activity");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
