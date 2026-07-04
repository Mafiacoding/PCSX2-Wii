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

#endif
