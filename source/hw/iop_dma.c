/*
 * iop_dma.c - IOP DMA controller register block. See iop_dma.h for
 * scope, per-register semantics, and PCSX2 source cross-reference.
 */
#include "core/hw/iop_dma.h"
#include "core/hw/iop_intc.h" /* Round 114: iop_intc_raise/iop_intc_raise_soft */
#include <string.h>

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

void iop_dma_init(void)
{
    memset(&g_dma, 0, sizeof(g_dma));
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
        case 0x08:
            c->chcr = value;
            /* Real hardware/PCSX2 calls the channel's own transfer
             * function here (psxDma0/DmaExec/DmaExec2) - NOT modeled,
             * see the header comment. This is a register latch only. */
            return 1;
        case 0x0C: c->tadr = value; return 1;
        default:   return 1; /* unmodeled sub-offset: accepted, discarded */
    }
}
