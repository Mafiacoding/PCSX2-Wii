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
static uint8_t *g_ee_ram = NULL;
static uint32_t g_ee_ram_size = 0;
static dma_sink_fn g_sinks[DMA_CHANNEL_COUNT];

void dma_init(void)
{
    memset(&g_dma, 0, sizeof(g_dma));
    memset(g_sinks, 0, sizeof(g_sinks));
    /* Deliberately not clearing g_ee_ram/g_ee_ram_size here - dma_init()
     * resets register state on emulator (re)start, but the RAM binding
     * is a one-time wiring done by ee_core_init() at a different point
     * in the boot sequence. */
}

dma_state_t *dma_get_state(void) { return &g_dma; }

void dma_bind_ee_ram(uint8_t *ram, uint32_t ram_size)
{
    g_ee_ram = ram;
    g_ee_ram_size = ram_size;
}

void dma_set_sink(int channel, dma_sink_fn fn)
{
    if (channel >= 0 && channel < DMA_CHANNEL_COUNT)
        g_sinks[channel] = fn;
}

/* Little-endian-explicit RAM access - same reasoning as ee_core.c and
 * iop_core.c: PS2 memory is little-endian, our Wii/PowerPC build
 * target is big-endian, so this can't be a raw memcpy. */
static int ram_read32(uint32_t phys_addr, uint32_t *out)
{
    if (!g_ee_ram || phys_addr + 4 > g_ee_ram_size)
        return 0;
    const uint8_t *p = g_ee_ram + phys_addr;
    *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return 1;
}

static int ram_ptr(uint32_t phys_addr, uint32_t len, const uint8_t **out)
{
    if (!g_ee_ram || phys_addr + len > g_ee_ram_size)
        return 0;
    *out = g_ee_ram + phys_addr;
    return 1;
}

/* Transfers 'qwc' quadwords (16 bytes each) starting at physical
 * address 'addr' to the channel's registered sink (if any). Returns 1
 * on success, 0 if the range falls outside bound RAM. */
static int transfer_quadwords(int channel, uint32_t addr, uint32_t qwc)
{
    if (qwc == 0)
        return 1;
    const uint8_t *p;
    if (!ram_ptr(addr, qwc * 16u, &p))
        return 0;
    if (g_sinks[channel])
        g_sinks[channel](channel, p, qwc);
    g_dma.chan[channel].quadwords_transferred += qwc;
    return 1;
}

/* Reads one 64-bit DMA chain tag (2 words: control word with QWC/ID/
 * IRQ, then the ADDR word) from physical address 'tag_addr'. Matches
 * PCSX2's tDMA_TAG bitfield layout (Dmac.h): QWC in bits 0-15, PCE in
 * 26-27, ID in 28-30, IRQ in bit 31 of the control word; ADDR in bits
 * 0-30 of the address word. */
static int read_chain_tag(uint32_t tag_addr, uint32_t *qwc, uint32_t *id,
                           uint32_t *addr, uint32_t *irq)
{
    uint32_t ctrl, addr_word;
    if (!ram_read32(tag_addr, &ctrl)) return 0;
    if (!ram_read32(tag_addr + 4, &addr_word)) return 0;

    *qwc  = ctrl & 0xFFFFu;
    *id   = (ctrl >> 28) & 0x7u;
    *irq  = (ctrl >> 31) & 0x1u;
    *addr = addr_word & 0x7FFFFFFFu;
    return 1;
}

void dma_channel_kick(int channel)
{
    if (channel < 0 || channel >= DMA_CHANNEL_COUNT)
        return;

    dma_channel_t *ch = &g_dma.chan[channel];
    ch->last_error = DMA_ERR_NONE;

    if (!g_ee_ram) {
        ch->last_error = DMA_ERR_NO_RAM_BOUND;
        ch->chcr &= ~0x100u; /* clear STR - can't run */
        return;
    }

    uint32_t mod = (ch->chcr >> 2) & 0x3u;

    if (mod == 0) {
        /* NORMAL mode: one shot, QWC quadwords straight from MADR. */
        if (!transfer_quadwords(channel, ch->madr, ch->qwc)) {
            ch->last_error = DMA_ERR_OUT_OF_BOUNDS;
        } else {
            ch->madr += ch->qwc * 16u;
            ch->qwc = 0;
        }
        ch->chcr &= ~0x100u; /* transfer complete - clear STR */
        dma_channel_signal_done(channel); /* task #176: real hwDmacIrq(n) equivalent */
        return;
    }

    if (mod == 1) {
        /* CHAIN mode: walk tags starting at TADR. Implements the four
         * most common tag IDs (REFE/CNT/NEXT/END); REF/REFS/CALL/RET
         * are flagged as unsupported rather than silently mishandled -
         * see docs/ROADMAP.md. */
        const int MAX_TAGS_PER_KICK = 4096; /* guards against a corrupt/cyclic chain hanging us forever */
        for (int guard = 0; guard < MAX_TAGS_PER_KICK; guard++) {
            uint32_t qwc, id, addr, irq;
            (void)irq;
            if (!read_chain_tag(ch->tadr, &qwc, &id, &addr, &irq)) {
                ch->last_error = DMA_ERR_OUT_OF_BOUNDS;
                break;
            }

            switch (id) {
            case DMA_TAG_REFE: /* 0: data at ADDR, then stop */
                if (!transfer_quadwords(channel, addr, qwc)) { ch->last_error = DMA_ERR_OUT_OF_BOUNDS; }
                ch->tadr += 16u;
                ch->chcr &= ~0x100u;
                dma_channel_signal_done(channel); /* task #176 */
                return;

            case DMA_TAG_CNT: /* 1: data follows the tag itself, keep going */
                if (!transfer_quadwords(channel, ch->tadr + 16u, qwc)) { ch->last_error = DMA_ERR_OUT_OF_BOUNDS; return; }
                ch->tadr = ch->tadr + 16u + qwc * 16u;
                continue;

            case DMA_TAG_NEXT: /* 2: data follows the tag, next tag is at ADDR */
                if (!transfer_quadwords(channel, ch->tadr + 16u, qwc)) { ch->last_error = DMA_ERR_OUT_OF_BOUNDS; return; }
                ch->tadr = addr;
                continue;

            case DMA_TAG_END: /* 7: data follows the tag, then stop */
                if (!transfer_quadwords(channel, ch->tadr + 16u, qwc)) { ch->last_error = DMA_ERR_OUT_OF_BOUNDS; }
                ch->tadr += 16u + qwc * 16u;
                ch->chcr &= ~0x100u;
                dma_channel_signal_done(channel); /* task #176 */
                return;

            default: /* REF/REFS/CALL/RET - not implemented */
                ch->last_error = DMA_ERR_UNSUPPORTED_TAG;
                ch->chcr &= ~0x100u;
                return;
            }
        }
        /* Fell out of the loop via the guard counter - treat as an error
         * rather than leaving STR set forever. */
        ch->last_error = DMA_ERR_UNSUPPORTED_TAG;
        ch->chcr &= ~0x100u;
        return;
    }

    /* INTERLEAVE mode (SPR only) - not implemented. */
    ch->last_error = DMA_ERR_UNSUPPORTED_TAG;
    ch->chcr &= ~0x100u;
}

/*
 * Task #176: sets DMAC_STAT's low (status) bit for `channel` - see
 * the doc comment on this function in dma.h and PCSX2's hwDmacIrq(n)
 * in Hw.cpp (`psHu32(DMAC_STAT) |= 1<<n`). d_stat's layout: bits 0-9
 * are per-channel status (this function only ever sets one of
 * those), bits 16-25 are the per-channel enable mask written via
 * dma_mmio_write32's special-cased DMAC_STAT toggle-on-write-1 path.
 */
void dma_channel_signal_done(int channel)
{
    if (channel < 0 || channel >= DMA_CHANNEL_COUNT)
        return;
    g_dma.d_stat |= (1u << channel);
}

void dma_channel_set_irq_enable(int channel, int enabled)
{
    if (channel < 0 || channel >= DMA_CHANNEL_COUNT)
        return;
    uint32_t bit = 1u << (16 + channel);
    if (enabled)
        g_dma.d_stat |= bit;
    else
        g_dma.d_stat &= ~bit;
}

int dma_dmac_interrupt_pending(void)
{
    /* Real hardware/PCSX2 dmacInterrupt() (Hw.cpp): pending if
     * (status_low & enable_high) != 0, or status_low bit 15 (BEIS,
     * bus-error/stall-detect - never set by this project, no bus
     * errors modeled, but checked here for completeness/fidelity)
     * is set - AND the DMAC is actually enabled (DMAC_CTRL.DMAE,
     * bit 0 of d_ctrl; real hardware also checks a "DMAC suspended"
     * byte this project doesn't model at all, so that half of the
     * gate is left out rather than fabricated). */
    uint32_t status_low = g_dma.d_stat & 0xFFFFu;
    uint32_t enable_high = (g_dma.d_stat >> 16) & 0xFFFFu;
    if (!(g_dma.d_ctrl & 0x1u))
        return 0; /* DMAC_CTRL.DMAE == 0: master DMA enable is off */
    if ((status_low & enable_high) != 0u)
        return 1;
    if (status_low & 0x8000u)
        return 1;
    return 0;
}

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
            *reg = val;
            /* Writing CHCR (offset 0x00) with STR (bit 8) set kicks off
             * a real transfer - see dma_channel_kick(). */
            if (off == 0x00 && (val & 0x100u))
                dma_channel_kick(ch);
        }
        return 1;
    }
    if (addr == 0x1000E010u) {
        /* Task #176: DMAC_STAT real write semantics (PCSX2 HwWrite.cpp
         * dmacWrite32<>, case DMAC_STAT) - NOT a plain overwrite:
         * lower 16 bits (per-channel status) clear on write-1, upper
         * 16 bits (per-channel enable mask) toggle on write-1. See the
         * doc comment on d_stat's layout in dma.h. */
        uint32_t status_low = g_dma.d_stat & 0xFFFFu;
        uint32_t enable_high = (g_dma.d_stat >> 16) & 0xFFFFu;
        status_low &= ~(val & 0xFFFFu);
        enable_high ^= (val >> 16) & 0xFFFFu;
        g_dma.d_stat = status_low | (enable_high << 16);
        return 1;
    }
    if (addr >= DMAC_CTRL_BASE && addr < DMAC_CTRL_END) {
        uint32_t *reg = ctrl_reg_ptr(addr);
        if (reg) *reg = val;
        return 1;
    }
    return 0;
}
