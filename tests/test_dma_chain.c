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

static void test_sink(int channel, const uint8_t *data, uint32_t qwc)
{
    (void)channel;
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
    dma_set_sink(DMA_CHANNEL_GIF, test_sink);

    /* --- Test 1: NORMAL mode --- */
    for (int i = 0; i < 16; i++) g_ram[0x1000 + i] = (uint8_t)(0x10 + i); /* 1 quadword of known data */
    g_dma.chan[DMA_CHANNEL_GIF].madr = 0x1000;
    g_dma.chan[DMA_CHANNEL_GIF].qwc = 1;
    dma_mmio_write32(0x1000A000u, 0x00000100u); /* CHCR: MOD=0 (normal), STR=1 */

    CHECK(g_sink_calls == 1, "NORMAL mode: sink called exactly once");
    CHECK(g_sink_capture_len == 16, "NORMAL mode: exactly 1 quadword (16 bytes) delivered");
    CHECK(memcmp(g_sink_capture, g_ram + 0x1000, 16) == 0, "NORMAL mode: delivered data matches source RAM");
    CHECK((dma_get_state()->chan[DMA_CHANNEL_GIF].chcr & 0x100u) == 0, "NORMAL mode: STR cleared after completion");
    CHECK(dma_get_state()->chan[DMA_CHANNEL_GIF].last_error == DMA_ERR_NONE, "NORMAL mode: no error");

    /* --- Test 2: CHAIN mode with CNT -> CNT -> REFE --- */
    g_sink_capture_len = 0; g_sink_calls = 0;
    memset(g_ram, 0, sizeof(g_ram));

    /* Tag 1 at 0x2000: DMA_TAG_CNT, qwc=1, data follows immediately at 0x2010 */
    wle32(g_ram + 0x2000, (DMA_TAG_CNT << 28) | 1u); /* control word: ID=CNT, QWC=1 */
    wle32(g_ram + 0x2004, 0);                         /* addr word unused for CNT */
    for (int i = 0; i < 16; i++) g_ram[0x2010 + i] = (uint8_t)(0x20 + i); /* tag1's data */

    /* Tag 2 at 0x2020 (right after tag1's 16-byte tag + 16-byte data):
     * DMA_TAG_REFE, qwc=1, data at address 0x3000 (out of line), then stop. */
    wle32(g_ram + 0x2020, (DMA_TAG_REFE << 28) | 1u);
    wle32(g_ram + 0x2024, 0x3000u);
    for (int i = 0; i < 16; i++) g_ram[0x3000 + i] = (uint8_t)(0x30 + i); /* tag2's out-of-line data */

    g_dma.chan[DMA_CHANNEL_GIF].tadr = 0x2000;
    dma_mmio_write32(0x1000A000u, 0x00000104u); /* CHCR: MOD=1 (chain), STR=1 */

    CHECK(g_sink_calls == 2, "CHAIN mode: sink called once per tag (2 tags total)");
    CHECK(g_sink_capture_len == 32, "CHAIN mode: 2 quadwords total delivered");
    CHECK(memcmp(g_sink_capture, g_ram + 0x2010, 16) == 0, "CHAIN mode: tag1 (CNT) delivered its inline data correctly");
    CHECK(memcmp(g_sink_capture + 16, g_ram + 0x3000, 16) == 0, "CHAIN mode: tag2 (REFE) delivered its out-of-line data correctly");
    CHECK((dma_get_state()->chan[DMA_CHANNEL_GIF].chcr & 0x100u) == 0, "CHAIN mode: STR cleared after REFE terminates the chain");
    CHECK(dma_get_state()->chan[DMA_CHANNEL_GIF].last_error == DMA_ERR_NONE, "CHAIN mode: no error for the fully-supported chain");

    /* --- Test 3: REF tag (task #447/#521, Round 553) - real PCSX2 source
     * (docs/reference/pcsx2/pcsx2/Hw.cpp's hwDmacSrcChainWithStack()):
     * "Transfer QWC from ADDR field", "Set TADR to next tag", CONTINUE (does
     * not stop the chain, unlike REFE). Chain: tag1 (REF) at 0x4000 with
     * ADDR=0x5000 (out-of-line data), qwc=1, continues to tag2 at 0x4010
     * (right after tag1's 16-byte tag, per "tadr += 16" - REF's data is
     * OUT of line, so unlike CNT/NEXT no data-sized gap is added here);
     * tag2 is a REFE at 0x6000 that stops the chain, so this test also
     * exercises "REF successfully hands off to a normal terminating tag". */
    g_sink_capture_len = 0; g_sink_calls = 0;
    memset(g_ram, 0, sizeof(g_ram));
    wle32(g_ram + 0x4000, (DMA_TAG_REF << 28) | 1u); /* tag1: REF, qwc=1 */
    wle32(g_ram + 0x4004, 0x5000u);                   /* tag1: ADDR = 0x5000 */
    for (int i = 0; i < 16; i++) g_ram[0x5000 + i] = (uint8_t)(0x50 + i); /* tag1's out-of-line data */
    wle32(g_ram + 0x4010, (DMA_TAG_REFE << 28) | 1u); /* tag2: REFE, qwc=1, right after tag1 (16 bytes) */
    wle32(g_ram + 0x4014, 0x6000u);                   /* tag2: ADDR = 0x6000 */
    for (int i = 0; i < 16; i++) g_ram[0x6000 + i] = (uint8_t)(0x60 + i); /* tag2's out-of-line data */

    g_dma.chan[DMA_CHANNEL_GIF].tadr = 0x4000;
    dma_mmio_write32(0x1000A000u, 0x00000104u); /* CHCR: MOD=1 (chain), STR=1 */

    CHECK(dma_get_state()->chan[DMA_CHANNEL_GIF].last_error == DMA_ERR_NONE, "REF tag: no error (real hardware tag type, now implemented)");
    CHECK(g_sink_calls == 2, "REF tag: sink called once per tag (REF then REFE)");
    CHECK(g_sink_capture_len == 32, "REF tag: 2 quadwords total delivered");
    CHECK(memcmp(g_sink_capture, g_ram + 0x5000, 16) == 0, "REF tag: delivered its out-of-line data (from ADDR field) correctly");
    CHECK(memcmp(g_sink_capture + 16, g_ram + 0x6000, 16) == 0, "REF tag: chain continued correctly into the terminating REFE tag");
    CHECK((dma_get_state()->chan[DMA_CHANNEL_GIF].chcr & 0x100u) == 0, "REF tag: STR cleared after the chain's REFE terminates");

    /* --- Test 4: CALL remains a genuinely unsupported tag ID (no evidence
     * yet this project's traces need the address-stack mechanism it
     * requires - see source/hw/dma.c's default: case comment) - keeps the
     * "unsupported tag flags an error instead of misbehaving" path tested. */
    g_sink_capture_len = 0; g_sink_calls = 0;
    memset(g_ram, 0, sizeof(g_ram));
    wle32(g_ram + 0x7000, (DMA_TAG_CALL << 28) | 1u); /* CALL - still not implemented */
    wle32(g_ram + 0x7004, 0x8000u);

    g_dma.chan[DMA_CHANNEL_GIF].tadr = 0x7000;
    dma_mmio_write32(0x1000A000u, 0x00000104u);

    CHECK(dma_get_state()->chan[DMA_CHANNEL_GIF].last_error == DMA_ERR_UNSUPPORTED_TAG, "unsupported tag ID (CALL) sets last_error instead of silently mishandling it");
    CHECK((dma_get_state()->chan[DMA_CHANNEL_GIF].chcr & 0x100u) == 0, "unsupported tag also clears STR (doesn't hang with STR stuck on)");
    CHECK(g_sink_calls == 0, "unsupported tag: no data delivered (correctly refuses rather than guessing)");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
