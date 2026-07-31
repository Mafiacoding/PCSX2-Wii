/*
 * test_dma_inbound.c - Round 198 (task #365): regression coverage
 * for dma_channel_receive_quadwords(), the missing inbound
 * (device -> EE RAM) DMA write capability identified as a genuine
 * structural gap by Round 197's full disassembly-traced root-cause
 * chase (STATUS.md 237th finding): before this round, dma.c's
 * transfer_quadwords()/ram_ptr() machinery only ever READ from EE
 * RAM to feed a registered sink (outbound) - there was no function
 * anywhere in this project that could write IOP-sourced data INTO EE
 * RAM. This test proves the new inbound primitive works correctly in
 * isolation: writes land at the target channel's own MADR, MADR
 * advances and QWC decrements exactly like the outbound path, real
 * completion status (DMAC_STAT) is signaled, out-of-bounds
 * destinations are rejected safely, and channels don't
 * cross-contaminate.
 *
 * Honest scope (see docs/STATUS.md's 238th finding): this proves the
 * CAPABILITY is real and correct - it does NOT by itself claim any
 * specific real IOP-side producer now calls it for the RAM[0x80020E3C]
 * OSDSYS gate Round 197 traced; nothing wires that up yet, since the
 * real trigger for that specific field is still unconfirmed by any
 * citable source (see Round 197/198 notes - not guessed here).
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

int main(void) {
    dma_init();
    memset(g_ram, 0, sizeof(g_ram));
    dma_bind_ee_ram(g_ram, sizeof(g_ram));

    /* --- Test 1: basic inbound write via SIF0 (the real "fromIOP" channel) --- */
    uint8_t payload[32];
    for (int i = 0; i < 32; i++) payload[i] = (uint8_t)(0xA0 + i);

    g_dma.chan[DMA_CHANNEL_SIF0].madr = 0x2000;
    g_dma.chan[DMA_CHANNEL_SIF0].qwc = 5; /* pretend 5 quadwords were expected */

    int rc = dma_channel_receive_quadwords(DMA_CHANNEL_SIF0, payload, 2); /* deliver 2 of them */
    CHECK(rc == 1, "SIF0 inbound write returns success");
    CHECK(memcmp(g_ram + 0x2000, payload, 32) == 0, "SIF0 inbound: payload landed at original MADR");
    CHECK(g_dma.chan[DMA_CHANNEL_SIF0].madr == 0x2000 + 32, "SIF0 inbound: MADR advanced by qwc*16");
    CHECK(g_dma.chan[DMA_CHANNEL_SIF0].qwc == 3, "SIF0 inbound: QWC decremented (5 - 2 = 3)");
    CHECK(g_dma.chan[DMA_CHANNEL_SIF0].quadwords_transferred == 2, "SIF0 inbound: lifetime counter incremented");
    CHECK((dma_get_state()->d_stat & (1u << DMA_CHANNEL_SIF0)) != 0, "SIF0 inbound: real completion status (DMAC_STAT) signaled");

    /* --- Test 2: QWC doesn't underflow if more is delivered than was pending --- */
    g_dma.chan[DMA_CHANNEL_SIF0].qwc = 1;
    rc = dma_channel_receive_quadwords(DMA_CHANNEL_SIF0, payload, 2);
    CHECK(rc == 1, "SIF0 inbound: over-delivery still succeeds");
    CHECK(g_dma.chan[DMA_CHANNEL_SIF0].qwc == 0, "SIF0 inbound: QWC clamps to 0 rather than underflowing");

    /* --- Test 3: zero-qwc call is a safe no-op --- */
    uint32_t madr_before = g_dma.chan[DMA_CHANNEL_SIF0].madr;
    rc = dma_channel_receive_quadwords(DMA_CHANNEL_SIF0, payload, 0);
    CHECK(rc == 1, "SIF0 inbound: qwc=0 returns success");
    CHECK(g_dma.chan[DMA_CHANNEL_SIF0].madr == madr_before, "SIF0 inbound: qwc=0 doesn't move MADR");

    /* --- Test 4: out-of-bounds destination is rejected, not silently clamped --- */
    g_dma.chan[DMA_CHANNEL_SIF1].madr = (uint32_t)sizeof(g_ram) - 8u; /* only 8 bytes left, need 16 */
    rc = dma_channel_receive_quadwords(DMA_CHANNEL_SIF1, payload, 1);
    CHECK(rc == 0, "SIF1 inbound: out-of-bounds destination correctly rejected");

    /* --- Test 5: no EE RAM bound -> safe failure, not a crash --- */
    dma_bind_ee_ram(NULL, 0);
    rc = dma_channel_receive_quadwords(DMA_CHANNEL_SIF0, payload, 1);
    CHECK(rc == 0, "inbound write with no RAM bound fails safely");
    dma_bind_ee_ram(g_ram, sizeof(g_ram));

    /* --- Test 6: channels don't cross-contaminate --- */
    CHECK(g_dma.chan[DMA_CHANNEL_GIF].madr == 0, "untouched GIF channel stayed zero throughout");
    CHECK(g_dma.chan[DMA_CHANNEL_VIF0].quadwords_transferred == 0, "untouched VIF0 channel's counter stayed zero");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
