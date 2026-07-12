#ifndef PCSX2WII_DMA_H
#define PCSX2WII_DMA_H

#include <stdint.h>

/*
 * EE DMA controller register model - skeleton only.
 *
 * Real memory map (from PCSX2's pcsx2/Hw.h), each channel's register
 * block is 0x400 bytes apart starting at 0x10008000:
 *   VIF0 0x10008000  VIF1  0x10009000  GIF    0x1000A000
 *   fromIPU 0x1000B000  toIPU 0x1000B400
 *   SIF0 0x1000C000  SIF1  0x1000C400  SIF2   0x1000C800
 *   fromSPR 0x1000D000  toSPR 0x1000D400
 * with the shared D_CTRL/D_STAT/etc block at 0x1000E000-0x1000F000.
 *
 * Within each channel block: CHCR at +0x00, MADR at +0x10, QWC at
 * +0x20, TADR at +0x30 (ASR0/ASR1/SADR follow for some channels).
 *
 * This models the REGISTERS ONLY - reads/writes are stored and
 * returned faithfully, but no actual DMA transfer logic runs yet (no
 * chain-mode tag walking, no interaction with GIF/VIF/SIF/memory).
 * That's the next step - see docs/ROADMAP.md section 3.
 */

typedef struct {
    uint32_t chcr;
    uint32_t madr;
    uint32_t qwc;
    uint32_t tadr;
    uint32_t asr0;
    uint32_t asr1;
    uint32_t sadr;

    /* Not real hardware registers - bookkeeping for our chain-mode
     * engine. last_error: 0 = none, else a DMA_ERR_* code (see below). */
    uint32_t last_error;
    uint32_t quadwords_transferred; /* lifetime counter, handy for tests/debugging */
} dma_channel_t;

#define DMA_ERR_NONE            0
#define DMA_ERR_UNSUPPORTED_TAG 1  /* hit REF/REFS/CALL/RET - not implemented */
#define DMA_ERR_NO_RAM_BOUND    2  /* dma_bind_ee_ram() was never called */
#define DMA_ERR_OUT_OF_BOUNDS   3  /* tag or data address outside bound RAM */

#define DMA_CHANNEL_VIF0    0
#define DMA_CHANNEL_VIF1    1
#define DMA_CHANNEL_GIF     2
#define DMA_CHANNEL_FROMIPU 3
#define DMA_CHANNEL_TOIPU   4
#define DMA_CHANNEL_SIF0    5
#define DMA_CHANNEL_SIF1    6
#define DMA_CHANNEL_SIF2    7
#define DMA_CHANNEL_FROMSPR 8
#define DMA_CHANNEL_TOSPR   9
#define DMA_CHANNEL_COUNT   10

/* Chain-mode tag IDs (see dma_channel_kick doc comment below for which
 * ones are actually implemented). Matches PCSX2's tag_id enum values. */
#define DMA_TAG_REFE 0
#define DMA_TAG_CNT  1
#define DMA_TAG_NEXT 2
#define DMA_TAG_REF  3
#define DMA_TAG_REFS 4
#define DMA_TAG_CALL 5
#define DMA_TAG_RET  6
#define DMA_TAG_END  7

typedef struct {
    dma_channel_t chan[DMA_CHANNEL_COUNT];

    /* Shared DMAC-wide registers (0x1000E000 block) */
    uint32_t d_ctrl;
    uint32_t d_stat;
    uint32_t d_pcr;
    uint32_t d_sqwc;
    uint32_t d_rbsr;
    uint32_t d_rbor;
    uint32_t d_stadr;
} dma_state_t;

/*
 * Task #176: DMAC_STAT (0x1000E010, stored as d_stat above) real
 * write semantics (PCSX2's HwWrite.cpp dmacWrite32<>, case DMAC_STAT):
 * the register is split in half - the LOWER 16 bits are per-channel
 * "transfer done" status flags (bit N = channel N, matching the
 * DMA_CHANNEL_* enum above), and writing 1 to one of THOSE bits
 * CLEARS it (`psHu16(0xe010) &= ~(value & 0xffff)`). The UPPER 16
 * bits are the per-channel interrupt-ENABLE mask (also bit N = channel
 * N), and writing 1 to one of THOSE bits TOGGLES it
 * (`psHu16(0xe012) ^= (u16)(value >> 16)`) - a real hardware quirk,
 * not a plain assignment (see also ee_intc.h's INTC_MASK, which has
 * the identical toggle-on-write-1 behavior). dma_mmio_write32() below
 * special-cases 0x1000E010 for this; dma_channel_kick() sets a
 * channel's low (status) bit on completion via
 * dma_channel_signal_done(), mirroring PCSX2's hwDmacIrq(n) in Hw.cpp
 * (`psHu32(DMAC_STAT) |= 1<<n`).
 */
void dma_init(void);
dma_state_t *dma_get_state(void);

/* Returns 1 if addr falls within the DMAC's memory-mapped I/O range
 * (0x10008000-0x1000FFFF) and handles the read/write; 0 otherwise
 * (caller should fall back to normal RAM access). Writing CHCR with
 * the STR (start) bit set will kick off a real transfer - see below. */
int dma_mmio_read32(uint32_t addr, uint32_t *out_val);
int dma_mmio_write32(uint32_t addr, uint32_t val);

/*
 * Chain-mode DMA transfer engine.
 *
 * dma.c needs to read EE RAM directly (source data + chain tags all
 * live there), so the EE core hands it a pointer at init time rather
 * than dma.h depending on ee_core.h (keeps the hw/ layer decoupled
 * from the CPU core layer). Physical addresses only (already masked
 * to the 0x1FFFFFFF EE physical range by the caller).
 */
void dma_bind_ee_ram(uint8_t *ram, uint32_t ram_size);

/* Called by a real (future) consumer - e.g. GIF packet parsing - to
 * receive the quadwords a channel's transfer produces. 'data' points
 * to qwc*16 bytes, valid only for the duration of the callback. If no
 * sink is registered for a channel, transferred data is simply
 * discarded (the transfer still runs and TADR/MADR/QWC/CHCR update
 * correctly) - this lets the DMA engine be tested and used before any
 * real GIF/VIF consumer exists.
 */
typedef void (*dma_sink_fn)(int channel, const uint8_t *data, uint32_t qwc);
void dma_set_sink(int channel, dma_sink_fn fn);

/*
 * CHCR bit layout (matches real hardware, cross-checked against
 * PCSX2's Dmac.h tDMA_CHCR):
 *   bit 0      DIR (direction, VIF1/SIF2 only)
 *   bits 2-3   MOD (0=normal, 1=chain, 2=interleave)
 *   bits 4-5   ASP (address stack pointer depth - CALL/RET, not impl.)
 *   bit 6      TTE (tag transfer enable - not modeled)
 *   bit 7      TIE (tag interrupt enable - not modeled)
 *   bit 8      STR (start/running - write 1 to kick off a transfer)
 *   bits 16-31 TAG (upper 16 bits of the most recently read tag)
 *
 * Writing CHCR via dma_mmio_write32 with bit 8 set triggers
 * dma_channel_kick() automatically - callers don't need to call it
 * directly, but it's exposed for tests.
 *
 * Chain-mode tag IDs implemented: REFE(0), CNT(1), NEXT(2), END(7) -
 * the four most common ones. REF/REFS(3/4, address-indirection without
 * advancing the tag pointer sequentially) and CALL/RET(5/6, ASR stack)
 * are NOT implemented - encountering one halts that channel's transfer
 * with an error flag rather than silently misbehaving. See
 * docs/ROADMAP.md.
 */
void dma_channel_kick(int channel);

/*
 * Sets DMAC_STAT's low (status) bit for `channel` - real hardware's
 * hwDmacIrq(n) equivalent (PCSX2 Hw.cpp). Called automatically by
 * dma_channel_kick() when a transfer completes; also exposed so
 * ee_core.c's sceSifSetDma (EE syscall 119) implementation - which
 * does its own synchronous RAM-to-RAM copy rather than going through
 * dma_channel_kick() - can signal the same real completion status
 * (task #176).
 */
void dma_channel_signal_done(int channel);

/*
 * Task #176: directly sets (enabled=1) or clears (enabled=0) channel
 * `channel`'s bit in DMAC_STAT's upper (enable-mask) half. Real
 * hardware's EnableDmac()/DisableDmac() BIOS calls (invoked via EE
 * syscalls 22/_EnableDmac and its DisableDmac counterpart) internally
 * perform a raw toggle-write to DMAC_STAT (see the write-1-to-toggle
 * semantics documented above) to reach the desired end state - this
 * project doesn't have the exact BIOS-internal instruction sequence
 * to replicate that raw toggle faithfully, so the syscall 22 handler
 * in ee_core.c calls this to directly set the desired end state
 * instead. Documented simplification, not fabricated register
 * semantics - the STAT/MASK bit LAYOUT and its effect on
 * dma_dmac_interrupt_pending() are real (PCSX2 Hw.cpp/HwWrite.cpp).
 */
void dma_channel_set_irq_enable(int channel, int enabled);

/*
 * Returns 1 if a real DMAC interrupt (Cause.IP3) should currently be
 * pending: (status_low & enable_high) != 0, OR status_low bit 15 set
 * (the BEIS/stall-detect bit) - matches PCSX2's dmacInterrupt() in
 * Hw.cpp, INCLUDING the real DMAC_CTRL.DMAE (master enable, bit 0 of
 * d_ctrl) gate it also checks. ee_core.c's ee_check_dmac_interrupt()
 * calls this every step, mirroring ee_check_timer_interrupt()'s
 * Cause.IP7 pattern for this new external line (task #176).
 */
int dma_dmac_interrupt_pending(void);

#endif
