/*
 * test_dma_reply_delivered.c - Round 225 (task #366/#172, 265th
 * finding): regression coverage for dma_channel_note_reply_delivered(),
 * the bookkeeping-only companion to dma_channel_receive_quadwords()
 * added this round for ee_core.c's SIF-RPC reply call sites
 * (sif_cmd_iop_send_rpcinit_ready()/sif_cmd_iop_send_rpc_bind_rend()),
 * which write reply bytes directly via ee_mem_write32() (already-
 * correct EE-visible data, unchanged this round) but never touched
 * the SIF0 channel's own MADR/QWC/quadwords_transferred hardware
 * state - see dma.h's doc comment above the declaration for the full
 * grounding and the Round 224 correction this addresses.
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

    /* --- Test 1: 48-byte reply (matches sif_cmd_iop_send_rpc_bind_rend's real packet size) --- */
    g_dma.chan[DMA_CHANNEL_SIF0].madr = 0xDEADBEEFu; /* pre-existing MADR must NOT matter/be required */
    g_dma.chan[DMA_CHANNEL_SIF0].qwc = 10;
    dma_channel_note_reply_delivered(DMA_CHANNEL_SIF0, 0x3000u, 48u);
    CHECK(g_dma.chan[DMA_CHANNEL_SIF0].madr == 0x3000u + 48u, "48-byte reply: MADR set to dest+nbytes (3 quadwords)");
    CHECK(g_dma.chan[DMA_CHANNEL_SIF0].qwc == 7, "48-byte reply: QWC decremented by 3 (10 - 3 = 7)");
    CHECK(g_dma.chan[DMA_CHANNEL_SIF0].quadwords_transferred == 3, "48-byte reply: lifetime counter advanced by 3");
    CHECK((dma_get_state()->d_stat & (1u << DMA_CHANNEL_SIF0)) != 0, "48-byte reply: real completion status (DMAC_STAT) signaled");

    /* --- Test 2: 24-byte reply (matches sif_cmd_iop_send_rpcinit_ready's real packet size) - not quadword-aligned, must round UP --- */
    g_dma.chan[DMA_CHANNEL_SIF0].madr = 0;
    g_dma.chan[DMA_CHANNEL_SIF0].qwc = 5;
    g_dma.d_stat = 0; /* isolate this check from Test 1's already-set bit */
    dma_channel_note_reply_delivered(DMA_CHANNEL_SIF0, 0x4000u, 24u);
    CHECK(g_dma.chan[DMA_CHANNEL_SIF0].madr == 0x4000u + 32u, "24-byte reply: MADR rounds UP to 2 whole quadwords (32 bytes)");
    CHECK(g_dma.chan[DMA_CHANNEL_SIF0].qwc == 3, "24-byte reply: QWC decremented by 2 (rounded-up quadword count)");

    /* --- Test 3: QWC doesn't underflow if channel had fewer QWC pending than delivered --- */
    g_dma.chan[DMA_CHANNEL_SIF0].qwc = 1;
    dma_channel_note_reply_delivered(DMA_CHANNEL_SIF0, 0x5000u, 48u);
    CHECK(g_dma.chan[DMA_CHANNEL_SIF0].qwc == 0, "reply delivery: QWC clamps to 0 rather than underflowing");

    /* --- Test 4: invalid channel index is a safe no-op, not a crash --- */
    uint32_t transferred_before = g_dma.chan[DMA_CHANNEL_SIF1].quadwords_transferred;
    dma_channel_note_reply_delivered(-1, 0x6000u, 48u);
    dma_channel_note_reply_delivered(DMA_CHANNEL_COUNT, 0x6000u, 48u);
    CHECK(g_dma.chan[DMA_CHANNEL_SIF1].quadwords_transferred == transferred_before, "invalid channel index: no cross-channel side effect");

    /* --- Test 5: does NOT perform any byte copy (caller's own ee_mem_write32 already did) --- */
    memset(g_ram + 0x7000, 0xAA, 64);
    dma_channel_note_reply_delivered(DMA_CHANNEL_SIF0, 0x7000u, 48u);
    CHECK(g_ram[0x7000] == 0xAA, "reply delivery: does not touch EE RAM contents (bookkeeping only)");

    /* --- Test 6: channels don't cross-contaminate --- */
    CHECK(g_dma.chan[DMA_CHANNEL_GIF].madr == 0, "untouched GIF channel stayed zero throughout");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
