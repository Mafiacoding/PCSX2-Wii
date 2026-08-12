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
static uint32_t vif_rd_le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

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
#define VIF_CMD_MSCAL    0x14
#define VIF_CMD_MSCALF   0x15
#define VIF_CMD_MSCNT    0x17
#define VIF_CMD_MPG      0x4A
#define VIF_CMD_DIRECT   0x50
#define VIF_CMD_UNPACK_S_32   0x60
#define VIF_CMD_UNPACK_V2_16  0x65
#define VIF_CMD_UNPACK_V3_8   0x6A
#define VIF_CMD_UNPACK_V4_32  0x6C
#define VIF_CMD_UNPACK_V4_5   0x6F

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

    /* --- UNPACK (task: "VIF UNPACK") --- */

    { /* V4-32, no mask, 1 vector: straightforward 4-component write to VU1 mem @ addr 0 */
        vif_init();
        uint8_t buf[16 * 2];
        memset(buf, 0, sizeof(buf));
        wle32(buf + 0,  enc_vifcode(VIF_CMD_UNPACK_V4_32, 1, 0));
        wle32(buf + 4,  0x11111111u);
        wle32(buf + 8,  0x22222222u);
        wle32(buf + 12, 0x33333333u);
        wle32(buf + 16, 0x44444444u);
        wle32(buf + 20, enc_vifcode(VIF_CMD_ITOP, 0, 0x3FFu));
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, sizeof(buf) / 16);
        vif_state_t *v1 = vif1_get_state();
        CHECK(v1->unsupported_cmds_seen == 0, "UNPACK V4-32: real, implemented code - not counted as unsupported");
        CHECK(v1->unpack_vectors_written == 1, "UNPACK V4-32: exactly 1 vector written");
        vu1_state_t *vu1 = vu1_get_state();
        uint32_t x = vif_rd_le32(vu1->mem + 0), y = vif_rd_le32(vu1->mem + 4);
        uint32_t z = vif_rd_le32(vu1->mem + 8), w = vif_rd_le32(vu1->mem + 12);
        CHECK(x == 0x11111111u && y == 0x22222222u && z == 0x33333333u && w == 0x44444444u,
              "UNPACK V4-32: all 4 lanes written correctly to VU1 mem address 0");
        CHECK(v1->itops == 0x3FFu, "UNPACK V4-32: parsing correctly resumed right after the 4 data words");
    }

    { /* S-32 broadcast: one source value written to all 4 lanes */
        vif_init();
        uint8_t buf[16];
        wle32(buf + 0, enc_vifcode(VIF_CMD_UNPACK_S_32, 1, 0));
        wle32(buf + 4, 0xCAFEBABEu);
        wle32(buf + 8, enc_vifcode(VIF_CMD_NOP, 0, 0));
        wle32(buf + 12, enc_vifcode(VIF_CMD_NOP, 0, 0));
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, 1);
        vu1_state_t *vu1 = vu1_get_state();
        int ok = 1;
        for (int lane = 0; lane < 4; lane++)
            if (vif_rd_le32(vu1->mem + lane * 4) != 0xCAFEBABEu) ok = 0;
        CHECK(ok, "UNPACK S-32: single source value broadcast to all 4 lanes");
    }

    { /* V2-16 signed vs unsigned (USN bit) + the real X=v0,Y=v1,Z=v0,W=v1 repeat */
        vif_init();
        uint8_t buf[16];
        memset(buf, 0, sizeof(buf));
        /* USN=0 (signed, bit14=0): 0x8000 must sign-extend to 0xFFFF8000 */
        wle32(buf + 0, enc_vifcode(VIF_CMD_UNPACK_V2_16, 1, 0));
        buf[4] = 0x00; buf[5] = 0x80; /* v0 = 0x8000 */
        buf[6] = 0x34; buf[7] = 0x12; /* v1 = 0x1234 */
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, 1);
        vu1_state_t *vu1 = vu1_get_state();
        uint32_t x = vif_rd_le32(vu1->mem + 0), y = vif_rd_le32(vu1->mem + 4);
        uint32_t z = vif_rd_le32(vu1->mem + 8), w = vif_rd_le32(vu1->mem + 12);
        CHECK(x == 0xFFFF8000u, "UNPACK V2-16 signed (USN=0): 0x8000 sign-extends to 0xFFFF8000");
        CHECK(y == 0x00001234u, "UNPACK V2-16 signed (USN=0): 0x1234 stays positive");
        CHECK(z == x && w == y, "UNPACK V2-16: real hardware repeats the pair (Z=v0, W=v1)");

        vif_init();
        memset(buf, 0, sizeof(buf));
        /* USN=1 (unsigned, bit14=1): 0x8000 must zero-extend to 0x00008000 */
        wle32(buf + 0, enc_vifcode(VIF_CMD_UNPACK_V2_16, 1, 0) | (1u << 14));
        buf[4] = 0x00; buf[5] = 0x80;
        buf[6] = 0x34; buf[7] = 0x12;
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, 1);
        vu1 = vu1_get_state();
        x = vif_rd_le32(vu1->mem + 0);
        CHECK(x == 0x00008000u, "UNPACK V2-16 unsigned (USN=1): 0x8000 zero-extends to 0x00008000");
    }

    { /* V3-8: real hardware reads a 4th (W) component 1 byte past the
       * real 3-byte vector - confirmed straight from PCSX2's own
       * Vif_Unpack.cpp comment/UNPACK_V4 reuse for V3. */
        vif_init();
        uint8_t buf[16];
        memset(buf, 0, sizeof(buf));
        wle32(buf + 0, enc_vifcode(VIF_CMD_UNPACK_V3_8, 1, 0));
        buf[4] = 0x10; buf[5] = 0x20; buf[6] = 0x30; /* the real 3-component vector */
        buf[7] = 0x40; /* 1 byte past it - real hardware reads this into W */
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, 1);
        vu1_state_t *vu1 = vu1_get_state();
        uint32_t x = vif_rd_le32(vu1->mem + 0), y = vif_rd_le32(vu1->mem + 4);
        uint32_t z = vif_rd_le32(vu1->mem + 8), w = vif_rd_le32(vu1->mem + 12);
        CHECK(x == 0x10u && y == 0x20u && z == 0x30u, "UNPACK V3-8: the real 3 components land in X/Y/Z");
        CHECK(w == 0x40u, "UNPACK V3-8: W reads 1 byte past the real vector, matching real hardware's documented quirk");
    }

    { /* V4-5: fixed real bit-shift decode of a packed 16-bit value */
        vif_init();
        uint8_t buf[16];
        memset(buf, 0, sizeof(buf));
        wle32(buf + 0, enc_vifcode(VIF_CMD_UNPACK_V4_5, 1, 0));
        /* raw=0x8421: X=(raw&0x1F)<<3, Y=(raw&0x3E0)>>2, Z=(raw&0x7C00)>>7, W=(raw&0x8000)>>8 */
        uint32_t raw = 0x8421u;
        buf[4] = (uint8_t)(raw & 0xFFu); buf[5] = (uint8_t)((raw >> 8) & 0xFFu);
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, 1);
        vu1_state_t *vu1 = vu1_get_state();
        uint32_t x = vif_rd_le32(vu1->mem + 0), y = vif_rd_le32(vu1->mem + 4);
        uint32_t z = vif_rd_le32(vu1->mem + 8), w = vif_rd_le32(vu1->mem + 12);
        CHECK(x == ((raw & 0x001Fu) << 3), "UNPACK V4-5: X decoded via the real bit-shift formula");
        CHECK(y == ((raw & 0x03E0u) >> 2), "UNPACK V4-5: Y decoded via the real bit-shift formula");
        CHECK(z == ((raw & 0x7C00u) >> 7), "UNPACK V4-5: Z decoded via the real bit-shift formula");
        CHECK(w == ((raw & 0x8000u) >> 8), "UNPACK V4-5: W decoded via the real bit-shift formula");
    }

    { /* STMASK-driven per-lane masking: Data/MaskRow/MaskCol/Write-Protect */
        vif_init();
        vif_state_t *v1 = vif1_get_state();
        v1->row[0] = 0x1000u; v1->row[1] = 0x2000u; v1->row[2] = 0x3000u; v1->row[3] = 0x4000u;
        v1->col[0] = 0x5000u;
        /* mask (2 bits/lane, cycle-pos 0 = bits 0-7): X=0(Data) Y=1(MaskRow) Z=2(MaskCol) W=3(WriteProtect) */
        v1->mask = 0x000000E4u; /* 0b11_10_01_00 = W:3,Z:2,Y:1,X:0 */

        vu1_state_t *vu1 = vu1_get_state();
        memset(vu1->mem, 0xFFu, 16); /* so Write-Protect leaving it untouched is verifiable */

        uint8_t buf[16 * 2];
        memset(buf, 0, sizeof(buf));
        wle32(buf + 0,  enc_vifcode(VIF_CMD_UNPACK_V4_32, 1, 0) | (1u << 28)); /* M bit (0x10) set */
        wle32(buf + 4,  0xAAAAAAAAu); wle32(buf + 8, 0xBBBBBBBBu);
        wle32(buf + 12, 0xCCCCCCCCu); wle32(buf + 16, 0xDDDDDDDDu);
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, sizeof(buf) / 16);

        uint32_t x = vif_rd_le32(vu1->mem + 0), y = vif_rd_le32(vu1->mem + 4);
        uint32_t z = vif_rd_le32(vu1->mem + 8), w = vif_rd_le32(vu1->mem + 12);
        CHECK(x == 0xAAAAAAAAu, "UNPACK masking: n=0 (Data) writes the real unpacked value");
        CHECK(y == 0x2000u, "UNPACK masking: n=1 (MaskRow) writes row[1] instead of the source data");
        CHECK(z == 0x5000u, "UNPACK masking: n=2 (MaskCol) writes col[0] (cycle position 0) instead of the source data");
        CHECK(w == 0xFFFFFFFFu, "UNPACK masking: n=3 (Write-Protect) leaves VU mem completely untouched");
    }

    { /* STMOD mode 2 (add row, then store the sum back into row) */
        vif_init();
        vif_state_t *v1 = vif1_get_state();
        v1->cycle_cl = 1; v1->cycle_wl = 1; /* 1 real value per address, no skip/fill - isolates the STMOD concern */
        v1->mode = 2;
        v1->row[0] = 100u;
        uint8_t buf[16 * 2];
        memset(buf, 0, sizeof(buf));
        wle32(buf + 0, enc_vifcode(VIF_CMD_UNPACK_S_32, 2, 0));
        wle32(buf + 4, 5u);
        wle32(buf + 8, 7u);
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, sizeof(buf) / 16);
        vu1_state_t *vu1 = vu1_get_state();
        uint32_t v0 = vif_rd_le32(vu1->mem + 0);
        uint32_t v1out = vif_rd_le32(vu1->mem + 16);
        CHECK(v0 == 105u, "UNPACK STMOD mode 2: first vector = data(5) + row[0](100) = 105");
        CHECK(v1out == 112u, "UNPACK STMOD mode 2: second vector = data(7) + the UPDATED row[0](105) = 112");
        CHECK(v1->row[0] == 112u, "UNPACK STMOD mode 2: row[0] ends at 112 - updated again by the 2nd vector's own accumulate");
    }

    { /* STCYCL skip-write mode (CL > WL): WL real values written per block, then CL-WL addresses skipped */
        vif_init();
        vif_state_t *v1 = vif1_get_state();
        v1->cycle_cl = 4; v1->cycle_wl = 2; /* write 2, skip 2, per 4-address block */
        uint8_t buf[16 * 4];
        memset(buf, 0, sizeof(buf));
        wle32(buf + 0, enc_vifcode(VIF_CMD_UNPACK_S_32, 4, 0));
        for (int i = 0; i < 4; i++) wle32(buf + 4 + i * 4, 0x1000u + (uint32_t)i);
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, sizeof(buf) / 16);
        vu1_state_t *vu1 = vu1_get_state();
        uint32_t a0 = vif_rd_le32(vu1->mem + 0 * 16);
        uint32_t a1 = vif_rd_le32(vu1->mem + 1 * 16);
        uint32_t a2 = vif_rd_le32(vu1->mem + 2 * 16); /* skipped - should be untouched (0) */
        uint32_t a3 = vif_rd_le32(vu1->mem + 3 * 16); /* skipped - should be untouched (0) */
        uint32_t a4 = vif_rd_le32(vu1->mem + 4 * 16); /* next block's 1st write */
        CHECK(a0 == 0x1000u && a1 == 0x1001u, "UNPACK skip-write (CL=4,WL=2): the first 2 real values land at addresses 0-1");
        CHECK(a2 == 0u && a3 == 0u, "UNPACK skip-write (CL=4,WL=2): addresses 2-3 are skipped (left untouched)");
        CHECK(a4 == 0x1002u, "UNPACK skip-write (CL=4,WL=2): the 3rd real value starts the NEXT 4-address block at address 4");
    }

    { /* STCYCL fill mode (WL > CL): CL real reads per block, with the
       * LAST real read repeating for the remaining (WL-CL) addresses.
       * Traced by hand against PCSX2's real _nVifUnpackLoop timing
       * (read happens BEFORE the post-increment advance check, so the
       * very first slot of a fresh block reads the not-yet-advanced
       * pointer, the pointer then advances once per real slot, and
       * only the FINAL real slot's position gets genuinely repeated
       * for the trailing fill slots): for CL=2,WL=4,NUM=4 the real,
       * verified sequence is [src(P0), src(P0+G), src(P0+2G),
       * src(P0+2G)] - i.e. 2 fresh reads (matching CL=2), then the
       * 2nd fresh read's value (NOT the 1st) repeats for the 1
       * remaining fill slot. */
        vif_init();
        vif_state_t *v1 = vif1_get_state();
        v1->cycle_cl = 2; v1->cycle_wl = 4;
        uint8_t buf[16 * 2];
        memset(buf, 0, sizeof(buf));
        wle32(buf + 0, enc_vifcode(VIF_CMD_UNPACK_S_32, 4, 0));
        wle32(buf + 4, 0x1111u);  /* src(P0) */
        wle32(buf + 8, 0x2222u);  /* src(P0+G) */
        wle32(buf + 12, 0x3333u); /* src(P0+2G) */
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, sizeof(buf) / 16);
        vu1_state_t *vu1 = vu1_get_state();
        uint32_t a0 = vif_rd_le32(vu1->mem + 0 * 16);
        uint32_t a1 = vif_rd_le32(vu1->mem + 1 * 16);
        uint32_t a2 = vif_rd_le32(vu1->mem + 2 * 16);
        uint32_t a3 = vif_rd_le32(vu1->mem + 3 * 16);
        CHECK(a0 == 0x1111u && a1 == 0x2222u, "UNPACK fill-write (CL=2,WL=4): the 2 real reads land at slots 0-1");
        CHECK(a2 == 0x3333u, "UNPACK fill-write (CL=2,WL=4): slot 2 is a 3rd real read (real hardware's advance-then-read timing), not a repeat of slot 0/1");
        CHECK(a3 == 0x3333u, "UNPACK fill-write (CL=2,WL=4): slot 3 (the true fill slot) repeats slot 2's value");
    }

    { /* Reserved VN/VL combination (e.g. S-5, CMD 0x63) - must stop cleanly, matching this project's own "stop rather than guess" philosophy */
        vif_init();
        uint8_t buf[16];
        wle32(buf + 0,  0x63u << 24); /* VN=0(S), VL=3 - reserved/invalid, gsize==0 in VIF_UNPACK_SIZE */
        wle32(buf + 4,  enc_vifcode(VIF_CMD_ITOP, 0, 0x3FFu));
        wle32(buf + 8,  enc_vifcode(VIF_CMD_ITOP, 0, 0x3FFu));
        wle32(buf + 12, enc_vifcode(VIF_CMD_ITOP, 0, 0x3FFu));
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, 1);
        vif_state_t *v1 = vif1_get_state();
        CHECK(v1->unsupported_cmds_seen == 1, "UNPACK reserved VN/VL (S-5): counted as unsupported");
        CHECK(v1->codes_processed == 1, "UNPACK reserved VN/VL: processing stopped immediately");
        CHECK(v1->itops == 0, "UNPACK reserved VN/VL: the words right after it were NOT misparsed as ITOP codes");
    }

    { /* Round 580 (task #536/#557): UNPACK payload split across TWO
       * separate vif1_process_quadwords() calls - a real VIF1 DMA
       * chain link boundary landing mid-UNPACK-payload, the UNPACK
       * counterpart of the MPG cross-DMA-chunk case Round 579 fixed.
       * A real DMA transfer is always a whole number of quadwords
       * (qwc), so a 1-qword (4-word) call carrying a V4-32/NUM=1
       * UNPACK (VIFcode + 4 data words = 5 words needed) can only
       * ever deliver the VIFcode + 3 of its 4 data words in a single
       * qword - the 4th data word necessarily lands in the NEXT
       * transfer. Real hardware buffers the partial payload and only
       * unpacks once the full vector has arrived - verified by
       * checking VU1 mem is COMPLETELY untouched after call 1, and
       * only becomes correct after call 2 completes it. */
        vif_init();
        vu1_init(); /* VU1 mem is a separate global not reset by vif_init() - earlier tests in this file leave real data there */
        uint8_t call1[16];
        wle32(call1 + 0, enc_vifcode(VIF_CMD_UNPACK_V4_32, 1, 0)); /* NUM=1 vector, VU1 addr 0; needed_bytes=16 */
        wle32(call1 + 4, 0xAAAAAAAAu); /* data word 0 of 4 */
        wle32(call1 + 8, 0xBBBBBBBBu); /* data word 1 of 4 */
        wle32(call1 + 12, 0xCCCCCCCCu); /* data word 2 of 4 - still short by 1 word/4 bytes */
        vif1_process_quadwords(DMA_CHANNEL_VIF1, call1, 1);
        vif_state_t *v1mid = vif1_get_state();
        vu1_state_t *vu1mid = vu1_get_state();
        CHECK(v1mid->unpack_pending == 1, "UNPACK split-transfer: still pending after call 1 (payload incomplete)");
        CHECK(v1mid->unpack_have_bytes == 12u && v1mid->unpack_needed_bytes == 16u, "UNPACK split-transfer: buffered exactly 12 of 16 needed bytes after call 1 (a 1-qword call can only ever supply VIFcode+3 words)");
        CHECK(vif_rd_le32(vu1mid->mem + 0) == 0u, "UNPACK split-transfer: VU1 mem X lane untouched after call 1 (real hardware buffers, doesn't write early)");
        CHECK(vif_rd_le32(vu1mid->mem + 4) == 0u, "UNPACK split-transfer: VU1 mem Y lane untouched after call 1");

        uint8_t call2[16];
        wle32(call2 + 0, 0xDDDDDDDDu); /* data word 3 of 4 - completes the pending UNPACK (needs exactly 4 more bytes) */
        wle32(call2 + 4, enc_vifcode(VIF_CMD_ITOP, 0, 0x3FFu)); /* fresh VIFcode right after - must be parsed correctly, not misparsed as leftover payload */
        wle32(call2 + 8, enc_vifcode(VIF_CMD_NOP, 0, 0));
        wle32(call2 + 12, enc_vifcode(VIF_CMD_NOP, 0, 0));
        vif1_process_quadwords(DMA_CHANNEL_VIF1, call2, 1);
        vif_state_t *v1end = vif1_get_state();
        vu1_state_t *vu1end = vu1_get_state();
        CHECK(v1end->unpack_pending == 0, "UNPACK split-transfer: no longer pending after call 2 completes the payload");
        CHECK(vif_rd_le32(vu1end->mem + 0) == 0xAAAAAAAAu, "UNPACK split-transfer: X lane correct (from call 1's buffered bytes)");
        CHECK(vif_rd_le32(vu1end->mem + 4) == 0xBBBBBBBBu, "UNPACK split-transfer: Y lane correct (from call 1's buffered bytes)");
        CHECK(vif_rd_le32(vu1end->mem + 8) == 0xCCCCCCCCu, "UNPACK split-transfer: Z lane correct (from call 1's buffered bytes)");
        CHECK(vif_rd_le32(vu1end->mem + 12) == 0xDDDDDDDDu, "UNPACK split-transfer: W lane correct (from call 2's completing byte)");
        CHECK(v1end->itops == 0x3FFu, "UNPACK split-transfer: the fresh ITOP VIFcode right after the completed payload was parsed correctly, not misparsed as leftover payload data");
        CHECK(v1end->unpack_vectors_written == 1u, "UNPACK split-transfer: exactly 1 real vector written total (not double-counted across the two calls)");
    }

    { /* Round 580: an UNPACK payload split across THREE real deliveries
       * (2 real resumes) plus a genuinely-empty qwc=0 call thrown in
       * the middle (real hardware can see zero-quadword transfers) -
       * guards against an off-by-one in the resume-accumulation loop
       * that a single-resume test alone wouldn't catch. V4-32,
       * NUM=3 (3 vectors = 48 bytes needed, forcing multiple resumes
       * since no single 1-2 qword call can supply that much) - needs
       * an explicit STCYCL CL=1/WL=1 (real "no skip, no fill"
       * identity mode) first, since the default un-STCYCL'd CL=WL=0
       * state makes NUM>1 UNPACKs re-read a single vector's worth of
       * source bytes for every vector (a real, pre-existing project
       * characteristic unrelated to this round's fix - see the dry-
       * run needed_bytes helper's citation in vif.c). */
        vif_init();
        vu1_init(); /* VU1 mem is a separate global not reset by vif_init() - earlier tests in this file leave real data there */
        vif1_get_state()->cycle_cl = 1;
        vif1_get_state()->cycle_wl = 1;
        uint8_t c1[16], c2[32];
        wle32(c1 + 0, enc_vifcode(VIF_CMD_UNPACK_V4_32, 3, 0)); /* needed_bytes = 3*16 = 48 */
        wle32(c1 + 4, 0x01010101u);
        wle32(c1 + 8, 0x02020202u);
        wle32(c1 + 12, 0x03030303u);
        vif1_process_quadwords(DMA_CHANNEL_VIF1, c1, 1); /* 1 qword -> VIFcode + 3 words = 12 bytes buffered */
        CHECK(vif1_get_state()->unpack_pending == 1 && vif1_get_state()->unpack_have_bytes == 12u, "UNPACK 3-call split: pending with 12/48 bytes after call 1");

        uint8_t empty[1];
        vif1_process_quadwords(DMA_CHANNEL_VIF1, empty, 0); /* genuinely empty (qwc=0) transfer - must be a safe no-op */
        CHECK(vif1_get_state()->unpack_pending == 1 && vif1_get_state()->unpack_have_bytes == 12u, "UNPACK 3-call split: a genuinely empty qwc=0 call in between changes nothing");

        for (int i = 0; i < 8; i++) wle32(c2 + i * 4, (uint32_t)(0x04040404u + (uint32_t)i * 0x01010101u));
        vif1_process_quadwords(DMA_CHANNEL_VIF1, c2, 2); /* 2 qwords = 8 more words = 32 bytes -> have=12+32=44, still short by 4 */
        CHECK(vif1_get_state()->unpack_pending == 1 && vif1_get_state()->unpack_have_bytes == 44u, "UNPACK 3-call split: still pending with 44/48 bytes after the 2nd real call (2nd resume)");

        uint8_t c4[16];
        wle32(c4 + 0, 0x0C0C0C0Cu); /* the final 4 bytes, completing 48 */
        wle32(c4 + 4, enc_vifcode(VIF_CMD_ITOP, 0, 0x155u));
        wle32(c4 + 8, enc_vifcode(VIF_CMD_NOP, 0, 0));
        wle32(c4 + 12, enc_vifcode(VIF_CMD_NOP, 0, 0));
        vif1_process_quadwords(DMA_CHANNEL_VIF1, c4, 1);
        vif_state_t *v1 = vif1_get_state();
        vu1_state_t *vu1 = vu1_get_state();
        CHECK(v1->unpack_pending == 0, "UNPACK 3-call split: completed after the 3rd real call (3rd resume)");
        CHECK(vif_rd_le32(vu1->mem + 0*16 + 0) == 0x01010101u && vif_rd_le32(vu1->mem + 0*16 + 12) == 0x04040404u, "UNPACK 3-call split: 1st vector's X/W lanes correct (bytes from calls 1 and 3 stitched together)");
        CHECK(vif_rd_le32(vu1->mem + 2*16 + 12) == 0x0C0C0C0Cu, "UNPACK 3-call split: 3rd (last) vector's W lane correct (the final completing bytes)");
        CHECK(v1->itops == 0x155u, "UNPACK 3-call split: the fresh ITOP VIFcode right after the fully-completed 48-byte payload was parsed correctly");
    }

    {
        /* Round 583 (task #560): regression test for the real MSCAL(0x14)/
         * MSCALF(0x15)/MSCNT(0x17) opcode-value fix. Ground-truthed
         * against real PCSX2's Vif_Codes.cpp vifCmdHandler[] dispatch
         * table (see vif.c's Round 583 comment). Before this fix,
         * this project had MSCALF=0x17 and MSCNT=0x15 (the two
         * swapped), which silently routed any real MSCALF VIFcode
         * (0x15) into the "resume at current tpc" MSCNT handler
         * instead of the "start microprogram at IMM, count as a
         * fresh MSCAL-family call" handler - exactly the bug behind
         * task #560's "mscal_calls stays at 1 despite ongoing per-
         * frame VIF1 traffic" symptom. */
        vif_init();
        vu1_init();
        uint8_t buf[16];

        /* Real MSCALF VIFcode: cmd=0x15, imm=0x10 (start byte = 0x80). */
        wle32(buf + 0, enc_vifcode(VIF_CMD_MSCALF, 0, 0x10u));
        for (int i = 1; i < 4; i++) wle32(buf + i * 4, enc_vifcode(VIF_CMD_NOP, 0, 0));
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, 1);
        vif_state_t *v1 = vif1_get_state();
        CHECK(v1->mscal_calls == 1, "MSCALF opcode fix: real cmd=0x15 dispatches to the MSCAL/MSCALF handler (mscal_calls==1)");
        CHECK(v1->mscal_last_start_byte == 0x80u, "MSCALF opcode fix: start byte correctly computed as imm*8 (0x10*8=0x80)");
        CHECK(v1->mscnt_calls == 0, "MSCALF opcode fix: real cmd=0x15 must NOT be misrouted into the MSCNT handler");

        /* Real MSCNT VIFcode: cmd=0x17, imm field is reserved/unused. */
        wle32(buf + 0, enc_vifcode(VIF_CMD_MSCNT, 0, 0));
        for (int i = 1; i < 4; i++) wle32(buf + i * 4, enc_vifcode(VIF_CMD_NOP, 0, 0));
        vif1_process_quadwords(DMA_CHANNEL_VIF1, buf, 1);
        CHECK(v1->mscnt_calls == 1, "MSCNT opcode fix: real cmd=0x17 dispatches to the MSCNT (resume) handler (mscnt_calls==1)");
        CHECK(v1->mscal_calls == 1, "MSCNT opcode fix: real cmd=0x17 must NOT be misrouted into the MSCAL/MSCALF handler (count stays 1)");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
