#ifndef PCSX2WII_IPU_H
#define PCSX2WII_IPU_H

#include <stdint.h>

/*
 * ipu.h - Image Processing Unit (IPU) register/FIFO skeleton.
 *
 * Round 521/522 (task #487, the video-decoder arc the user explicitly
 * asked for after Round 520's audit found the IPU completely
 * unmodeled). Real hardware: the EE's own MPEG1/MPEG2 macroblock-
 * layer video decoder, used for FMV playback and some BIOS intro
 * content on real consoles - see docs/STATUS.md's Round 521 entry for
 * the full citation trail (ps2tek's IOP/EE memory map page + PCSX2's
 * own pcsx2/IPU/IPU.h and IPUdma.cpp, GPL-3.0, same citation practice
 * this project already uses for ee_core.c).
 *
 * Real register map (ps2tek, directly cited):
 *   0x10002000  IPU_CMD.DATA   (write: issues a command; read: either
 *                                the command's own status word, or -
 *                                if no command has ever been issued
 *                                yet - the raw first 32 bits of
 *                                whatever is sitting in the input
 *                                FIFO, a real documented quirk some
 *                                games rely on to peek at a header)
 *   0x10002004  IPU_CMD.BUSY   (bit 31 = command in progress)
 *   0x10002010  IPU_CTRL
 *   0x10002020  IPU_BP         (bit-stream pointer/status)
 *   0x10002030  TOP
 *   0x10002034  TOPBUSY
 *   0x10007000  Out FIFO (read, pulled by DMA_CHANNEL_FROMIPU)
 *   0x10007010  In FIFO  (write, pushed by DMA_CHANNEL_TOIPU)
 *
 * IPU_CTRL real bitfield (ported from PCSX2's tIPU_CTRL, IPU.h):
 *   IFC:4 (input FIFO count, 0-8) | OFC:4 (output FIFO count, 0-8) |
 *   CBP:6 (coded block pattern) | ECD:1 (error code detected) |
 *   SCD:1 (start code detected) | IDP:2 (intra DC precision) |
 *   resv:2 | AS:1 (alternate scan) | IVF:1 (intra VLC format) |
 *   QST:1 (Q scale step) | MP1:1 (MPEG1 bitstream) | PCT:3 (picture
 *   type) | resv:3 | RST:1 (reset) | BUSY:1.
 * Real writable-bits mask on write: 0x47f30000 (everything else is a
 * read-only status bit the IPU itself sets) - directly cited from
 * PCSX2's own tIPU_CTRL::write().
 *
 * Real command set (top 4 bits of the 32-bit value written to
 * IPU_CMD.DATA; low 28 bits are the command's own OPTION field):
 *   0=BCLR 1=IDEC 2=BDEC 3=VDEC 4=FDEC 5=SETIQ 6=SETVQ 7=CSC 8=PACK
 *   9=SETTH
 * (PCSX2's enum SCE_IPU, IPU.h.)
 *
 * SCOPE THIS ROUND (deliberately limited, matching this project's own
 * established "register protocol before real dispatch logic" pattern
 * - e.g. Round 206 built CDVD's N-command register block completely
 * before dispatch_ncmd() had any real per-command behavior, several
 * rounds before READCD actually delivered real sector data):
 *   - Real register read/write semantics for CMD/CTRL/BP/TOP/TOPBUSY,
 *     including the real CTRL writable-bits mask and RST behavior.
 *   - BCLR really clears the input FIFO and resets BP/IFC, matching
 *     real hardware.
 *   - Every other command (IDEC/BDEC/VDEC/FDEC/SETIQ/SETVQ/CSC/PACK/
 *     SETTH) is accepted and acknowledged (ECD/SCD cleared per real
 *     spec, BUSY set then immediately cleared) but does NOT perform
 *     real MPEG2 macroblock decode, IDCT, motion compensation, or
 *     colorspace conversion yet - honestly a no-op beyond the real
 *     register-level bookkeeping. That real decode logic is deferred
 *     to a later, evidence-driven round once something in the actual
 *     boot/game trace exercises the IPU and there's a concrete real
 *     bitstream to test against (see docs/STATUS.md Round 521's scope
 *     decision).
 *   - The input FIFO is real (8 QWC deep, matching real hardware),
 *     fed by DMA_CHANNEL_TOIPU via dma_set_sink() exactly like GIF/
 *     VIF0/VIF1 already are. Consumed input data is currently
 *     discarded (no decoder to hand it to yet) rather than silently
 *     pretended-into something - IFC still tracks real fill level.
 *   - The output FIFO/OFC and DMA_CHANNEL_FROMIPU's real inbound-to-
 *     EE-RAM delivery path are NOT wired up yet, since nothing
 *     produces real output data at this stage - OFC reads 0 always,
 *     honestly reflecting "nothing decoded yet" rather than faking a
 *     nonzero count with garbage data behind it.
 */

void ipu_init(void);

/* Returns 1 and fills *out if addr (already KSEG0/1-masked by the
 * caller, same convention as dma_mmio_read32/ee_timers_mmio_read32)
 * is one of IPU_CMD.DATA/BUSY, IPU_CTRL, IPU_BP, TOP, or TOPBUSY.
 * 0 otherwise. */
int ipu_mmio_read32(uint32_t addr, uint32_t *out);
int ipu_mmio_write32(uint32_t addr, uint32_t value);

/* dma_set_sink() target for DMA_CHANNEL_TOIPU - real signature
 * matching gif_process_quadwords()/vif0_process_quadwords() etc.
 * Pushes up to 8 QWC into the real input FIFO (IFC), discarding
 * anything beyond that depth (matching real hardware's fixed FIFO
 * size - a genuinely full FIFO stalls the DMA on real hardware; this
 * project doesn't yet model that stall, an honest, documented
 * simplification since no real decode consumer exists yet to drain
 * it either). */
void ipu_process_quadwords(int channel, const uint8_t *data, uint32_t qwc);

#endif
