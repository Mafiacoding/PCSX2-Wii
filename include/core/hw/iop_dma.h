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
 * NOT modeled: actual transfer execution for any channel EXCEPT
 * channel 9 (SIF0, "fromIOP") as of Round 199 below, the
 * interrupt/exception side effects PCSX2's ICR write handler
 * triggers (iopIntcIrq/psxDmaInterrupt - no such wiring exists in
 * iop_core.c yet), and DMA_ICR's 16-bit "high half" write variant
 * (0x1F8010F6) since this project only models 32-bit hardware
 * register access elsewhere too.
 *
 * Round 199 (task #367) update: channel 9 (SIF0, "fromIOP") is now a
 * PARTIAL exception to the "no actual transfer execution" scope note
 * above - see the doc comment on iop_dma_mmio_write32()'s CHCR case
 * in iop_dma.c for the full citation trail (psx-spx's real, cited
 * CHCR/BCR bit layout) and honest gaps (no DREQ/handshake timing
 * modeled, CHCR bit 0's direction value not asserted for this
 * channel specifically since no citable IOP-specific source fixes
 * it, SIF1/channel 10 - the reverse "toIOP" direction - remains
 * entirely unmodeled pending a symmetric EE-side "write into IOP
 * RAM" capability this round did not build). Every other channel
 * remains exactly as this file originally documented: register latch
 * only.
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

/*
 * Round 199 (task #367): binds the IOP's own 2MB RAM so
 * iop_dma_mmio_write32()'s CHCR handler can actually read real
 * source bytes for a SIF0 (channel 9, "fromIOP" per psx-spx's real,
 * cited channel table cross-referenced with this project's own
 * dma.h) send, instead of only latching the register value. Mirrors
 * this project's existing dma_bind_ee_ram() (core/hw/dma.h) exactly -
 * same one-time wiring done by iop_core_init() right after IOP RAM is
 * allocated.
 */
void iop_dma_bind_iop_ram(uint8_t *ram, uint32_t ram_size);


int iop_dma_mmio_read32(uint32_t addr, uint32_t *out);
int iop_dma_mmio_write32(uint32_t addr, uint32_t value);

iop_dma_state_t *iop_dma_get_state(void);

/* Round 114 (task #172/#269/#270): real per-channel DMA-completion
 * signal - the counterpart, on the IOP DMA controller side, of this
 * project's already-existing EE-side dma_channel_signal_done()
 * (core/hw/dma.h). Sets the real, already-documented "per-channel
 * pending-IRQ flag" bit (this file's own icr_write()'s bits 24-30,
 * cited from PCSX2's IopHwWrite.cpp) for `channel` in whichever real
 * register owns it (DMA_ICR for channels 0-6, DMA_ICR2 for channels
 * 7-12 - see this header's channel table above), and, if that
 * channel's real per-channel enable bit (bits 16-22, same citation)
 * AND the real master-enable bit (DMA_ICR bit 23 - per Round 113's
 * fetched intrman.c, EnableIntr always sets THIS bit, in DMA_ICR
 * specifically, regardless of which controller's channel is being
 * enabled, so it is the one real master gate checked here for both
 * ranges) are both set, raises the real IOP_IRQ_DMA hardware line
 * (iop_intc_raise(3)) and Round 112's soft-dispatch simplification
 * (iop_intc_raise_soft()) for the corresponding real irq number.
 *
 * Honest scope note: this project's own icr_write() (PCSX2-cited,
 * already tested in tests/test_iop_dma.c) and the real, fetched
 * ps2sdk intrman.c EnableIntr/DisableIntr (Round 113) don't fully
 * agree on the exact real meaning of every low-order bit in these
 * registers - intrman.c's own EnableIntr additionally touches a
 * different bit range (documented in iop_hle_intr.c) whose precise
 * real hardware purpose isn't confirmed by either fetched source.
 * This function deliberately uses ONLY the higher-confidence, better-
 * corroborated bits (16-22 enable / 23 master / 24-30 flag) both
 * fetched sources independently agree on for the enable-bit position
 * specifically, rather than asserting a single unified theory of the
 * whole register this project doesn't have full evidence for. */
void iop_dma_signal_channel_done(int channel);

/*
 * Round 206 (task #366): generic "device writes bytes INTO IOP RAM at
 * a channel's current MADR" primitive - the missing counterpart to
 * iop_dma_sif0_try_transfer()'s "read bytes FROM IOP RAM" direction,
 * needed by iop_cdvd.c's new real N-command ReadCd/ReadDvd
 * implementation (channel 3, CDROM - see this header's own channel
 * table above, and ps2tek's "CDVD Reads and Seeks" page, directly
 * cited in iop_cdvd.c: "the CDVD DMA channel can store the data in
 * memory"). Generic rather than CDVD-specific since any future
 * "device -> IOP RAM" channel (MDEC out, SPU, etc.) would need the
 * identical primitive - same reasoning as dma_channel_receive_
 * quadwords() being generic on the EE side.
 *
 * Writes `nbytes` bytes from `data` into IOP RAM starting at
 * ch[channel].madr, then advances that channel's MADR by `nbytes`
 * (matching real hardware's own auto-incrementing address behavior
 * during a burst transfer - the same convention this project's own
 * EE-side dma.c already documents for its channels). Returns 1 on
 * success, 0 if IOP RAM isn't bound yet (iop_dma_bind_iop_ram()) or
 * the write would run past the end of IOP RAM (safe no-op, matches
 * iop_dma_sif0_try_transfer()'s existing out-of-bounds handling).
 * Does NOT touch CHCR/STR or raise any IRQ - callers (iop_cdvd.c)
 * handle their own real per-command completion/IRQ semantics, since
 * those are channel/command-specific, not part of this generic byte-
 * mover primitive.
 */
int iop_dma_channel_write_bytes(int channel, const uint8_t *data, uint32_t nbytes);

/*
 * Round 511 (task #470): counts real, completed IOP-RAM-to-EE-RAM SIF2
 * transfers (channel 2, real "GPU"/SIF2 dual-purpose channel per the
 * user's own uploaded real sifman.c/dmacman.h source - see
 * iop_dma_sif2_try_transfer()'s doc comment in iop_dma.c for the full
 * citation). Same hit-counter diagnostic convention already used
 * throughout this project (dispatch_ncmd() count,
 * iop_sio2_get_pad_command_count(), etc.) - lets a boot survey
 * empirically confirm whether real guest IOP code ever actually
 * issues this kick, independent of whether the transfer function
 * itself is correct.
 */
uint32_t iop_dma_get_sif2_transfer_count(void);

/* Round 712 (task #683): counts real, completed IOP-RAM-to-SPU2-RAM
 * channel-7 transfers, mirroring iop_dma_get_sif2_transfer_count()'s
 * own diagnostic precedent. */
uint32_t iop_dma_get_spu2_transfer_count(void);

#endif
