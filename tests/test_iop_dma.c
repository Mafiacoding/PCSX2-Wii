/*
 * test_iop_dma.c - host-native test for source/hw/iop_dma.c
 *
 * Covers per-channel register roundtrip/address decoding (including
 * the channel-5 gap), and the DMA_ICR/DMA_ICR2 write-side bit logic
 * ported from real PCSX2 (ps2/Iop/IopHwWrite.cpp): plain overwrite of
 * bits 0-23, write-1-to-clear on the per-channel flag bits 24-30, and
 * the recomputed master IRQ flag (bit 31).
 */
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "hw/iop_dma.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else printf("ok:   %s\n", msg); \
} while (0)

int main(void)
{
    iop_dma_init();

    uint32_t v;

    /* --- per-channel register roundtrip, a few representative channels --- */

    /* Channel 0 (MDEC in), base 0x1F801080 */
    CHECK(iop_dma_mmio_write32(0x1F801080u, 0x11111111u) == 1, "ch0 MADR write accepted");
    CHECK(iop_dma_mmio_write32(0x1F801084u, 0x22222222u) == 1, "ch0 BCR write accepted");
    CHECK(iop_dma_mmio_write32(0x1F801088u, 0x33333333u) == 1, "ch0 CHCR write accepted");
    CHECK(iop_dma_mmio_write32(0x1F80108Cu, 0x44444444u) == 1, "ch0 TADR write accepted");
    iop_dma_mmio_read32(0x1F801080u, &v); CHECK(v == 0x11111111u, "ch0 MADR readback");
    iop_dma_mmio_read32(0x1F801084u, &v); CHECK(v == 0x22222222u, "ch0 BCR readback");
    iop_dma_mmio_read32(0x1F801088u, &v); CHECK(v == 0x33333333u, "ch0 CHCR readback");
    iop_dma_mmio_read32(0x1F80108Cu, &v); CHECK(v == 0x44444444u, "ch0 TADR readback");

    /* Channel 9 (SIF0), base 0x1F801520 - the channel most relevant to
     * this project's EE<->IOP work */
    iop_dma_mmio_write32(0x1F801528u, 0xCAFEBABEu); /* CHCR */
    iop_dma_mmio_read32(0x1F801528u, &v);
    CHECK(v == 0xCAFEBABEu, "ch9 (SIF0) CHCR roundtrip");

    /* Channel 5 does not exist on real hardware - its address range
     * (0x1F8010D0-0x1F8010DF, between ch4 at 0xC0 and ch6 at 0xE0)
     * must NOT be claimed. */
    CHECK(iop_dma_mmio_read32(0x1F8010D0u, &v) == 0, "channel 5 gap: address not claimed on read");
    CHECK(iop_dma_mmio_write32(0x1F8010D0u, 0x1u) == 0, "channel 5 gap: address not claimed on write");

    /* Writing channel 0's registers must not leak into channel 1
     * (base 0x1F801090) - basic isolation check. */
    iop_dma_mmio_read32(0x1F801090u, &v);
    CHECK(v == 0u, "channel 1 unaffected by channel 0's writes");

    /* --- DMA_PCR: plain read/write --- */
    iop_dma_mmio_write32(0x1F8010F0u, 0x12345678u);
    iop_dma_mmio_read32(0x1F8010F0u, &v);
    CHECK(v == 0x12345678u, "DMA_PCR plain roundtrip");

    /* --- DMA_ICR: bit-level write semantics --- */

    /* Set enable bit for a channel (bit 16, "channel 0 enable") plus
     * master-enable (bit 23), and the force-IRQ bit is NOT set. */
    iop_dma_mmio_write32(0x1F8010F4u, (1u << 16) | (1u << 23));
    iop_dma_mmio_read32(0x1F8010F4u, &v);
    CHECK((v & 0x80000000u) == 0u,
          "DMA_ICR: enable set but no flag set yet -> master IRQ bit (31) stays clear");

    /* Flag bits (24-30) can only ever be CLEARED by a software write
     * (write-1-to-clear), never SET - on real hardware they're only
     * ever set by an actual DMA completion event, which this
     * register-stub model doesn't simulate (see iop_dma.h). To
     * exercise "master bit reflects enable&flag" without a fake
     * completion-event hook, simulate the hardware side directly
     * (same approach tests/test_sif.c uses for SIF's SMFLAG: poke
     * the underlying state, not the write path, to represent
     * something an external event would have done) and then perform
     * a write that does NOT touch the flag bit, to trigger the
     * master-bit recompute against that pre-set flag. */
    g_dma.icr |= (1u << 24); /* simulate: channel 0's DMA completed and flagged itself */
    iop_dma_mmio_write32(0x1F8010F4u, (1u << 16) | (1u << 23)); /* re-assert enable bits, don't touch bit 24 */
    iop_dma_mmio_read32(0x1F8010F4u, &v);
    CHECK((v & (1u << 16)) != 0u, "DMA_ICR: enable bit 16 stuck (plain overwrite of bits 0-23)");
    CHECK((v & (1u << 24)) != 0u, "DMA_ICR: pre-set flag bit 24 preserved (write value didn't target it)");
    CHECK((v & 0x80000000u) != 0u,
          "DMA_ICR: matching enable+flag pair -> master IRQ bit (31) now set");

    /* Acknowledge (clear) the flag bit by writing a 1 to bit 24 -
     * write-1-to-clear on the flag bits specifically. Enable bits
     * (16, 23) are NOT part of the write-1-clear mask, so writing 1s
     * there again just re-asserts them (plain overwrite), not clears
     * them - confirm both behaviors in the same write. */
    iop_dma_mmio_write32(0x1F8010F4u, (1u << 16) | (1u << 23) | (1u << 24));
    iop_dma_mmio_read32(0x1F8010F4u, &v);
    CHECK((v & (1u << 24)) == 0u, "DMA_ICR: writing 1 to a flag bit clears it (write-1-to-clear)");
    CHECK((v & (1u << 16)) != 0u, "DMA_ICR: enable bit 16 still set (bits 0-23 are plain overwrite, not clear-on-write)");
    CHECK((v & 0x80000000u) == 0u,
          "DMA_ICR: master IRQ bit clears once the matching flag is acknowledged");

    /* Force-IRQ bit (15) alone sets the master IRQ flag regardless of
     * any channel enable/flag state. */
    iop_dma_mmio_write32(0x1F8010F4u, (1u << 15));
    iop_dma_mmio_read32(0x1F8010F4u, &v);
    CHECK((v & 0x80000000u) != 0u, "DMA_ICR: force-IRQ bit (15) alone sets the master IRQ bit");

    /* --- DMA_ICR2: same logic, independent state --- */
    g_dma.icr2 |= (1u << 24); /* simulate a completion event, same approach as ICR above */
    iop_dma_mmio_write32(0x1F801574u, (1u << 16) | (1u << 23));
    iop_dma_mmio_read32(0x1F801574u, &v);
    CHECK((v & 0x80000000u) != 0u, "DMA_ICR2: same bit logic as DMA_ICR (matching enable+flag sets master bit)");
    iop_dma_mmio_read32(0x1F8010F4u, &v);
    CHECK((v & 0x80000000u) != 0u,
          "DMA_ICR and DMA_ICR2 are independent registers (ICR's earlier force-bit state undisturbed by ICR2 writes)");

    /* --- Round 114 (task #172/#269/#270): iop_dma_signal_channel_done()
     * - the real per-channel completion signal this file's own
     * earlier comment (above) noted this register-stub model didn't
     * yet have. Covers both real registers (DMA_ICR for channels
     * 0-6, DMA_ICR2 for channels 7-12) and the real master-enable
     * gate (DMA_ICR bit 23, per Round 113's cited intrman.c - always
     * DMA_ICR, even when signaling a DMA_ICR2-owned channel). --- */
    {
        iop_dma_init();
        iop_intc_init();

        /* Channel 2 (dicr1 range) with nothing enabled yet: flag bit
         * gets set (real hardware always flags completion), but no
         * IRQ is raised since neither the per-channel enable bit nor
         * the master-enable bit is set. */
        iop_dma_signal_channel_done(2);
        CHECK((g_dma.icr & (1u << (2 + 24))) != 0u, "channel 2 completion sets the real DMA_ICR flag bit (local_ch+24) even with nothing enabled");
        CHECK(iop_intc_get_state()->istat == 0u, "channel 2 completion with nothing enabled raises no IOP_IRQ_DMA (istat still 0)");
        CHECK(iop_intc_get_state()->istat_hi == 0u, "channel 2 completion with nothing enabled raises no soft irq either");

        /* Now enable channel 2 for real (bit 16+2=18) and set the
         * real master-enable bit (23) directly, mirroring what
         * EnableIntr(0x22) would do (Round 113) without depending on
         * that HLE layer here - this file only tests iop_dma.c. */
        iop_dma_init();
        iop_intc_init();
        g_dma.icr |= (1u << (2 + 16)) | 0x800000u; /* real enable bit + real master-enable bit */
        iop_dma_signal_channel_done(2);
        CHECK((iop_intc_get_state()->istat & (1u << 3)) != 0u, "channel 2 completion (enabled) raises the real IOP_IRQ_DMA hardware line (istat bit 3)");
        CHECK((iop_intc_get_state()->istat_hi & (1u << (0x22 - 32))) != 0u, "channel 2 completion (enabled) also raises the Round 112 soft-dispatch irq 0x22");

        /* Channel 9 (SIF0, dicr2 range) - the exact real channel this
         * project's Round 111-113 work has tracked throughout. Local
         * index within dicr2 is channel-7=2, so enable bit is 2+16=18,
         * flag bit is 2+24=26 - but the real master gate is STILL
         * DMA_ICR's bit 23 (not DMA_ICR2's), per Round 113's citation. */
        iop_dma_init();
        iop_intc_init();
        g_dma.icr2 |= (1u << (2 + 16)); /* real per-channel enable bit in DICR2 */
        g_dma.icr  |= 0x800000u;        /* real master-enable bit, always in DICR1 */
        iop_dma_signal_channel_done(9);
        CHECK((g_dma.icr2 & (1u << (2 + 24))) != 0u, "channel 9 (SIF0) completion sets the real DMA_ICR2 flag bit");
        CHECK((iop_intc_get_state()->istat & (1u << 3)) != 0u, "channel 9 (SIF0) completion (enabled) raises the real IOP_IRQ_DMA hardware line");
        CHECK((iop_intc_get_state()->istat_hi & (1u << (0x2A - 32))) != 0u, "channel 9 (SIF0) completion raises the real irq 0x2A (IOP_IRQ_DMA_SIF0) via Round 112's soft dispatch");

        /* Channel 9 completion does NOT raise anything if DMA_ICR's
         * master-enable bit (23) is missing, even with DICR2's own
         * per-channel enable bit set - confirms the real cross-
         * register master gate is actually being checked, not just
         * assumed true. */
        iop_dma_init();
        iop_intc_init();
        g_dma.icr2 |= (1u << (2 + 16)); /* enabled locally, but... */
        /* ...g_dma.icr's bit 23 deliberately left clear */
        iop_dma_signal_channel_done(9);
        CHECK(iop_intc_get_state()->istat == 0u, "channel 9 completion without DMA_ICR's real master-enable bit (23) raises nothing");

        /* Invalid channels (5, and out-of-range) are safely ignored,
         * matching the same channel-5 gap already established for
         * MMIO addressing above. */
        iop_dma_init();
        iop_intc_init();
        iop_dma_signal_channel_done(5);
        iop_dma_signal_channel_done(13);
        iop_dma_signal_channel_done(-1);
        CHECK(g_dma.icr == 0u && g_dma.icr2 == 0u, "invalid channels (5, 13, -1) are safely ignored, no register touched");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
