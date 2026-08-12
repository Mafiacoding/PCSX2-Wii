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
 *   FLUSHE - a correct no-op (see below for why).
 *   MSCAL/MSCNT/MSCALF - now call into the real VU0/VU1 microcode
 *   interpreter (`include/core/hw/vu.h`, added alongside this round's
 *   VU0/VU1 micro-instruction memory work): this synchronously runs
 *   the microprogram until a real E-bit-flagged instruction retires
 *   (with the real one-more-instruction delay - see vu.h) or a safety
 *   cap is hit. MSCAL/MSCALF start at the VIFcode's IMM address
 *   (`vu0_exec_micro()`/`vu1_exec_micro()`); MSCNT resumes from the
 *   VU's own current TPC instead, ignoring IMM (`vu0_exec_micro_
 *   continue()`/`vu1_exec_micro_continue()`, Round 576/task #551) -
 *   ground-truthed against ps2sdk's packet2_utils_vu_add_continue_
 *   program(), the real standard way game code re-invokes an already-
 *   started VU1 program once per subsequent draw call. Before Round
 *   576 this project treated MSCNT identically to MSCAL, silently
 *   restarting from address 0 (a real MSCNT VIFcode's IMM field is
 *   unused/reserved, so it was always ~0) on every "continue" instead
 *   of resuming - see vu.c's citation for the full writeup. Because
 *   MSCAL/MSCNT/MSCALF now run synchronously to completion, FLUSHE
 *   never actually has anything left to wait for by the time it's
 *   reached - a correct no-op given that, not a shortcut. See vu.h for the important caveat that no
 *   real per-opcode VU instruction body is decoded yet (real control
 *   flow only) - this is a genuine, narrower step forward, not a full
 *   VU implementation.
 *   FLUSH(VIF1)/FLUSHA(VIF1) - real no-ops, same rationale as FLUSHE.
 *   STMASK/STROW/STCOL - store their trailing data word(s) into
 *   mask/row[4]/col[4], matching real register semantics, even
 *   though nothing consumes them yet (UNPACK, which would, is out of
 *   scope this round - see below).
 *   MPG - now writes its microprogram data for real into VU0/VU1
 *   micro-instruction memory (via `vu0_micro_write32()`/
 *   `vu1_micro_write32()`), at the destination address given by the
 *   VIFcode's IMM field (same "instruction pair index" addressing
 *   units as MSCAL/MSCNT - see vu.h). Previously (before this round's
 *   VU work) this data had nowhere to go and was just skipped;
 *   no longer counted as unsupported since it now does something
 *   real.
 *   DIRECT/DIRECTHL (VIF1 only) - THE one command this round actually
 *   produces pixels: forwards its data span verbatim to
 *   `gif_process_quadwords()`, exactly the real hardware behavior
 *   (real PCSX2's `_vifCode_Direct` does the same handoff to its GIF
 *   unit). DIRECT vs DIRECTHL differ on real hardware only in a GS
 *   FIFO "horizontal" transfer-path nuance irrelevant to a software
 *   model with no FIFO - both are treated identically here, which is
 *   what PCSX2 itself effectively does too (shared implementation).
 *
 * UNPACK (CMD 0x60-0x7F) - now implemented (this round). This decodes
 * S/V2/V3/V4 data (32/16/8-bit component variants, plus V4-5's packed
 * 16-bit format) into VU0/VU1 local DATA memory (`vu0_mem_write32()`/
 * `vu1_mem_write32()` - VU0's/VU1's data memory now both exist, see
 * `include/core/hw/vu.h` and `include/core/ee/ee_core.h`). Ported
 * directly from a live fetch of PCSX2's own `Vif_Unpack.cpp`/
 * `Vif_Unpack.h` (github.com/PCSX2/pcsx2, master) - NOT guessed - see
 * `vif.c`'s `vif_unpack()` for the full per-line citation trail. Real
 * behavior implemented:
 *   - CMD bits: bits 5-6 = 0b11 (signals UNPACK), bit 4 = M (mask
 *     enable), bits 0-3 = VN*4+VL (VN: 0=S,1=V2,2=V3,3=V4; VL:
 *     0=32-bit,1=16-bit,2=8-bit,3=5-bit, V4 only). Per-format source
 *     byte size from PCSX2's own `nVifT[16]` table, reproduced
 *     verbatim as `VIF_UNPACK_SIZE[16]` in vif.c.
 *   - IMM field reinterpreted for UNPACK: bits 0-9 = VU mem address
 *     (qwords), bit 14 = USN (0=signed/1=unsigned source components),
 *     bit 15 = FLG (VIF1 only - address relative to TOPS).
 *   - S: one component, broadcast to all 4 lanes. V2: two components,
 *     written X=v0,Y=v1,Z=v0,W=v1 (real hardware repeats the pair,
 *     confirmed from PCSX2's `UNPACK_V2`). V3: reuses the V4 read
 *     logic - the W lane reads one component-width PAST the real
 *     3-component data (typically into the next vector's first
 *     component) and gets overwritten by the next unpack - a real,
 *     cited hardware quirk games depend on (PCSX2's own comment names
 *     Ape Escape 3), not a bug here. V4-5: a single 16-bit read
 *     decoded via the exact real bit-shift formula from
 *     `UNPACK_V4_5`, MODE forced to 0 (real hardware: "V4_5 unpacks
 *     do not support the MODE register").
 *   - STCYCL-controlled CL/WL skip-write/fill-write cycles (real
 *     "isFill"/"skipSize" logic from `_nVifUnpackLoop`), STMASK/
 *     STROW/STCOL-based per-lane masking (Data/MaskRow/MaskCol/
 *     Write-Protect, real 2-bits-per-lane-per-cycle-position `mask`
 *     register layout from `writeXYZW`), and STMOD-driven row
 *     accumulate/chain modes (modes 0-3, real `writeXYZW` switch).
 *
 * Round 580 (task #536/#557): a partial UNPACK payload split across
 * multiple DMA calls is now handled for real, closing the gap flagged
 * above in earlier rounds. Ported from PCSX2's `Vif_Unpack.cpp`
 * (`nVifUnpack<idx>()`'s buffer-then-process pattern) and
 * `vifUnpackSetup<idx>()`'s closed-form upfront payload-size formula
 * (`nVifT[16]` gsize table x STCYCL CL/WL, skipping-write vs
 * filling-write cases) - see `vif.c`'s `vif_unpack_needed_bytes()`
 * and `vif_state_t`'s `unpack_pending`/`unpack_buffer` fields for the
 * full citation trail. This is the direct UNPACK-side counterpart of
 * Round 579's MPG cross-DMA-chunk continuation fix, found by
 * following the same "does this command have the identical gap"
 * question that MPG turned out to have.
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
    uint64_t unpack_vectors_written;   /* real vectors written to VU mem via UNPACK */
    uint64_t unsupported_cmds_seen;    /* reserved VN/VL combo, or a VIF1-only cmd issued to VIF0 */

    /* Round 578 (task #536/task #551 pivot): real diagnostic counters,
     * not hardware registers - count every real MPG (micro-instruction
     * upload) command processed and the total 32-bit words actually
     * written to VU micro-instruction memory as a result. Added to
     * directly answer "does any real microcode ever get uploaded to
     * VU1 before MSCAL/MSCNT/XGKICK are issued" without extra scratch
     * instrumentation - see tools/round578-vu1-diag/driver.c. */
    uint64_t mpg_calls;
    uint64_t mpg_words_written;

    /* Round 578b (task #536/#551 pivot): real diagnostic counters for
     * MSCAL/MSCALF/MSCNT dispatch - captures the byte address VU1
     * actually starts/resumes execution from on every call, plus the
     * MPG destination byte address of the LAST real micro-instruction
     * upload (VIF1 side only - this field is meaningless on a VIF0
     * struct instance). Added to directly answer whether real
     * uploaded microcode (found via mpg_words_written) and the real
     * MSCAL/MSCNT start/resume address ever land in the same place -
     * see tools/round578-vu1-diag/driver.c. */
    uint64_t mscal_calls;
    uint32_t mscal_last_start_byte;   /* imm*8 of the most recent MSCAL/MSCALF */
    uint64_t mscnt_calls;
    uint32_t mscnt_last_resume_byte;  /* VU1 tpc value BEFORE the most recent MSCNT ran */
    uint32_t mpg_last_dest_byte;      /* dest_byte of the most recent real MPG upload */

    /* Round 579 (task #536/#556): real MPG partial-transfer state,
     * persisted ACROSS separate vif_process() calls - matches real
     * hardware's vifStruct.tag.addr/tag.size + vifX.cmd/pass fields
     * (PCSX2's vifCode_MPG "Partial Transfer" vs "Full Transfer"
     * path, Vif_Codes.cpp). Ground-truthed this round: a real VU1
     * microprogram upload can span MULTIPLE separate VIF1 DMA
     * transfers (chain links), and real hardware resumes writing
     * from where a truncated MPG left off on the NEXT transfer,
     * rather than re-reading a fresh VIFcode at that offset. This
     * project's vif_process() previously had no such state (see the
     * older, still-accurate UNPACK-specific version of this same gap
     * documented in this struct's own header comment above) - MPG
     * had the identical gap, undocumented until this round's
     * diagnostic counters (mscal_last_start_byte/mpg_last_dest_byte)
     * caught it in the act: a 2-call, 30-word-total upload where real
     * BIOS data plausibly spans far more than that, with the leftover
     * continuation words silently misparsed as fresh VIFcodes on the
     * following DMA chain link. mpg_pending is nonzero while a
     * partial MPG transfer is outstanding; mpg_pending_addr/_words
     * track the destination byte and words remaining. */
    int      mpg_pending;
    uint32_t mpg_pending_addr;
    uint32_t mpg_pending_words;

    /* Round 580 (task #536/#557): real UNPACK partial-transfer state -
     * the UNPACK-equivalent of Round 579's MPG fix, matching real
     * hardware's nVifStruct::buffer accumulate-then-unpack semantics
     * (Vif_Unpack.cpp's nVifUnpack<>/vifUnpackSetup<> - see vif.c's
     * citation trail). Architecturally DIFFERENT from MPG's
     * incremental-write-and-resume approach: real hardware BUFFERS
     * raw payload bytes across truncated DMA transfers and only runs
     * the actual per-vector unpack loop once the FULL expected
     * payload has accumulated (buffer capped at 4096 bytes = 256*16,
     * matching nVifStruct::buffer's real size, Vif_Dynarec.h - and
     * matching this project's own vif_unpack_needed_bytes() worst
     * case: num=256, gsize=16 -> exactly 4096 bytes, never more).
     * unpack_pending is nonzero while a partial UNPACK transfer is
     * outstanding; unpack_code/unpack_cmd are the original VIFcode
     * that started it (needed to replay the unpack loop - address/
     * NUM/USN/FLG bits - once complete); unpack_needed_bytes/
     * unpack_have_bytes track the total payload size (computed
     * upfront via the real vifUnpackSetup<> closed-form formula, see
     * vif_unpack_needed_bytes()) and how much has been buffered so
     * far. KNOWN SIMPLIFICATION (documented, not silently swept
     * under the rug): the destination VU-mem base address for a
     * FLG-relative (VIF1 TOPS-relative) UNPACK is recomputed from the
     * CURRENT vif->tops at buffer-completion time, not latched at
     * command-issue time like real hardware's tag.addr - a real gap
     * only if TOPS itself changes mid-transfer, an edge case not
     * expected to matter for the BIOS-boot upload this fix targets. */
    int      unpack_pending;
    uint32_t unpack_code;
    uint32_t unpack_cmd;
    uint32_t unpack_needed_bytes;
    uint32_t unpack_have_bytes;
    uint8_t  unpack_buffer[4096];
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
