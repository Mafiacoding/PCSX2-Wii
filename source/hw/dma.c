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
/* Round 572: bound the same way g_ee_ram is (see dma_bind_ee_ram() /
 * dma_bind_scratchpad() below) rather than calling ee_core_get_state()
 * directly - dma.c must stay linkable standalone (several test source
 * files #include this file directly without linking ee_core.o at all;
 * a hard call-time dependency on ee_core_get_state() broke every one
 * of them with an undefined-reference link error the first time this
 * was tried). ee_core_init() wires this in exactly like it already
 * does for g_ee_ram. */
static uint8_t *g_ee_scratch = NULL;
static uint32_t g_ee_scratch_size = 0;
static dma_sink_fn g_sinks[DMA_CHANNEL_COUNT];

void dma_init(void)
{
    memset(&g_dma, 0, sizeof(g_dma));
    memset(g_sinks, 0, sizeof(g_sinks));
    /* Deliberately not clearing g_ee_ram/g_ee_ram_size here - dma_init()
     * resets register state on emulator (re)start, but the RAM binding
     * is a one-time wiring done by ee_core_init() at a different point
     * in the boot sequence. */

    /* Round 539: real hardware/PCSX2 hwReset() sets DMAC_ENABLER and
     * DMAC_ENABLEW to 0x1201 at boot/reset (see dma.h's doc comment on
     * d_enable_state for the full citation) - NOT zero, unlike every
     * other register memset above. */
    g_dma.d_enable_state = 0x1201u;
}

dma_state_t *dma_get_state(void) { return &g_dma; }

void dma_bind_ee_ram(uint8_t *ram, uint32_t ram_size)
{
    g_ee_ram = ram;
    g_ee_ram_size = ram_size;
}

/* Round 572 (task #536/#547): binds the EE's 16KB on-chip scratchpad
 * (the SAME buffer ee_core.c's ee_mem_ptr() exposes to CPU loads/
 * stores at KUSEG 0x70000000-0x70003FFF, ee_state_t.scratch) so
 * dma_resolve_ptr() can route SPR-flagged DMA addresses (real
 * hardware's tDMAC_ADDR bit 31 - see dma_resolve_ptr()'s doc comment)
 * to it instead of misreading them as wild main-RAM offsets. Mirrors
 * dma_bind_ee_ram() exactly, including the "may never be called"
 * safety: dma_resolve_ptr() checks g_ee_scratch for NULL before use,
 * same as every other binding in this file. */
void dma_bind_scratchpad(uint8_t *scratch, uint32_t scratch_size)
{
    g_ee_scratch = scratch;
    g_ee_scratch_size = scratch_size;
}

void dma_set_sink(int channel, dma_sink_fn fn)
{
    if (channel >= 0 && channel < DMA_CHANNEL_COUNT)
        g_sinks[channel] = fn;
}

#define EE_SCRATCH_SIZE (16u * 1024u)

/* Resolves a raw 32-bit DMA address register value (MADR/TADR, or a
 * chain tag's own ADDR word) to a host pointer, per real hardware's
 * tDMAC_ADDR/tDMA_TAG bitfield layout (PCSX2's Dmac.h, vendored at
 * docs/reference/pcsx2/pcsx2/Dmac.h): bits 0-30 are the real address,
 * bit 31 is the SPR (scratchpad) selector - "Memory/SPR Address (only
 * effective for MADR and TADR of non-SPR DMAs)". When SPR is set the
 * low bits address the EE's 16KB on-chip scratchpad - the SAME buffer
 * ee_core.c's ee_mem_ptr() already exposes to CPU loads/stores at
 * KUSEG 0x70000000-0x70003FFF (ee_state_t.scratch) - which is a
 * completely different memory than main RAM, not an offset within it.
 *
 * Round 572 (task #536/#547): found via a live diskless-boot trace
 * that real BIOS code writes VIF1's TADR as an address with bit 31
 * set (observed: 0x80002290) mid-chain. Before this fix, every DMA
 * address (MADR/TADR/tag-ADDR) was used as a flat, unmasked 32-bit
 * offset straight into g_ee_ram - bit 31 alone is ~2GB, always
 * failing the g_ee_ram_size (32MB) bound check. The chain-walk loop's
 * only response to that failure is a silent `break` that leaves the
 * channel's STR bit set and never signals completion (see
 * dma_channel_kick()) - so the channel looks "still busy" forever,
 * and every later MMIO-triggered re-kick just re-fails at the exact
 * same stuck TADR, doing nothing. This was the actual, precise reason
 * VIF1's real DMA traffic (tens of thousands of real kicks) never
 * delivered enough real data to ever reach an MSCAL/MSCNT VIFcode. */
static int dma_resolve_ptr(uint32_t raw_addr, uint32_t len, uint8_t **out)
{
    uint32_t addr = raw_addr & 0x7FFFFFFFu;
    if (raw_addr & 0x80000000u) {
        if (!g_ee_scratch)
            return 0;
        uint32_t off = addr & (EE_SCRATCH_SIZE - 1u);
        if (off + len > g_ee_scratch_size || off + len > EE_SCRATCH_SIZE)
            return 0;
        *out = g_ee_scratch + off;
        return 1;
    }
    if (!g_ee_ram || addr + len > g_ee_ram_size)
        return 0;
    *out = g_ee_ram + addr;
    return 1;
}

/* Little-endian-explicit RAM access - same reasoning as ee_core.c and
 * iop_core.c: PS2 memory is little-endian, our Wii/PowerPC build
 * target is big-endian, so this can't be a raw memcpy. */
static int ram_read32(uint32_t raw_addr, uint32_t *out)
{
    uint8_t *p;
    if (!dma_resolve_ptr(raw_addr, 4, &p))
        return 0;
    *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return 1;
}

static int ram_ptr(uint32_t raw_addr, uint32_t len, const uint8_t **out)
{
    uint8_t *p;
    if (!dma_resolve_ptr(raw_addr, len, &p))
        return 0;
    *out = p;
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
 * 26-27, ID in 28-30, IRQ in bit 31 of the control word; the ADDR word
 * is itself a tDMAC_ADDR (ADDR:31, SPR:1 - see dma_resolve_ptr()'s doc
 * comment above). Round 572: *addr now preserves the raw word
 * (including bit 31/SPR) instead of masking it away here - a
 * NEXT-type tag can legitimately continue the chain into scratchpad,
 * and the caller stores this value straight into ch->tadr, which
 * dma_resolve_ptr() (via ram_read32()/ram_ptr()) already knows how to
 * interpret correctly on its own next use. */
static int read_chain_tag(uint32_t tag_addr, uint32_t *qwc, uint32_t *id,
                           uint32_t *addr, uint32_t *irq)
{
    uint32_t ctrl, addr_word;
    if (!ram_read32(tag_addr, &ctrl)) return 0;
    if (!ram_read32(tag_addr + 4, &addr_word)) return 0;

    *qwc  = ctrl & 0xFFFFu;
    *id   = (ctrl >> 28) & 0x7u;
    *irq  = (ctrl >> 31) & 0x1u;
    *addr = addr_word;
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

            case DMA_TAG_REF: /* 3 */
            case DMA_TAG_REFS: /* 4 - task #447/#521 (Round 553): real PCSX2 source
                 * (docs/reference/pcsx2/pcsx2/Hw.cpp's hwDmacSrcChainWithStack(),
                 * cross-checked against Dmac.h's own tag_id comments) treats REF
                 * and REFS identically for basic chain-walk purposes: "Transfer
                 * QWC from ADDR field" (i.e. the SAME transfer_quadwords(addr,qwc)
                 * shape this file already uses for REFE, just without stopping),
                 * then "Set TADR to next tag" (tadr += 16) and continue the walk
                 * (hwDmacSrcChainWithStack returns false = not-done for both,
                 * unlike REFE's true = done). REFS's real difference from REF is
                 * STADR-based "stall control" (an unrelated flow-control feature,
                 * not modeled here, same honest scope as this file's existing
                 * "no chain-mode stall control" gap) - the basic data-transfer and
                 * chain-advancement behavior this fix needs is identical for both,
                 * so both are handled together rather than leaving REFS on the
                 * unsupported path this fix specifically closes for REF.
                 *
                 * Found via a real, reproducible host-native trace (task #447's
                 * Round 552 fast-boot-patch scratch driver): EELOAD's own real,
                 * unmodified code - reached organically for the first time via
                 * Round 552's "rom0:OSDSYS"-string patch, no register hijacking
                 * involved - calls a real SIF0 chain-mode DMA send (via a function
                 * at 0x00083fd0, called right after a real CreateSema) whose real
                 * tag chain includes a REF tag (id=3) this project had never
                 * implemented; the previous default: path aborted the transfer
                 * (DMA_ERR_UNSUPPORTED_TAG, STR cleared, no completion signal),
                 * which is the most likely reason the real completion interrupt
                 * that should eventually let EELOAD's own code SignalSema() the
                 * semaphore it is WaitSema()-blocked on never fires. */
                if (!transfer_quadwords(channel, addr, qwc)) { ch->last_error = DMA_ERR_OUT_OF_BOUNDS; return; }
                ch->tadr += 16u;
                continue;

            default: /* CALL/RET - not implemented (no evidence yet this project's
                 * traces ever need the address-stack mechanism these two tag
                 * types require; left honestly unsupported per task #447/#521's
                 * evidence-only-implement standard, unlike REF/REFS above which
                 * were directly observed and confirmed against real PCSX2 source). */
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

/*
 * Round 198 (task #365): the missing inbound (device -> EE RAM)
 * write capability - see the doc comment on this function's
 * declaration in dma.h for the full citation trail and honest scope
 * note. Mirrors transfer_quadwords()'s contract exactly, just in the
 * reverse direction: writes to the channel's own MADR instead of
 * reading from it, using a plain byte-for-byte copy (endian-safe
 * regardless of host CPU, same reasoning as ram_ptr()'s existing raw
 * uint8_t* contract for the outbound side - no multi-byte integer
 * interpretation happens here, so the PPC-host/little-endian-PS2-
 * data mismatch that requires explicit byte assembly in ram_read32()
 * simply doesn't apply to a raw copy).
 */
int dma_channel_receive_quadwords(int channel, const uint8_t *data, uint32_t qwc)
{
    if (channel < 0 || channel >= DMA_CHANNEL_COUNT)
        return 0;
    if (!g_ee_ram)
        return 0;
    if (qwc == 0)
        return 1;

    dma_channel_t *ch = &g_dma.chan[channel];
    uint32_t len = qwc * 16u;
    if ((uint64_t)ch->madr + (uint64_t)len > (uint64_t)g_ee_ram_size)
        return 0;

    memcpy(g_ee_ram + ch->madr, data, len);
    ch->madr += len;
    ch->qwc = (ch->qwc > qwc) ? (ch->qwc - qwc) : 0u;
    ch->quadwords_transferred += qwc;

    dma_channel_signal_done(channel); /* real completion status, same as the outbound path */
    return 1;
}

/*
 * Round 225 (task #366/#172, 265th finding): see the doc comment in
 * dma.h above this function's declaration for full grounding. Mirrors
 * dma_channel_receive_quadwords()'s real bookkeeping exactly, minus
 * the byte copy (the caller already performed it directly via
 * ee_mem_write32, matching this project's existing SIF-RPC reply
 * pattern) and targeting the caller-supplied dest_addr instead of the
 * channel's pre-existing MADR.
 */
void dma_channel_note_reply_delivered(int channel, uint32_t dest_addr, uint32_t nbytes)
{
    if (channel < 0 || channel >= DMA_CHANNEL_COUNT)
        return;

    dma_channel_t *ch = &g_dma.chan[channel];
    uint32_t qwc = (nbytes + 15u) / 16u; /* round up to whole quadwords - real DMA transfer granularity */

    ch->madr = dest_addr + qwc * 16u;
    ch->qwc = (ch->qwc > qwc) ? (ch->qwc - qwc) : 0u;
    ch->quadwords_transferred += qwc;

    dma_channel_signal_done(channel);
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
     * bit 0 of d_ctrl) AND not suspended (Round 539: real hardware's
     * DMAC_ENABLER "suspended" byte, `psHu8(DMAC_ENABLER+2) == 1` -
     * previously left out as a documented, deliberately-not-fabricated
     * gap; now modeled for real via d_enable_state, see dma.h). */
    uint32_t status_low = g_dma.d_stat & 0xFFFFu;
    uint32_t enable_high = (g_dma.d_stat >> 16) & 0xFFFFu;
    if (!(g_dma.d_ctrl & 0x1u))
        return 0; /* DMAC_CTRL.DMAE == 0: master DMA enable is off */
    if (((g_dma.d_enable_state >> 16) & 0xFFu) == 0x01u)
        return 0; /* DMAC_ENABLER byte+2 == 1: DMAC suspended */
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
    if (addr == 0x1000F520u || addr == 0x1000F590u) {
        /* Round 539: DMAC_ENABLER/DMAC_ENABLEW - see dma.h's
         * d_enable_state doc comment. Not adjacent to either range
         * above (they live in the same 0x1000F000-0x1000FFFF page as
         * INTC/SIF/MCH, at offset 0x520/0x590), so handled explicitly. */
        *out_val = g_dma.d_enable_state;
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
    if (addr == 0x1000F520u || addr == 0x1000F590u) {
        /* Round 539: DMAC_ENABLER/DMAC_ENABLEW - see dma.h's
         * d_enable_state doc comment. PCSX2 doesn't special-case a
         * toggle/mask here (unlike DMAC_STAT above) - it's a plain
         * store, matching real games' documented use (write 0 or
         * 0xFFFFFFFF wholesale, not per-bit twiddling). */
        g_dma.d_enable_state = val;
        return 1;
    }
    return 0;
}
