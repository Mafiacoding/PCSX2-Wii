/*
 * iop_dma.h - IOP DMA controller register block
 *
 * The IOP has its own, entirely separate DMA controller from the EE's
 * (core/hw/dma.h models the EE's 10-channel controller; this is a
 * different piece of hardware with a different register layout and a
 * different channel count/purpose). Real IOP DMA channels move data
 * between IOP RAM and MDEC, GPU (PS1 legacy), CDROM, SPU/SPU2, DEV9,
 * SIF0/SIF1 (the OTHER end of the SIF connection this project's
 * dma.c/sif.c already model from the EE side), and SIO2.
 *
 * Scope: this is a REGISTER STUB, matching how this project's EE DMA
 * model (core/hw/dma.h) started before its chain-mode transfer engine
 * was added - reads/writes are latched faithfully and addresses are
 * decoded against the real PS2 memory map, but writing CHCR does NOT
 * trigger an actual transfer. Real PCSX2 (ps2/Iop/IopHwWrite.cpp)
 * calls a per-channel transfer function (psxDma0/DmaExec/DmaExec2 -
 * MDEC/GPU/CDROM-specific device logic) immediately after latching
 * CHCR; none of that device-specific transfer logic is modeled here.
 *
 * Register semantics ported from real PCSX2 source (pcsx2/IopHw.h for
 * addresses, ps2/Iop/IopHwWrite.cpp for the one piece of write-side
 * logic that IS modeled here - DMA_ICR/DMA_ICR2's bit manipulation):
 *
 * Per-channel registers (MADR/BCR/CHCR/TADR, +0x00/+0x04/+0x08/+0x0C
 * from each channel's base address below) are plain read/write
 * storage - no special casing, matching CHCR's real behavior of
 * "just latch the value" before the (unmodeled) transfer call:
 *
 *   ch 0 (MDEC in)   base 0x1F801080
 *   ch 1 (MDEC out)  base 0x1F801090
 *   ch 2 (GPU)       base 0x1F8010A0
 *   ch 3 (CDROM)     base 0x1F8010B0
 *   ch 4 (SPU)       base 0x1F8010C0
 *   ch 6 (OTC)       base 0x1F8010E0   (channel 5 does not exist on
 *                                       real hardware - intentional
 *                                       gap, not a typo)
 *   ch 7 (SPU2)      base 0x1F801500
 *   ch 8 (DEV9)      base 0x1F801510
 *   ch 9 (SIF0)      base 0x1F801520
 *   ch 10 (SIF1)     base 0x1F801530
 *   ch 11 (SIO2 in)  base 0x1F801540
 *   ch 12 (SIO2 out) base 0x1F801550
 *
 * Simplification: real hardware doesn't implement all four of MADR/
 * BCR/CHCR/TADR identically on every channel (some channels lack a
 * TADR register in practice); this model gives every channel all
 * four slots uniformly as harmless plain storage rather than
 * special-casing which channels "really" have TADR, the same kind of
 * simplification gs_mem.c makes for GS memory addressing (see its
 * header comment) - documented here rather than silently assumed.
 *
 * Global registers:
 *   0x1F8010F0 DMA_PCR  - per-channel priority/enable control. Plain
 *                          read/write (PCSX2 has no special-case
 *                          write handler for this address at all).
 *   0x1F8010F4 DMA_ICR  - per-channel completion IRQ enable/flag
 *                          register, and the one place in this file
 *                          with real (not just latched) write
 *                          behavior - see iop_dma.c for the exact
 *                          bit-level port of PCSX2's handler.
 *   0x1F801570 DMA_PCR2 - same idea as DMA_PCR, for channels 7-12.
 *   0x1F801574 DMA_ICR2 - same idea as DMA_ICR, for channels 7-12.
 *
 * NOT modeled: actual transfer execution for any channel, the
 * interrupt/exception side effects PCSX2's ICR write handler
 * triggers (iopIntcIrq/psxDmaInterrupt - no such wiring exists in
 * iop_core.c yet), and DMA_ICR's 16-bit "high half" write variant
 * (0x1F8010F6) since this project only models 32-bit hardware
 * register access elsewhere too.
 */
#ifndef PCSX2_WII_IOP_DMA_H
#define PCSX2_WII_IOP_DMA_H

#include <stdint.h>

#define IOP_DMA_NUM_CHANNELS 13 /* indices 0-12; index 5 is unused/reserved */

typedef struct {
    uint32_t madr;
    uint32_t bcr;
    uint32_t chcr;
    uint32_t tadr;
} iop_dma_channel_t;

typedef struct {
    iop_dma_channel_t ch[IOP_DMA_NUM_CHANNELS];
    uint32_t pcr;
    uint32_t icr;
    uint32_t pcr2;
    uint32_t icr2;
} iop_dma_state_t;

void iop_dma_init(void);

int iop_dma_mmio_read32(uint32_t addr, uint32_t *out);
int iop_dma_mmio_write32(uint32_t addr, uint32_t value);

iop_dma_state_t *iop_dma_get_state(void);

#endif
