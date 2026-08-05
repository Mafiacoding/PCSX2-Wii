/*
 * iop_dma.c - IOP DMA controller register block. See iop_dma.h for
 * scope, per-register semantics, and PCSX2 source cross-reference.
 */
#include "core/hw/iop_dma.h"
#include "core/hw/iop_intc.h" /* Round 114: iop_intc_raise/iop_intc_raise_soft */
#include <string.h>
#include "core/hw/dma.h" /* Round 199: dma_channel_receive_quadwords() - the EE-side inbound-write primitive Round 198 built */

#define DMA_PCR   0x1F8010F0u
#define DMA_ICR   0x1F8010F4u
#define DMA_PCR2  0x1F801570u
#define DMA_ICR2  0x1F801574u

typedef struct {
    uint32_t base;
    int      channel;
} iop_dma_range_t;

/* Real PS2 IOP DMA channel base addresses (pcsx2/IopHw.h). Channel 5
 * genuinely does not exist on real hardware - intentional gap. */
static const iop_dma_range_t s_ranges[] = {
    { 0x1F801080u, 0 },  /* MDEC in  */
    { 0x1F801090u, 1 },  /* MDEC out */
    { 0x1F8010A0u, 2 },  /* GPU      */
    { 0x1F8010B0u, 3 },  /* CDROM    */
    { 0x1F8010C0u, 4 },  /* SPU      */
    { 0x1F8010E0u, 6 },  /* OTC      */
    { 0x1F801500u, 7 },  /* SPU2     */
    { 0x1F801510u, 8 },  /* DEV9     */
    { 0x1F801520u, 9 },  /* SIF0     */
    { 0x1F801530u, 10 }, /* SIF1     */
    { 0x1F801540u, 11 }, /* SIO2 in  */
    { 0x1F801550u, 12 }, /* SIO2 out */
};
#define NUM_RANGES (int)(sizeof(s_ranges) / sizeof(s_ranges[0]))

static iop_dma_state_t g_dma;
static uint32_t g_sif2_transfer_count; /* Round 511 diagnostic: counts real, completed IOP-RAM-to-EE-RAM SIF2 transfers */

void iop_dma_init(void)
{
    memset(&g_dma, 0, sizeof(g_dma));
}

static uint8_t *g_iop_ram = NULL;
static uint32_t g_iop_ram_size = 0;

void iop_dma_bind_iop_ram(uint8_t *ram, uint32_t ram_size)
{
    g_iop_ram = ram;
    g_iop_ram_size = ram_size;
}

#define IOP_DMA_SIF0_CHANNEL 9
#define IOP_DMA_SIF2_CHANNEL 2

/*
 * Round 199 (task #367): SIF0 (channel 9, real "fromIOP" direction -
 * psx-spx's DMA Channels page, cross-referenced with this project's
 * own dma.h SIF0 doc comment) is the one channel where writing CHCR
 * with the real, cited STR/"start" bit (bit 24 - psx-spx's own CHCR
 * bit table, already used by this project's icr_write()/DMAC_STAT
 * logic elsewhere) now actually MOVES bytes, instead of only
 * latching the register - closing exactly the gap Round 197's 237th
 * finding root-caused (dma.c had a receive-side primitive with no
 * IOP-side sender ever calling it).
 *
 * Real, cited semantics used here (psx-spx DMA Channels page):
 *   - BCR (Dn_BCR): "BC/BS/BA can be in range 0001h..FFFFh (or 0=
 *     10000h)" - SyncMode 1 gives total length = BS (bits 0-15) * BA
 *     (bits 16-31) words; SyncMode 0 gives a plain BC word count in
 *     bits 0-15 with bits 16-31 unused. This project doesn't have a
 *     citable source for which SyncMode real IOP SIF0 hardware always
 *     uses, so BOTH real conventions are honored defensively: if the
 *     upper 16 bits (BA) are zero, the lower 16 bits are treated as a
 *     plain word count (SyncMode-0-style); otherwise BS*BA is used
 *     (SyncMode-1-style). Either way, the result is a real, in-range
 *     word count - never an invented number.
 *   - CHCR bit 24 (STR/start) is the only bit gated on here. CHCR bit
 *     0 (real direction bit) is deliberately NOT asserted to any
 *     specific fixed value for this channel, since no citable IOP-
 *     specific source fixes it - this project doesn't guess.
 *
 * Honest scope, not fabricated: no DREQ/handshake timing is modeled
 * (the transfer happens synchronously and instantly on this single
 * MMIO write, same simplification this project's EE-side
 * dma_channel_kick() already makes for its own channels). Data lands
 * at the EE's SIF0 channel's CURRENT MADR - i.e. this assumes the EE
 * side has already programmed its own SIF0 destination address first,
 * matching real usage order (EE arms its receive channel, then the
 * IOP sends) but not independently re-verified against a citable
 * source for this exact ordering guarantee. SIF1 (channel 10, the
 * reverse "toIOP" direction) remains completely unmodeled - it would
 * need a symmetric "write into IOP RAM" primitive this round did not
 * build; noted as open scope, not silently skipped.
 */
static void iop_dma_sif0_try_transfer(iop_dma_channel_t *ch)
{
    if (!g_iop_ram)
        return; /* iop_dma_bind_iop_ram() never called - safe no-op, mirrors dma_channel_receive_quadwords()'s own no-RAM-bound safety check */

    uint32_t bs = ch->bcr & 0xFFFFu;
    uint32_t ba = (ch->bcr >> 16) & 0xFFFFu;
    if (bs == 0) bs = 0x10000u; /* real "0 means 10000h" convention */
    uint32_t total_words = (ba == 0) ? bs : (bs * ba);

    uint32_t total_bytes = total_words * 4u;
    uint32_t qwc = total_bytes / 16u; /* SIF transfers are quadword-granular in real practice */
    if (qwc == 0) {
        ch->chcr &= ~0x01000000u; /* nothing to move, but still clear STR - matches "auto-cleared upon completion" even for a degenerate 0-length kick */
        return;
    }

    if ((uint64_t)ch->madr + (uint64_t)(qwc * 16u) > (uint64_t)g_iop_ram_size) {
        ch->chcr &= ~0x01000000u; /* out-of-bounds source: clear STR, drop the transfer rather than reading past IOP RAM */
        return;
    }

    dma_channel_receive_quadwords(DMA_CHANNEL_SIF0, g_iop_ram + ch->madr, qwc);
    ch->chcr &= ~0x01000000u; /* real hardware auto-clears STR upon completion */
    iop_dma_signal_channel_done(IOP_DMA_SIF0_CHANNEL); /* real per-channel completion IRQ path, already existing (Round 114) */
}

/*
 * Round 511 (task #470, following directly from Round 510's diagnosis
 * that the shared kernel idle loop this project has documented since
 * Rounds 265-271 waits on DMAC_STAT bit 0x80 = SIF2 completion OR
 * INTC_STAT bit 0x2 = SBUS, and that "SIF2's DPCR enable bit is set...
 * but dma_channel_kick() is never called for it" - Round 267's
 * finding, re-confirmed as recently as Round 469's investigation):
 * IOP DMA channel 2 (real hardware/PCSX2 base 0x1F8010A0) is
 * PS1-legacy "GPU" repurposed as the real, both-directions SIF2
 * channel on PS2 hardware - confirmed via the user's own uploaded,
 * real 2002-era IOP SIFMAN reimplementation source
 * (dmacman.h: "#define DMAch_GPU 2 // SIF2 both directions",
 * "#define DMAch_SIF2_MADR (*(volatile int*)0xBF8010A0)" etc. -
 * exactly this project's own existing channel-2/0x1F8010A0 mapping,
 * already present in s_ranges[] below, just never given a real
 * transfer function). The same source's sifman_call18_sceSifSetSIF2DMA()
 * (sifman.c) is the real kicker: sets MADR to the source buffer
 * (masked to 0xFFFFFF, i.e. within the IOP's own address space),
 * BCR_size/BCR_count (low/high halfwords of BCR, same layout this
 * project's existing iop_dma_sif0_try_transfer() below already uses
 * for SIF0's BS*BA total-word-count convention - independently
 * confirmed matching, not assumed), then writes CHCR with DMAf_TR
 * (0x01000000, the same real STR/start bit SIF0 already gates on)
 * OR'd with DMAf_DR (0x00000001) when-and-only-when the caller's own
 * `attr` parameter requested SIF_TO_EE - i.e. the DR bit is the real,
 * cited signal that this specific kick moves data IOP RAM -> EE RAM
 * (the exact direction this project's existing
 * dma_channel_receive_quadwords() inbound-write primitive, Round 198,
 * already implements and already wires up for SIF0 above - this
 * function is a direct sibling, not new architecture).
 *
 * Honest scope: only the IOP-RAM-to-EE-RAM (DR=1/"to EE") direction
 * is implemented, mirroring Round 199's own documented SIF0/SIF1
 * asymmetry (SIF1's reverse direction was left as open scope, not
 * fabricated). A CHCR write with TR set but DR clear (the real
 * EE-to-IOP direction sceSifSetSIF2DMA() also supports per this same
 * source) is intentionally left as a plain register latch - this
 * project has no citable source yet for what IOP-side buffer such a
 * transfer should land in, so nothing is invented for it.
 */
static void iop_dma_sif2_try_transfer(iop_dma_channel_t *ch)
{
    if (!g_iop_ram)
        return; /* iop_dma_bind_iop_ram() never called - same safety check as SIF0 above */

    uint32_t bs = ch->bcr & 0xFFFFu;         /* real BCR_size, low halfword (0xBF8010A4) */
    uint32_t ba = (ch->bcr >> 16) & 0xFFFFu; /* real BCR_count, high halfword (0xBF8010A6) */
    if (bs == 0) bs = 0x10000u; /* same real "0 means 10000h" convention as SIF0 */
    uint32_t total_words = (ba == 0) ? bs : (bs * ba);

    uint32_t total_bytes = total_words * 4u;
    uint32_t qwc = total_bytes / 16u;
    if (qwc == 0) {
        ch->chcr &= ~0x01000000u; /* nothing to move - still clear STR, matches SIF0's degenerate case */
        return;
    }

    /* Real sceSifSetSIF2DMA() masks its own MADR write to 0xFFFFFF
     * (24-bit IOP-local address space) before this project's own
     * iop_dma_mmio_write32() ever stores it - ch->madr already holds
     * that masked value by the time this function runs, so no
     * additional masking is needed here (mirrors how SIF0's own
     * madr is used as-is below). */
    if ((uint64_t)ch->madr + (uint64_t)(qwc * 16u) > (uint64_t)g_iop_ram_size) {
        ch->chcr &= ~0x01000000u; /* out-of-bounds source: drop rather than read past IOP RAM */
        return;
    }

    dma_channel_receive_quadwords(DMA_CHANNEL_SIF2, g_iop_ram + ch->madr, qwc);
    ch->chcr &= ~0x01000000u; /* real hardware auto-clears STR upon completion */
    iop_dma_signal_channel_done(IOP_DMA_SIF2_CHANNEL); /* real per-channel completion IRQ path, same as SIF0 */
    g_sif2_transfer_count++; /* Round 511 diagnostic */
}

uint32_t iop_dma_get_sif2_transfer_count(void)
{
    return g_sif2_transfer_count;
}

int iop_dma_channel_write_bytes(int channel, const uint8_t *data, uint32_t nbytes)
{
    if (!g_iop_ram)
        return 0; /* iop_dma_bind_iop_ram() never called */
    if (channel < 0 || channel >= IOP_DMA_NUM_CHANNELS)
        return 0;
    iop_dma_channel_t *ch = &g_dma.ch[channel];
    if ((uint64_t)ch->madr + (uint64_t)nbytes > (uint64_t)g_iop_ram_size)
        return 0; /* out-of-bounds destination: drop rather than write past IOP RAM */
    memcpy(g_iop_ram + ch->madr, data, nbytes);
    ch->madr += nbytes;
    return 1;
}

iop_dma_state_t *iop_dma_get_state(void) { return &g_dma; }

/* Real PCSX2 (ps2/Iop/IopHwWrite.cpp, HW_DMA_ICR/HW_DMA_ICR2 write
 * cases): a general "write to acknowledge, then recompute the master
 * flag" pattern shared by ICR and ICR2. Bits 0-23 of the write value
 * plainly overwrite bits 0-23 of the register (force-IRQ bit 15,
 * per-channel enable bits 16-22, master-enable bit 23). Bits 24-30
 * (per-channel pending-IRQ flags) are write-1-to-clear: a 1 bit in
 * the WRITE value clears the corresponding flag bit, rather than
 * setting it. Bit 31 (master IRQ flag) is then recomputed: it's set
 * if the force bit (15) is set, OR if the master-enable bit (23) is
 * set AND at least one channel has both its enable bit (16-22) and
 * its flag bit (24-30, shifted down to line up with the enable bits)
 * set. This mirrors real PCSX2's:
 *   u32 newtmp = (old & 0xff000000) | (val & 0xffffff);
 *   newtmp &= ~(val & 0x7f000000);
 *   if ((newtmp>>15)&1 || ((newtmp>>23)&1 && (((newtmp&0x7f000000)>>8) & (newtmp&0x7f0000))))
 *       newtmp |= 0x80000000;
 *   else
 *       newtmp &= ~0x80000000;
 */
static uint32_t icr_write(uint32_t old_val, uint32_t val)
{
    uint32_t newv = (old_val & 0xFF000000u) | (val & 0x00FFFFFFu);
    newv &= ~(val & 0x7F000000u);

    uint32_t force_bit    = (newv >> 15) & 0x1u;
    uint32_t master_en    = (newv >> 23) & 0x1u;
    uint32_t flags_at_16  = (newv & 0x7F000000u) >> 8;  /* bits 24-30 -> 16-22 */
    uint32_t enables_at16 = newv & 0x007F0000u;          /* bits 16-22 */

    if (force_bit || (master_en && (flags_at_16 & enables_at16) != 0))
        newv |= 0x80000000u;
    else
        newv &= ~0x80000000u;

    return newv;
}

static iop_dma_channel_t *find_channel(uint32_t addr, uint32_t *sub_off_out)
{
    for (int i = 0; i < NUM_RANGES; i++) {
        uint32_t base = s_ranges[i].base;
        if (addr >= base && addr < base + 0x10u) {
            *sub_off_out = addr - base;
            return &g_dma.ch[s_ranges[i].channel];
        }
    }
    return NULL;
}

int iop_dma_mmio_read32(uint32_t addr, uint32_t *out)
{
    switch (addr) {
        case DMA_PCR:  *out = g_dma.pcr;  return 1;
        case DMA_ICR:  *out = g_dma.icr;  return 1;
        case DMA_PCR2: *out = g_dma.pcr2; return 1;
        case DMA_ICR2: *out = g_dma.icr2; return 1;
        default: break;
    }

    uint32_t sub_off;
    iop_dma_channel_t *c = find_channel(addr, &sub_off);
    if (!c)
        return 0;

    switch (sub_off) {
        case 0x00: *out = c->madr; return 1;
        case 0x04: *out = c->bcr;  return 1;
        case 0x08: *out = c->chcr; return 1;
        case 0x0C: *out = c->tadr; return 1;
        default:   *out = 0;       return 1; /* unmodeled sub-offset within the 0x10 block */
    }
}

/* Round 114 - see iop_dma.h's own doc comment for the full citation
 * and honesty caveats. */
void iop_dma_signal_channel_done(int channel)
{
    if (channel < 0 || channel == 5 || channel > 12)
        return;

    uint32_t local_ch;
    uint32_t *icr_reg;
    uint32_t irq_index;

    if (channel <= 6) {
        local_ch  = (uint32_t)channel;
        icr_reg   = &g_dma.icr;
        irq_index = 0x20u + (uint32_t)channel;
    } else {
        local_ch  = (uint32_t)(channel - 7);
        icr_reg   = &g_dma.icr2;
        irq_index = 0x28u + (uint32_t)(channel - 7);
    }

    uint32_t flag_bit   = 1u << (local_ch + 24u);
    uint32_t enable_bit = 1u << (local_ch + 16u);

    *icr_reg |= flag_bit; /* real per-channel pending-IRQ flag */

    /* Real master-enable gate: per Round 113's fetched intrman.c,
     * EnableIntr always sets DMA_ICR (not DMA_ICR2) bit 23 regardless
     * of which controller's channel is being enabled - so g_dma.icr's
     * bit 23 is the one real master gate for both ranges here. */
    if ((g_dma.icr & 0x800000u) && (*icr_reg & enable_bit)) {
        iop_intc_raise(3);              /* real IOP_IRQ_DMA hardware line */
        iop_intc_raise_soft(irq_index); /* Round 112 soft-dispatch simplification */
    }
}

int iop_dma_mmio_write32(uint32_t addr, uint32_t value)
{
    switch (addr) {
        case DMA_PCR:  g_dma.pcr  = value; return 1; /* plain - no special write handler on real hardware/PCSX2 */
        case DMA_ICR:  g_dma.icr  = icr_write(g_dma.icr,  value); return 1;
        case DMA_PCR2: g_dma.pcr2 = value; return 1;
        case DMA_ICR2: g_dma.icr2 = icr_write(g_dma.icr2, value); return 1;
        default: break;
    }

    uint32_t sub_off;
    iop_dma_channel_t *c = find_channel(addr, &sub_off);
    if (!c)
        return 0;

    switch (sub_off) {
        case 0x00: c->madr = value; return 1;
        case 0x04: c->bcr  = value; return 1;
        case 0x08: {
            c->chcr = value;
            /* Real hardware/PCSX2 calls the channel's own transfer
             * function here (psxDma0/DmaExec/DmaExec2). Round 199:
             * for SIF0 (channel 9) specifically, if the real STR/
             * start bit (24) is set, this now actually performs the
             * transfer - see iop_dma_sif0_try_transfer()'s doc
             * comment for the full citation trail and honest scope.
             * Every other channel remains a register latch only, as
             * originally documented. */
            int this_channel = (int)(c - g_dma.ch);
            if (this_channel == IOP_DMA_SIF0_CHANNEL && (value & 0x01000000u)) {
                iop_dma_sif0_try_transfer(c);
            }
            /* Round 511 (task #470): channel 2 (real "GPU"/SIF2 dual-
             * purpose channel, s_ranges[] base 0x1F8010A0) - only the
             * real IOP-RAM-to-EE-RAM direction (DMAf_TR=0x01000000
             * AND DMAf_DR=0x00000001 both set, per the cited real
             * sceSifSetSIF2DMA() semantics in iop_dma_sif2_try_transfer()'s
             * own doc comment) triggers a real transfer. TR-without-DR
             * (the reverse, EE-to-IOP direction) is intentionally left
             * as a plain latch - honest unmodeled scope, not fabricated. */
            if (this_channel == IOP_DMA_SIF2_CHANNEL &&
                (value & 0x01000000u) && (value & 0x00000001u)) {
                iop_dma_sif2_try_transfer(c);
            }
            return 1;
        }
        case 0x0C: c->tadr = value; return 1;
        default:   return 1; /* unmodeled sub-offset: accepted, discarded */
    }
}
