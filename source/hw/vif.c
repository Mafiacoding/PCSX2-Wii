/*
 * vif.c - see include/core/hw/vif.h for scope notes and references.
 */

#include "core/hw/vif.h"
#include "core/hw/gif.h"
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

    while (pos < total_words) {
        uint32_t code = vif_rd_le32(data + pos * 4u);
        pos++;
        vif->code = code;
        vif->codes_processed++;

        uint32_t cmd = (code >> 24) & 0x7Fu;
        uint32_t imm = code & 0xFFFFu;

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
        case VIF_CMD_MSCAL:
        case VIF_CMD_MSCALF:
        case VIF_CMD_MSCNT:
            /* No VU microcode interpreter exists (docs/ROADMAP.md
             * section 5) - there is nothing queued to flush or
             * execute, so "do nothing" is the correct behavior for
             * this project's current scope, not a shortcut. */
            break;

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
             * (512 words) - matches Vif_Codes.cpp's vifCode_MPG. No VU
             * micro-instruction memory exists yet, so the data is
             * skipped (correctly, so the VIFcode stream stays in
             * sync) rather than written anywhere. */
            uint32_t num = (code >> 16) & 0xFFu;
            uint32_t words = (num ? num : 256u) * 2u;
            if (words > total_words - pos)
                words = total_words - pos;
            pos += words;
            vif->unsupported_cmds_seen++;
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
                gif_process_quadwords(DMA_CHANNEL_GIF, data + pos * 4u, words / 4u);
                vif->direct_qwords_forwarded += words / 4u;
            }
            pos += words;
        } break;

        default:
            /* UNPACK (0x60-0x7F) or any other unrecognized/reserved
             * code - see vif.h's scope comment. Stop here rather than
             * guess a skip length. */
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
