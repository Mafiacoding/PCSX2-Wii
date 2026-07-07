#include <stdio.h>
#include <string.h>
#include "core/hw/gs_mem.h"
#include "core/hw/gif.h"
#include "core/hw/vif.h"
#include "core/hw/vu.h"
#include "core/hw/dma.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

static uint32_t enc_vifcode(uint32_t cmd, uint32_t num, uint32_t imm)
{
    return ((cmd & 0x7Fu) << 24) | ((num & 0xFFu) << 16) | (imm & 0xFFFFu);
}

#define VIF_CMD_NOP      0x00
#define VIF_CMD_STCYCL   0x01
#define VIF_CMD_OFFSET   0x02
#define VIF_CMD_BASE     0x03
#define VIF_CMD_ITOP     0x04
#define VIF_CMD_STMASK   0x20
#define VIF_CMD_STROW    0x30
#define VIF_CMD_STCOL    0x31
#define VIF_CMD_MPG      0x4A
#define VIF_CMD_DIRECT   0x50
#define VIF_CMD_UNPACK_V4_32 0x6C

int main(void)
{
    {
        vif_init();
        uint8_t buf[16];
        for (int i = 0; i < 4; i++) wle32(buf + i * 4, enc_vifcode(VIF_CMD_NOP, 0, 0));
        vif0_process_quadwords(DMA_CHANNEL_VIF0, buf, 1);
        vif_state_t *v0 = vif0_get_state();
        CHECK(v0->codes_processed == 4, "NOP: all 4 words in the qword processed cleanly as NOPs");
        CHECK(v0->unsupported_cmds_seen == 0, "NOP: not counted as unsupported");
    }

    {
        vif_init();
        uint8_t buf[16];
        wle32(buf, enc_vifcode(VIF_CMD_STCYCL, 0, (7u << 8) | 3u));
        for (int i = 1; i < 4; i++) wle32(buf + i * 4, enc_vifcode(VIF_CMD_NOP, 0, 0));
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, 1);
        vif_state_t *v1 = vif1_get_state();
        CHECK(v1->cycle_cl == 3 && v1->cycle_wl == 7, "STCYCL: cl/wl decoded correctly from IMM");
    }

    {
        vif_init();
        uint8_t buf[16];
        wle32(buf, enc_vifcode(VIF_CMD_ITOP, 0, 0x3FFu));
        for (int i = 1; i < 4; i++) wle32(buf + i * 4, enc_vifcode(VIF_CMD_NOP, 0, 0));
        vif0_process_quadwords(DMA_CHANNEL_VIF0, buf, 1);
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, 1);
        CHECK(vif0_get_state()->itops == 0xFFu, "ITOP: VIF0 masks ITOPS to 0xFF (VU0 mem is smaller)");
        CHECK(vif1_get_state()->itops == 0x3FFu, "ITOP: VIF1 keeps the full 0x3FF (VU1 mem is larger)");
    }

    {
        vif_init();
        uint8_t buf[16];
        wle32(buf, enc_vifcode(VIF_CMD_OFFSET, 0, 0x100u));
        for (int i = 1; i < 4; i++) wle32(buf + i * 4, enc_vifcode(VIF_CMD_NOP, 0, 0));
        vif0_process_quadwords(DMA_CHANNEL_VIF0, buf, 1);
        CHECK(vif0_get_state()->unsupported_cmds_seen == 1, "OFFSET on VIF0: counted as unsupported (VIF1-only on real hardware)");
        CHECK(vif0_get_state()->ofst == 0, "OFFSET on VIF0: register NOT updated");

        vif_init();
        wle32(buf, enc_vifcode(VIF_CMD_BASE, 0, 0x123u));
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, 1);
        CHECK(vif1_get_state()->unsupported_cmds_seen == 0, "BASE on VIF1: accepted (real VIF1-only command)");
        CHECK(vif1_get_state()->base == 0x123u, "BASE on VIF1: register updated correctly");
    }

    {
        vif_init();
        uint8_t buf[16];
        wle32(buf + 0,  enc_vifcode(VIF_CMD_STMASK, 0, 0));
        wle32(buf + 4,  0xDEADBEEFu);
        wle32(buf + 8,  enc_vifcode(VIF_CMD_STCYCL, 0, (5u << 8) | 5u));
        wle32(buf + 12, enc_vifcode(VIF_CMD_NOP, 0, 0));
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, 1);
        vif_state_t *v1 = vif1_get_state();
        CHECK(v1->mask == 0xDEADBEEFu, "STMASK: trailing data word stored into mask register");
        CHECK(v1->cycle_cl == 5 && v1->cycle_wl == 5, "STMASK: parsing correctly resumed at the marker code right after the data word");
        CHECK(v1->codes_processed == 3, "STMASK: exactly 3 codes seen (STMASK, STCYCL marker, trailing NOP)");
    }

    {
        vif_init();
        uint8_t buf[16 * 3];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        wle32(buf + off, enc_vifcode(VIF_CMD_STROW, 0, 0)); off += 4;
        for (int i = 0; i < 4; i++) { wle32(buf + off, 0x10u + (uint32_t)i); off += 4; }
        wle32(buf + off, enc_vifcode(VIF_CMD_STCOL, 0, 0)); off += 4;
        for (int i = 0; i < 4; i++) { wle32(buf + off, 0x20u + (uint32_t)i); off += 4; }
        while (off < (int)sizeof(buf)) { wle32(buf + off, enc_vifcode(VIF_CMD_NOP, 0, 0)); off += 4; }
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, sizeof(buf) / 16);
        vif_state_t *v1 = vif1_get_state();
        int ok_row = 1, ok_col = 1;
        for (int i = 0; i < 4; i++) {
            if (v1->row[i] != 0x10u + (uint32_t)i) ok_row = 0;
            if (v1->col[i] != 0x20u + (uint32_t)i) ok_col = 0;
        }
        CHECK(ok_row, "STROW: all 4 trailing data words stored into row[0..3] in order");
        CHECK(ok_col, "STCOL: all 4 trailing data words stored into col[0..3] in order");
    }

    {
        vif_init();
        uint8_t buf[16 * 2];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        wle32(buf + off, enc_vifcode(VIF_CMD_MPG, 2, 0)); off += 4;
        for (int i = 0; i < 4; i++) { wle32(buf + off, 0xAAu + (uint32_t)i); off += 4; }
        wle32(buf + off, enc_vifcode(VIF_CMD_STCYCL, 0, (9u << 8) | 9u)); off += 4;
        while (off < (int)sizeof(buf)) { wle32(buf + off, enc_vifcode(VIF_CMD_NOP, 0, 0)); off += 4; }
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, sizeof(buf) / 16);
        vif_state_t *v1 = vif1_get_state();
        CHECK(v1->unsupported_cmds_seen == 0, "MPG: no longer counted as unsupported (now writes real VU1 micro-instruction memory)");
        CHECK(v1->cycle_cl == 9 && v1->cycle_wl == 9, "MPG: correctly skipped its 4-word data span and parsed the marker code right after it");

        vu1_state_t *vu1 = vu1_get_state();
        int mpg_ok = 1;
        for (int i = 0; i < 4; i++) {
            uint32_t w = (uint32_t)vu1->micro[i*4] | ((uint32_t)vu1->micro[i*4+1]<<8) |
                         ((uint32_t)vu1->micro[i*4+2]<<16) | ((uint32_t)vu1->micro[i*4+3]<<24);
            if (w != 0xAAu + (uint32_t)i) mpg_ok = 0;
        }
        CHECK(mpg_ok, "MPG: the 4 microprogram words were actually written into VU1 micro-instruction memory at IMM=0");
    }

    {
        gs_mem_init();
        gif_init();
        vif_init();

        uint8_t gifpkt[16 * 7];
        memset(gifpkt, 0, sizeof(gifpkt));
        int goff = 0;
        wle32(gifpkt + goff, 6u | (1u << 15)); wle32(gifpkt + goff + 4, (0u << 26) | (1u << 28));
        wle32(gifpkt + goff + 8, GIF_REG_AD); wle32(gifpkt + goff + 12, 0); goff += 16;
        wle32(gifpkt + goff, (10u << 9)); wle32(gifpkt + goff + 4, 0);
        wle32(gifpkt + goff + 8, GS_REG_FRAME_1); wle32(gifpkt + goff + 12, 0); goff += 16;
        wle32(gifpkt + goff, 0); wle32(gifpkt + goff + 4, 0);
        wle32(gifpkt + goff + 8, GS_REG_XYOFFSET_1); wle32(gifpkt + goff + 12, 0); goff += 16;
        wle32(gifpkt + goff, PRIM_TYPE_SPRITE); wle32(gifpkt + goff + 4, 0);
        wle32(gifpkt + goff + 8, GS_REG_PRIM); wle32(gifpkt + goff + 12, 0); goff += 16;
        wle32(gifpkt + goff, 0xFF00FF00u); wle32(gifpkt + goff + 4, 0);
        wle32(gifpkt + goff + 8, GS_REG_RGBAQ); wle32(gifpkt + goff + 12, 0); goff += 16;
        wle32(gifpkt + goff, (10u << 4)); wle32(gifpkt + goff + 4, (10u << 4));
        wle32(gifpkt + goff + 8, GS_REG_XYZ2); wle32(gifpkt + goff + 12, 0); goff += 16;
        wle32(gifpkt + goff, (30u << 4)); wle32(gifpkt + goff + 4, (30u << 4));
        wle32(gifpkt + goff + 8, GS_REG_XYZ2); wle32(gifpkt + goff + 12, 0); goff += 16;
        uint32_t gif_qwc = (uint32_t)(goff / 16);

        /* DIRECT's data (the GIF packet) starts IMMEDIATELY after the
         * VIFcode word itself (byte offset 4) - no filler/alignment
         * gap, matching real hardware (and vif.c's implementation,
         * which forwards starting right at its own "pos" after
         * consuming just the one code word). Round the buffer up to
         * a whole qword; any trailing bytes beyond the packet are
         * harmless NOP padding parsed as separate codes afterward. */
        uint32_t vifbuf_words = 1u + gif_qwc * 4u;
        uint32_t total_qwc = (vifbuf_words + 3u) / 4u;
        uint8_t vifbuf[16 * 8]; /* generously sized; only the first total_qwc*16 bytes are used */
        memset(vifbuf, 0, sizeof(vifbuf));
        for (uint32_t w = 0; w < total_qwc * 4u; w++) wle32(vifbuf + w * 4u, enc_vifcode(VIF_CMD_NOP, 0, 0));
        wle32(vifbuf, enc_vifcode(VIF_CMD_DIRECT, 0, gif_qwc));
        memcpy(vifbuf + 4, gifpkt, (size_t)goff);

        vif1_process_quadwords(DMA_CHANNEL_VIF1, vifbuf, total_qwc);

        vif_state_t *v1 = vif1_get_state();
        CHECK(v1->direct_qwords_forwarded == gif_qwc, "DIRECT: forwarded exactly the packet's own qword count to the GIF parser");
        CHECK(v1->unsupported_cmds_seen == 0, "DIRECT: not counted as unsupported (a real, implemented command)");

        gif_state_t *gs = gif_get_state();
        CHECK(gs->sprites_drawn == 1, "DIRECT: the forwarded GIF packet actually drew a sprite");
        CHECK(gs_mem_read_psmct32(0, 640, 15, 15) == 0xFF00FF00u, "DIRECT: the sprite's pixels are correct (green) - real DMA->VIF->GIF->pixels path");
    }

    {
        gs_mem_init();
        gif_init();
        vif_init();
        uint8_t buf[16];
        wle32(buf, enc_vifcode(VIF_CMD_DIRECT, 0, 1));
        for (int i = 1; i < 4; i++) wle32(buf + i * 4, enc_vifcode(VIF_CMD_NOP, 0, 0));
        vif0_process_quadwords(DMA_CHANNEL_VIF0, buf, 1);
        vif_state_t *v0 = vif0_get_state();
        CHECK(v0->unsupported_cmds_seen == 1, "DIRECT on VIF0: counted as unsupported (VIF1-only on real hardware)");
        CHECK(v0->direct_qwords_forwarded == 0, "DIRECT on VIF0: nothing forwarded to the GIF parser");
    }

    {
        vif_init();
        uint8_t buf[16];
        wle32(buf + 0, enc_vifcode(VIF_CMD_UNPACK_V4_32, 1, 0));
        wle32(buf + 4, enc_vifcode(VIF_CMD_ITOP, 0, 0x3FFu));
        wle32(buf + 8, enc_vifcode(VIF_CMD_ITOP, 0, 0x3FFu));
        wle32(buf + 12, enc_vifcode(VIF_CMD_ITOP, 0, 0x3FFu));
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, 1);
        vif_state_t *v1 = vif1_get_state();
        CHECK(v1->unsupported_cmds_seen == 1, "UNPACK: counted as unsupported");
        CHECK(v1->codes_processed == 1, "UNPACK: processing stopped immediately - nothing after it was parsed as a code");
        CHECK(v1->itops == 0, "UNPACK: the words right after it were NOT misparsed as ITOP codes");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
