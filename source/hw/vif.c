/*
 * vif.c - see include/core/hw/vif.h for scope notes and references.
 */

#include "core/hw/vif.h"
#include "core/hw/gif.h"
#include "core/hw/vu.h"
#include "core/ee/ee_core.h"
#include <string.h>

/* VIFcode CMD field values (bits 24-30 of the code word) - cross-
 * checked against PCSX2's Vif_Codes.cpp vifCmdHandler[] dispatch
 * table, not guessed. Only the commands this file actually handles
 * (see vif.h) are named here; everything else (including all of
 * UNPACK, 0x60-0x7F) falls through to the "unsupported" path. */
#define VIF_CMD_NOP      0x00
#define VIF_CMD_STCYCL   0x01
#define VIF_CMD_OFFSET   0x02 /* VIF1 only */
#define VIF_CMD_BASE     0x03 /* VIF1 only */
#define VIF_CMD_ITOP     0x04
#define VIF_CMD_STMOD    0x05
#define VIF_CMD_MSKPATH3 0x06 /* VIF1 only */
#define VIF_CMD_MARK     0x07
#define VIF_CMD_FLUSHE   0x10
#define VIF_CMD_FLUSH    0x11 /* VIF1 only */
#define VIF_CMD_FLUSHA   0x13 /* VIF1 only */
#define VIF_CMD_MSCAL    0x14
#define VIF_CMD_MSCALF   0x17
#define VIF_CMD_MSCNT    0x15
#define VIF_CMD_STMASK   0x20
#define VIF_CMD_STROW    0x30
#define VIF_CMD_STCOL    0x31
#define VIF_CMD_MPG      0x4A
#define VIF_CMD_DIRECT   0x50 /* VIF1 only */
#define VIF_CMD_DIRECTHL 0x51 /* VIF1 only */

static vif_state_t g_vif0;
static vif_state_t g_vif1;

void vif_init(void)
{
    memset(&g_vif0, 0, sizeof(g_vif0));
    memset(&g_vif1, 0, sizeof(g_vif1));
    g_vif0.is_vif1 = 0;
    g_vif1.is_vif1 = 1;
}

vif_state_t *vif0_get_state(void) { return &g_vif0; }
vif_state_t *vif1_get_state(void) { return &g_vif1; }

static inline uint32_t vif_rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Real, cited: PCSX2's Vif_Unpack.cpp `nVifT[16]` table (fetched
 * directly from github.com/PCSX2/pcsx2, master) - "Number of bytes of
 * data in the source stream needed for each vector. [equivalent to
 * ((32 >> VL) * (VN+1)) / 8]". Index = VN*4+VL (VN: 0=S,1=V2,2=V3,
 * 3=V4; VL: 0=32-bit,1=16-bit,2=8-bit,3=5-bit/reserved). The three
 * VL==3 slots for S/V2/V3 (indices 3,7,11) don't exist on real
 * hardware - 0 marks them reserved/invalid, matched in vif_unpack(). */
static const uint8_t VIF_UNPACK_SIZE[16] = {
    4, 2, 1, 0,  /* S-32,  S-16,  S-8,  ---- */
    8, 4, 2, 0,  /* V2-32, V2-16, V2-8, ---- */
    12, 6, 3, 0, /* V3-32, V3-16, V3-8, ---- */
    16, 8, 4, 2  /* V4-32, V4-16, V4-8, V4-5 */
};

/* Bounds-checked little-endian read of 'width' (1/2/4) bytes at
 * absolute byte offset 'byte_off' within 'data' (which is
 * total_bytes long). Returns 0 if the read would run past the end of
 * the buffer this project actually has - a safety guard of this
 * project's own, NOT a real hardware behavior (real hardware would
 * read whatever real DMA data follows; see vif.h's "NOT implemented"
 * note on partial/split UNPACK transfers). */
static inline uint32_t vif_rd_bytes_le(const uint8_t *data, uint32_t total_bytes, uint32_t byte_off, uint32_t width)
{
    if (byte_off + width > total_bytes)
        return 0;
    uint32_t v = 0;
    for (uint32_t i = 0; i < width; i++)
        v |= ((uint32_t)data[byte_off + i]) << (8u * i);
    return v;
}

/* Sign/zero-extends a raw 1/2/4-byte component to a full 32-bit value
 * per the UNPACK VIFcode's USN bit (0=signed,1=unsigned) - 32-bit
 * components are always used as-is (matches PCSX2's UnpackFuncSet
 * always using the plain u32 variant for the 32-bit-wide case
 * regardless of USN). */
static inline uint32_t vif_extend(uint32_t raw, uint32_t width, int usn)
{
    if (width == 4u)
        return raw;
    if (width == 2u) {
        if (usn)
            return raw & 0xFFFFu;
        return (uint32_t)(int32_t)(int16_t)(raw & 0xFFFFu);
    }
    /* width == 1 */
    if (usn)
        return raw & 0xFFu;
    return (uint32_t)(int32_t)(int8_t)(raw & 0xFFu);
}

/* Writes one lane (0=x,1=y,2=z,3=w) of one unpacked vector into VU0/
 * VU1 data memory at byte address dest_addr+lane*4, applying the real
 * per-lane mask (Data/MaskRow/MaskCol/Write-Protect) and STMOD mode
 * (0=none,1=add row,2=add+store row,3=store row raw) logic - ported
 * directly from PCSX2's Vif_Unpack.cpp `writeXYZW()` (the 2-bits-per-
 * lane-per-cycle-position `mask` register layout, and the exact
 * `setVifRow()` read-modify-write semantics for modes 2/3). block_pos
 * is the vector's 0-based position within the current STCYCL block
 * (clamped to 0-3 for indexing, matching writeXYZW's own
 * switch(vif.cl){case 0,1,2,default} / std::min(vif.cl,3) grouping). */
static void vif_unpack_write_lane(vif_state_t *vif, uint32_t dest_addr, int lane, uint32_t data, int mode, int mask_enable, uint32_t block_pos)
{
    uint32_t bp = block_pos > 3u ? 3u : block_pos;
    uint32_t n = 0;
    if (mask_enable)
        n = (vif->mask >> (bp * 8u + (uint32_t)lane * 2u)) & 0x3u;

    uint32_t out;
    switch (n) {
    case 0: /* Data */
        switch (mode) {
        case 1: out = data + vif->row[lane]; break;
        case 2: out = vif->row[lane] + data; vif->row[lane] = out; break;
        case 3: out = data; vif->row[lane] = data; break;
        default: out = data; break;
        }
        break;
    case 1: /* MaskRow */
        out = vif->row[lane];
        break;
    case 2: /* MaskCol */
        out = vif->col[bp];
        break;
    default: /* 3: Write Protect - real hardware leaves VU mem untouched */
        return;
    }

    if (vif->is_vif1)
        vu1_mem_write32(dest_addr + (uint32_t)lane * 4u, out);
    else
        vu0_mem_write32(ee_core_get_state(), dest_addr + (uint32_t)lane * 4u, out);
}

/* UNPACK (CMD 0x60-0x7F) - see vif.h's header comment for the full
 * scope/citation trail. Returns 1 on success (having advanced *pos
 * past the consumed payload words), 0 if this is a reserved VN/VL
 * combination (stops the caller from processing the rest of the
 * stream, same "stop rather than guess" philosophy as every other
 * out-of-scope code in this file). */
static int vif_unpack(vif_state_t *vif, uint32_t code, uint32_t cmd, const uint8_t *data, uint32_t total_words, uint32_t *pos)
{
    uint32_t total_bytes = total_words * 4u;
    uint32_t imm = code & 0xFFFFu;
    uint32_t vn = (cmd >> 2) & 0x3u; /* 0=S,1=V2,2=V3,3=V4 */
    uint32_t vl = cmd & 0x3u;        /* 0=32,1=16,2=8,3=5(V4 only) */
    int mask_enable = (cmd & 0x10u) != 0;
    uint32_t gsize = VIF_UNPACK_SIZE[vn * 4u + vl];

    if (gsize == 0u) {
        vif->unsupported_cmds_seen++;
        return 0;
    }

    int usn = (int)((code >> 14) & 0x1u);
    int flg = (int)((code >> 15) & 0x1u);

    uint32_t addr_mask = vif->is_vif1 ? 0x3FFu : 0xFFu;
    uint32_t addr_bits = imm & addr_mask;
    if (vif->is_vif1 && flg)
        addr_bits = (addr_bits + vif->tops) & 0x3FFu;
    uint32_t base_addr = addr_bits << 4; /* qword index -> byte offset */

    uint32_t num = (code >> 16) & 0xFFu;
    if (num == 0u)
        num = 256u;

    uint32_t cl_eff = vif->cycle_cl;
    uint32_t wl_eff = vif->cycle_wl ? vif->cycle_wl : 256u;
    int is_fill = (cl_eff < wl_eff);
    int32_t skip_bytes = ((int32_t)cl_eff - (int32_t)wl_eff) * 16;

    /* vl==3 only occurs for V4 (V4-5); its "component width" is a
     * single 16-bit raw read, handled as a special case below rather
     * than through the generic per-lane read path. */
    uint32_t component_width = (vl == 0u) ? 4u : (vl == 1u) ? 2u : (vl == 2u) ? 1u : 2u;

    uint32_t addr_cursor = base_addr;
    uint32_t block_pos = 0;
    uint32_t src_cursor = (*pos) * 4u;
    uint32_t start_byte = src_cursor;
    /* Tracks the furthest byte offset actually dereferenced by any
     * iteration (including V3's real "reads 1 component past its own
     * gsize" quirk, and fill-mode's repeat iterations which re-read
     * an already-covered position without extending this). This -
     * NOT how far src_cursor itself moved - is what determines how
     * many words of the stream this UNPACK consumed, since (a) in
     * fill mode src_cursor stops advancing once the last real chunk
     * is reached even though that chunk's own bytes are still real,
     * consumed data, and (b) a trailing partial iteration whose
     * cursor advanced in anticipation of a read that never actually
     * happened (num ran out) must NOT be counted. */
    uint32_t max_read_end = start_byte;

    /* V4-5 forces STMOD mode to 0 - see vif.h's citation. */
    int mode = (vn == 3u && vl == 3u) ? 0 : (int)vif->mode;

    for (uint32_t v = 0; v < num; v++) {
        uint32_t lane_val[4];
        uint32_t read_span;

        if (vn == 3u && vl == 3u) { /* V4-5 */
            uint32_t raw = vif_rd_bytes_le(data, total_bytes, src_cursor, 2u);
            lane_val[0] = (raw & 0x001Fu) << 3;
            lane_val[1] = (raw & 0x03E0u) >> 2;
            lane_val[2] = (raw & 0x7C00u) >> 7;
            lane_val[3] = (raw & 0x8000u) >> 8;
            read_span = 2u;
        } else if (vn == 0u) { /* S - broadcast to all 4 lanes */
            uint32_t d = vif_extend(vif_rd_bytes_le(data, total_bytes, src_cursor, component_width), component_width, usn);
            lane_val[0] = lane_val[1] = lane_val[2] = lane_val[3] = d;
            read_span = component_width;
        } else if (vn == 1u) { /* V2 - real hardware repeats the pair: X=v0,Y=v1,Z=v0,W=v1 */
            uint32_t v0 = vif_extend(vif_rd_bytes_le(data, total_bytes, src_cursor + 0u * component_width, component_width), component_width, usn);
            uint32_t v1 = vif_extend(vif_rd_bytes_le(data, total_bytes, src_cursor + 1u * component_width, component_width), component_width, usn);
            lane_val[0] = v0; lane_val[1] = v1; lane_val[2] = v0; lane_val[3] = v1;
            read_span = 2u * component_width;
        } else { /* V3 (reuses V4's 4-component read - real hardware
                  * quirk, see citation above) or V4: both dereference
                  * 4 full components, even though V3's OWN gsize is
                  * only 3 components wide. */
            for (int lane = 0; lane < 4; lane++)
                lane_val[lane] = vif_extend(vif_rd_bytes_le(data, total_bytes, src_cursor + (uint32_t)lane * component_width, component_width), component_width, usn);
            read_span = 4u * component_width;
        }

        uint32_t this_end = src_cursor + read_span;
        if (this_end > max_read_end)
            max_read_end = this_end;

        for (int lane = 0; lane < 4; lane++)
            vif_unpack_write_lane(vif, addr_cursor, lane, lane_val[lane], mode, mask_enable, block_pos);
        vif->unpack_vectors_written++;

        addr_cursor += 16u;
        block_pos++;

        if (is_fill) {
            if (block_pos <= cl_eff)
                src_cursor += gsize;
            else if (block_pos == wl_eff)
                block_pos = 0;
        } else {
            src_cursor += gsize;
            if (block_pos >= wl_eff) {
                addr_cursor = (uint32_t)((int32_t)addr_cursor + skip_bytes);
                block_pos = 0;
            }
        }
    }

    uint32_t consumed_bytes = max_read_end - start_byte;
    uint32_t consumed_words = (consumed_bytes + 3u) / 4u;
    uint32_t avail_words = total_words - (*pos);
    if (consumed_words > avail_words)
        consumed_words = avail_words;
    *pos += consumed_words;
    return 1;
}

/* Walks a stream of VIFcode words (interspersed with per-command data
 * words), starting at 'data' (qwc*16 bytes = qwc*4 32-bit words).
 * Returns when the stream is exhausted OR an unsupported command
 * (UNPACK, or a VIF1-only command issued to VIF0) is hit - in the
 * latter case, the rest of 'data' is deliberately left unprocessed
 * rather than guessing a skip length and misparsing what follows as
 * garbage VIFcodes (same philosophy as gif.c's REGLIST/IMAGE
 * fallback). */
static void vif_process(vif_state_t *vif, const uint8_t *data, uint32_t qwc)
{
    uint32_t total_words = qwc * 4u;
    uint32_t pos = 0; /* in 32-bit words */

    /* Round 579 (task #536/#556): resume a real MPG upload left
     * outstanding by a PRIOR vif_process() call (a real VIF1 DMA
     * chain link boundary that landed mid-microprogram-upload) BEFORE
     * reading any fresh VIFcode from this call's buffer - see vif.h's
     * mpg_pending field comment. Matches real hardware's vifCode_MPG
     * "Partial Transfer" -> resume-on-next-transfer semantics
     * (Vif_Codes.cpp), which this project previously had no
     * equivalent of at all. */
    if (vif->mpg_pending) {
        uint32_t avail = total_words - pos;
        uint32_t words = (vif->mpg_pending_words < avail) ? vif->mpg_pending_words : avail;
        for (uint32_t w = 0; w < words; w++) {
            uint32_t word = vif_rd_le32(data + (pos + w) * 4u);
            if (vif->is_vif1)
                vu1_micro_write32(vif->mpg_pending_addr + w * 4u, word);
            else
                vu0_micro_write32(ee_core_get_state(), vif->mpg_pending_addr + w * 4u, word);
        }
        pos += words;
        vif->mpg_pending_addr += words * 4u;
        vif->mpg_pending_words -= words;
        vif->mpg_words_written += words;
        if (vif->mpg_pending_words == 0u)
            vif->mpg_pending = 0;
    }

    while (pos < total_words) {
        uint32_t code = vif_rd_le32(data + pos * 4u);
        pos++;
        vif->code = code;
        vif->codes_processed++;

        uint32_t cmd = (code >> 24) & 0x7Fu;
        uint32_t imm = code & 0xFFFFu;

        if ((cmd & 0x60u) == 0x60u) {
            /* UNPACK (0x60-0x7F) - see vif.h/vif_unpack() for the
             * full scope. vif_unpack() advances pos itself past
             * whatever payload it consumed. */
            if (!vif_unpack(vif, code, cmd, data, total_words, &pos))
                return;
            continue;
        }

        switch (cmd) {
        case VIF_CMD_NOP:
            break;

        case VIF_CMD_STCYCL:
            vif->cycle_cl = (uint8_t)(imm & 0xFFu);
            vif->cycle_wl = (uint8_t)((imm >> 8) & 0xFFu);
            break;

        case VIF_CMD_OFFSET:
            if (vif->is_vif1) {
                vif->ofst = imm & 0x3FFu;
                vif->tops = vif->base;
            } else {
                vif->unsupported_cmds_seen++;
            }
            break;

        case VIF_CMD_BASE:
            if (vif->is_vif1)
                vif->base = imm & 0x3FFu;
            else
                vif->unsupported_cmds_seen++;
            break;

        case VIF_CMD_ITOP:
            /* Real hardware masks to 0xFF on VIF0 (VU0 mem is smaller)
             * and 0x3FF on VIF1 (VU1 mem is larger) - see Vif_Codes.cpp's
             * vifCode_ITop / the ITOP overrun check in vuExecMicro. */
            vif->itops = imm & (vif->is_vif1 ? 0x3FFu : 0xFFu);
            break;

        case VIF_CMD_STMOD:
            vif->mode = code & 0x3u;
            break;

        case VIF_CMD_MSKPATH3:
            if (!vif->is_vif1)
                vif->unsupported_cmds_seen++;
            /* VIF1-side effect is masking GIF PATH3 arbitration - not
             * modeled (no GIF path-priority model exists), safe no-op
             * on VIF1 too. */
            break;

        case VIF_CMD_MARK:
            vif->mark = imm;
            break;

        case VIF_CMD_FLUSHE:
            /* Real hardware waits for the VU's microprogram to
             * finish; this project's vu0_exec_micro()/vu1_exec_micro()
             * already run synchronously to completion (or the safety
             * cap) inside MSCAL/MSCNT/MSCALF below, so by the time
             * control reaches a later FLUSHE there is never anything
             * left to wait for - a correct no-op given that. */
            break;

        case VIF_CMD_MSCAL:
        case VIF_CMD_MSCALF: {
            /* Real hardware: MSCAL/MSCALF start the microprogram at
             * IMM. See include/core/hw/vu.h for what "execute" means
             * this round (real control flow, no real opcode bodies
             * yet - a genuine, narrower step from vif.c's prior total
             * no-op). */
            /* Round 578b: real diagnostic capture - see vif.h's
             * mscal_calls/mscal_last_start_byte field comment. */
            vif->mscal_calls++;
            vif->mscal_last_start_byte = imm * 8u;
            if (vif->is_vif1)
                vu1_exec_micro(imm);
            else
                vu0_exec_micro(ee_core_get_state(), imm);
        } break;

        case VIF_CMD_MSCNT: {
            /* Round 576 (task #551): real MSCNT resumes the VU's
             * microprogram from its CURRENT TPC, ignoring IMM (a real
             * MSCNT VIFcode's IMM field is unused/reserved - ground-
             * truthed against ps2sdk's own
             * packet2_utils_vu_add_continue_program(), which issues
             * FLUSH+MSCNT with NO address argument at all, unlike
             * packet2_utils_vu_add_start_program()'s FLUSH+MSCAL(addr)
             * - see vu.c's vu1_exec_micro_continue() citation). This
             * project previously treated MSCNT identically to MSCAL
             * (passing IMM, almost always 0, as a fresh start address
             * every time), which would silently restart any multi-
             * draw-call microprogram from address 0 on every "continue"
             * instead of resuming where it left off - exactly the real,
             * standard pattern real game code uses to issue one draw
             * call per MSCNT after a single MSCAL. */
            /* Round 578b: real diagnostic capture - see vif.h's
             * mscnt_calls/mscnt_last_resume_byte field comment. Read
             * BEFORE the call so this records where execution actually
             * resumed FROM, not the (mid-run or post-cap) tpc it ends
             * up at afterward. */
            vif->mscnt_calls++;
            if (vif->is_vif1)
                vif->mscnt_last_resume_byte = vu1_get_state()->tpc;
            /* VU0 macro-mode has no exposed tpc accessor - this
             * diagnostic field is only meaningful on the VIF1/VU1
             * struct instance, left unset (0) for VIF0. */
            if (vif->is_vif1)
                vu1_exec_micro_continue();
            else
                vu0_exec_micro_continue(ee_core_get_state());
        } break;

        case VIF_CMD_FLUSH:
        case VIF_CMD_FLUSHA:
            if (!vif->is_vif1)
                vif->unsupported_cmds_seen++;
            break;

        case VIF_CMD_STMASK:
            if (pos < total_words) {
                vif->mask = vif_rd_le32(data + pos * 4u);
                pos++;
            }
            break;

        case VIF_CMD_STROW:
            for (int i = 0; i < 4 && pos < total_words; i++) {
                vif->row[i] = vif_rd_le32(data + pos * 4u);
                pos++;
            }
            break;

        case VIF_CMD_STCOL:
            for (int i = 0; i < 4 && pos < total_words; i++) {
                vif->col[i] = vif_rd_le32(data + pos * 4u);
                pos++;
            }
            break;

        case VIF_CMD_MPG: {
            /* NUM field (bits 16-23): number of VU micro-instructions,
             * each 2 words (8 bytes); NUM==0 means 256 instructions
             * (512 words) - matches Vif_Codes.cpp's vifCode_MPG. IMM
             * is the destination micro-instruction-memory address, in
             * the same "instruction pair index" units MSCAL/MSCNT use
             * (byte offset = imm*8 - see vu.h's vu1_exec_micro() doc
             * comment, same addressing convention). Now that real
             * VU0/VU1 micro-instruction memory exists (this round),
             * the data is actually written there instead of just
             * being skipped over. */
            uint32_t num = (code >> 16) & 0xFFu;
            uint32_t requested_words = (num ? num : 256u) * 2u;
            uint32_t avail_words = total_words - pos;
            uint32_t words = (requested_words > avail_words) ? avail_words : requested_words;
            uint32_t dest_byte = imm * 8u;
            for (uint32_t w = 0; w < words; w++) {
                uint32_t word = vif_rd_le32(data + (pos + w) * 4u);
                if (vif->is_vif1)
                    vu1_micro_write32(dest_byte + w * 4u, word);
                else
                    vu0_micro_write32(ee_core_get_state(), dest_byte + w * 4u, word);
            }
            pos += words;
            /* Round 578/578b: real diagnostic counters - see vif.h's
             * mpg_calls/mpg_words_written/mpg_last_dest_byte field
             * comments. */
            vif->mpg_calls++;
            vif->mpg_words_written += words;
            vif->mpg_last_dest_byte = dest_byte;
            /* Round 579: real hardware's "Partial Transfer" case -
             * this DMA chain link ended before the full requested
             * upload was written. Persist the remainder so the NEXT
             * vif_process() call resumes writing it instead of the
             * leftover words being misread as fresh VIFcodes - see
             * vif.h's mpg_pending field comment. Since this consumes
             * the rest of the current buffer, the outer while loop
             * exits naturally (pos == total_words). */
            if (words < requested_words) {
                vif->mpg_pending = 1;
                vif->mpg_pending_addr = dest_byte + words * 4u;
                vif->mpg_pending_words = requested_words - words;
            }
        } break;

        case VIF_CMD_DIRECT:
        case VIF_CMD_DIRECTHL: {
            if (!vif->is_vif1) {
                vif->unsupported_cmds_seen++;
                break;
            }
            /* IMM = qword count to forward; IMM==0 means 65536 qwords
             * (real hardware wraparound - see Vif_Codes.cpp's
             * _vifCode_Direct: "vifImm ? (vifImm*4) : (65536*4)",
             * counted there in 32-bit words). */
            uint32_t qw = imm ? imm : 65536u;
            uint32_t words = qw * 4u;
            uint32_t avail = total_words - pos;
            if (words > avail)
                words = avail; /* real hardware would stall waiting for
                                 * more DMA data; we just forward what
                                 * we actually have this call. */
            if (words > 0) {
                /* Round 542: this is real hardware PATH2 (VIF1 DIRECT/
                 * DIRECTHL forwarding straight to GIF, bypassing the
                 * GIF DMA channel entirely) - was previously mislabeled
                 * as DMA_CHANNEL_GIF (a DMA-channel constant, not a
                 * transfer-path one) purely because gif_process_quadwords()
                 * ignored its channel argument. Now that the argument
                 * drives real GIF_TAG/CNT/P3CNT/P3TAG register state
                 * (see gif.c), it must be the correct real path. */
                gif_process_quadwords(GIF_PATH_2, data + pos * 4u, words / 4u);
                vif->direct_qwords_forwarded += words / 4u;
            }
            pos += words;
        } break;

        default:
            /* Any other unrecognized/reserved code - see vif.h's
             * scope comment. Stop here rather than guess a skip
             * length. */
            vif->unsupported_cmds_seen++;
            return;
        }
    }
}

void vif0_process_quadwords(int channel, const uint8_t *data, uint32_t qwc)
{
    (void)channel;
    vif_process(&g_vif0, data, qwc);
}

void vif1_process_quadwords(int channel, const uint8_t *data, uint32_t qwc)
{
    (void)channel;
    vif_process(&g_vif1, data, qwc);
}
