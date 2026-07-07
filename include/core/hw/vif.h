#ifndef PCSX2WII_VIF_H
#define PCSX2WII_VIF_H

#include <stdint.h>
#include "core/hw/dma.h"

/*
 * VIF0/VIF1 (Vector Interface) - first, deliberately narrow increment.
 *
 * Real hardware: VIF0 feeds VU0 (as a DMA front-end, distinct from
 * VU0's other life as EE COP2 "macro mode" - see ee_core.c's COP2
 * handling), VIF1 feeds VU1 and can also forward raw GS packet data
 * straight through to the GIF (the DIRECT/DIRECTHL commands - the
 * single most common way real BIOS/game code gets a GS packet to the
 * screen without going through the direct EE->GIF DMA path this
 * project already had). Reference: PCSX2's `Vif.h` (register layout,
 * STAT/FBRST/ERR bitfields), `Vif_Codes.cpp` (the VIFcode command
 * dispatch table this file's CMD constants and semantics are ported
 * from - NOT guessed).
 *
 * A VIF DMA transfer is a stream of 32-bit "VIFcode" words
 * interspersed with data - NOT the 128-bit-tag-plus-PACKED-rows
 * format GIF uses. Each VIFcode word: bits 0-15 = IMM, bits 16-23 =
 * NUM, bits 24-30 = CMD (7 bits), bit 31 = I (interrupt request,
 * not modeled - no VIF interrupt delivery exists in this project).
 *
 * Implemented this round (see vif.c for the full per-command table):
 *   NOP, STCYCL, OFFSET/BASE (VIF1 only), ITOP, STMOD, MARK - trivial
 *   register stores, no real behavioral effect since nothing in this
 *   project reads them back yet (they exist so real VIF code streams
 *   parse without desyncing, and so a later round has somewhere to
 *   read them from).
 *   FLUSHE/FLUSH(VIF1)/FLUSHA(VIF1), MSCAL/MSCNT/MSCALF - real
 *   no-ops here: on real hardware these synchronize with the VU
 *   microcode engine, which this project does not have (see
 *   docs/ROADMAP.md section 5, "VU0/VU1 register file + micro-
 *   instruction memory" and "VU microcode interpreter" are both
 *   still open) - correctly modeled as "nothing to wait for".
 *   STMASK/STROW/STCOL - store their trailing data word(s) into
 *   mask/row[4]/col[4], matching real register semantics, even
 *   though nothing consumes them yet (UNPACK, which would, is out of
 *   scope this round - see below).
 *   MPG - recognized and its data span correctly skipped (so the
 *   VIFcode stream doesn't desync), but the microprogram bytes
 *   themselves go nowhere - there is no VU micro-instruction memory
 *   to write them into yet. Counted as unsupported.
 *   DIRECT/DIRECTHL (VIF1 only) - THE one command this round actually
 *   produces pixels: forwards its data span verbatim to
 *   `gif_process_quadwords()`, exactly the real hardware behavior
 *   (real PCSX2's `_vifCode_Direct` does the same handoff to its GIF
 *   unit). DIRECT vs DIRECTHL differ on real hardware only in a GS
 *   FIFO "horizontal" transfer-path nuance irrelevant to a software
 *   model with no FIFO - both are treated identically here, which is
 *   what PCSX2 itself effectively does too (shared implementation).
 *
 * Explicitly NOT implemented: UNPACK (CMD 0x60-0x7F) - the format
 * that decodes S/V2/V3/V4 data (32/16/8/5-bit component variants,
 * with STCYCL-controlled skip-write cycles and STMASK/STROW/STCOL-
 * based masking) into VU data memory. This is a substantial format in
 * its own right (`Vif_Unpack.cpp`) and this project has no VU data
 * memory to unpack INTO for VU1 (VU0 has some vector-datapath state
 * from round 13, but VU1 has nothing yet). Encountering an UNPACK
 * code (or any other unrecognized/reserved code) stops processing
 * the REST of that DMA transfer's data stream cleanly - counted via
 * unsupported_cmds_seen, not silently misparsed as garbage VIFcodes -
 * matching this project's established pattern for out-of-scope
 * formats (see gif.c's REGLIST/IMAGE handling).
 */

typedef struct {
    int is_vif1; /* 0 = VIF0, 1 = VIF1 - some commands (OFFSET/BASE/
                  * FLUSH/FLUSHA/MSKPATH3/DIRECT/DIRECTHL) only exist
                  * on real VIF1; issuing them to VIF0 is counted as
                  * unsupported, matching real hardware routing them
                  * to an error/null handler. */

    uint32_t code;              /* last VIFcode word processed */
    uint8_t  cycle_cl, cycle_wl; /* STCYCL */
    uint32_t mode;               /* STMOD */
    uint32_t mark;                /* MARK */
    uint32_t mask;                /* STMASK */
    uint32_t row[4], col[4];      /* STROW/STCOL */
    uint32_t itop, itops;          /* ITOP */
    uint32_t base, ofst, tops;      /* OFFSET/BASE - VIF1 only on real hardware */

    uint64_t codes_processed;
    uint64_t direct_qwords_forwarded; /* via DIRECT/DIRECTHL */
    uint64_t unsupported_cmds_seen;    /* UNPACK, MPG, or a VIF1-only cmd issued to VIF0 */
} vif_state_t;

void vif_init(void);
vif_state_t *vif0_get_state(void);
vif_state_t *vif1_get_state(void);

/* Matches dma_sink_fn - register with
 * dma_set_sink(DMA_CHANNEL_VIF0, vif0_process_quadwords) and
 * dma_set_sink(DMA_CHANNEL_VIF1, vif1_process_quadwords). */
void vif0_process_quadwords(int channel, const uint8_t *data, uint32_t qwc);
void vif1_process_quadwords(int channel, const uint8_t *data, uint32_t qwc);

#endif
