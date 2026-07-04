/*
 * dma.c - EE DMA controller register skeleton
 *
 * See include/core/hw/dma.h for scope notes. Register layout matches
 * real PS2 hardware (cross-checked against PCSX2's pcsx2/Hw.h), but
 * this file only stores/returns register values - it does not
 * execute any actual DMA transfers yet.
 */

#include "core/hw/dma.h"
#include <string.h>

static dma_state_t g_dma;

void dma_init(void)
{
    memset(&g_dma, 0, sizeof(g_dma));
}

dma_state_t *dma_get_state(void) { return &g_dma; }

#define DMAC_BASE       0x10008000u
#define DMAC_CHAN_END   0x1000E000u
#define DMAC_CTRL_BASE  0x1000E000u
#define DMAC_CTRL_END   0x1000F000u
#define CHAN_BLOCK_SIZE 0x400u

/* Explicit (base, size, channel) table - VIF0/VIF1/GIF each get a
 * full 0x1000-byte block, but fromIPU/toIPU and the SIFx/SPR channels
 * pack two channels into the same 0x1000 region (0x400 bytes each),
 * so this can't be resolved by masking to one fixed block size -
 * must check explicit ranges, smallest/most-specific first. */
typedef struct { uint32_t base, size; int channel; } dma_range_t;

static const dma_range_t s_ranges[] = {
    { 0x10008000u, 0x1000u, DMA_CHANNEL_VIF0 },
    { 0x10009000u, 0x1000u, DMA_CHANNEL_VIF1 },
    { 0x1000A000u, 0x1000u, DMA_CHANNEL_GIF },
    { 0x1000B000u, 0x0400u, DMA_CHANNEL_FROMIPU },
    { 0x1000B400u, 0x0400u, DMA_CHANNEL_TOIPU },
    { 0x1000C000u, 0x0400u, DMA_CHANNEL_SIF0 },
    { 0x1000C400u, 0x0400u, DMA_CHANNEL_SIF1 },
    { 0x1000C800u, 0x0400u, DMA_CHANNEL_SIF2 },
    { 0x1000D000u, 0x0400u, DMA_CHANNEL_FROMSPR },
    { 0x1000D400u, 0x0400u, DMA_CHANNEL_TOSPR },
};

static int decode_channel(uint32_t addr, uint32_t *reg_off)
{
    for (size_t i = 0; i < sizeof(s_ranges) / sizeof(s_ranges[0]); i++) {
        if (addr >= s_ranges[i].base && addr < s_ranges[i].base + s_ranges[i].size) {
            *reg_off = addr - s_ranges[i].base;
            return s_ranges[i].channel;
        }
    }
    return -1;
}

static uint32_t *channel_reg_ptr(dma_channel_t *c, uint32_t off)
{
    switch (off) {
    case 0x00: return &c->chcr;
    case 0x10: return &c->madr;
    case 0x20: return &c->qwc;
    case 0x30: return &c->tadr;
    case 0x40: return &c->asr0;
    case 0x50: return &c->asr1;
    case 0x80: return &c->sadr;
    default: return NULL;
    }
}

static uint32_t *ctrl_reg_ptr(uint32_t addr)
{
    switch (addr) {
    case 0x1000E000u: return &g_dma.d_ctrl;
    case 0x1000E010u: return &g_dma.d_stat;
    case 0x1000E020u: return &g_dma.d_pcr;
    case 0x1000E030u: return &g_dma.d_sqwc;
    case 0x1000E040u: return &g_dma.d_rbsr;
    case 0x1000E050u: return &g_dma.d_rbor;
    case 0x1000E060u: return &g_dma.d_stadr;
    default: return NULL;
    }
}

int dma_mmio_read32(uint32_t addr, uint32_t *out_val)
{
    if (addr >= DMAC_BASE && addr < DMAC_CHAN_END) {
        uint32_t off;
        int ch = decode_channel(addr, &off);
        if (ch < 0) { *out_val = 0; return 1; }
        uint32_t *reg = channel_reg_ptr(&g_dma.chan[ch], off);
        *out_val = reg ? *reg : 0;
        return 1;
    }
    if (addr >= DMAC_CTRL_BASE && addr < DMAC_CTRL_END) {
        uint32_t *reg = ctrl_reg_ptr(addr);
        *out_val = reg ? *reg : 0;
        return 1;
    }
    return 0;
}

int dma_mmio_write32(uint32_t addr, uint32_t val)
{
    if (addr >= DMAC_BASE && addr < DMAC_CHAN_END) {
        uint32_t off;
        int ch = decode_channel(addr, &off);
        if (ch < 0) return 1; /* consumed, ignored - unknown sub-register */
        uint32_t *reg = channel_reg_ptr(&g_dma.chan[ch], off);
        if (reg) {
            /* D_CHCR bit 8 (STR - start) being set would normally kick
             * off a real transfer. We don't execute transfers yet, so
             * we just latch the register value - see docs/ROADMAP.md. */
            *reg = val;
        }
        return 1;
    }
    if (addr >= DMAC_CTRL_BASE && addr < DMAC_CTRL_END) {
        uint32_t *reg = ctrl_reg_ptr(addr);
        if (reg) *reg = val;
        return 1;
    }
    return 0;
}
