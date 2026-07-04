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
} dma_channel_t;

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
 * (caller should fall back to normal RAM access). */
int dma_mmio_read32(uint32_t addr, uint32_t *out_val);
int dma_mmio_write32(uint32_t addr, uint32_t val);

#endif
