/*
 * Round 588 (task #536/#447, "beta version" per explicit user request)
 * forced-rendering proof driver. Host-native only, not part of the Wii
 * build (tools/ is excluded from the Makefile's SOURCES list - see
 * tools/round577-tekken-discboot/driver.c's header comment for the
 * same established convention).
 *
 * User's exact request this round: "Focus on the Bios Only so the
 * bios can display some framebuffer and build an beta version where
 * you can try to force some rendering." This directly follows Round
 * 586's finding (1.68B-instruction survey) that the diskless JP BIOS
 * boot's VU1/GIF pipeline is implemented correctly but never fires
 * organically, because OSDSYS's own disc-browser dispatch never
 * escalates (task #447, itself unresolved - Round 587's beta-guess
 * poke of the browser-state field didn't unlock it either).
 *
 * Rather than faking pixel data directly into GS memory (which would
 * prove nothing about the real pipeline), this driver hand-constructs
 * a genuine GIF REGLIST-mode packet (FLG=1 - see gif.c's
 * process_one_packet(), Round 26/542 documentation) and feeds it
 * through the real, production gif_process_quadwords(GIF_PATH_3, ...)
 * entry point - the exact same function real GIF DMA transfers use.
 * Every register in a REGLIST packet is routed through
 * apply_ad_write(), the same per-register writer A+D-mode PACKED
 * packets use, so this exercises the real PRIM-dispatch,
 * RGBAQ-unpack, XYZ2-vertex-kick, and rasterize_triangle() code paths
 * end-to-end (rasterize_triangle() itself unchanged since Round 451).
 *
 * Packet layout: REGLIST tag with NREG=5, NLOOP=1 (five distinct
 * register codes cycled once - process_one_packet()'s REGLIST loop is
 * regs_nibble(tag_w2,tag_w3, i % nreg), so NREG must equal the real
 * number of distinct registers in the sequence for a plain 1:1 list
 * like this one):
 *   reg0: PRIM   = TRIANGLE (type=3), flat/untextured (apply_ad_write's
 *         GS_REG_PRIM case)
 *   reg1: RGBAQ  = solid semi-transparent red (R=255,G=40,B=40,A=128),
 *         Q=1.0f (GS_REG_RGBAQ case: RGBA packed into data_lo bytes,
 *         Q is data_hi as a raw IEEE-754 float)
 *   reg2: XYZ2   = vertex 0 (top),         pixel (320,60)  * 16
 *   reg3: XYZ2   = vertex 1 (bottom-left), pixel (220,180) * 16
 *   reg4: XYZ2   = vertex 2 (bottom-right),pixel (420,180) * 16
 * (12.4 fixed point per apply_xyz2_kick's raw_x/raw_y >>4 decode; 3
 * XYZ2 vertices under PRIM_TYPE_TRIANGLE triggers rasterize_triangle()
 * on the 3rd vertex - g_gif.tri_vseq % 3 == 0).
 *
 * No FRAME_1/XYOFFSET_1/SCISSOR_1 writes are made by this driver -
 * unlike a from-scratch A+D stream, this one deliberately reuses
 * whatever real context state the organic 40M-instruction boot
 * already configured via its own genuine GIF traffic (this project's
 * own gif_init() defaults - fbp=0/fbw=640/xyoffset=0/unclipped -
 * would only apply on a truly cold GS, which this isn't once any
 * organic FRAME_1/XYOFFSET_1 write has happened). A first version of
 * this driver assumed the cold defaults and used bare pixel*16
 * coordinates - it silently landed off-screen (verified: no pixel
 * change, even though triangles_drawn still incremented and no error
 * occurred - a rasterize genuinely happened, just outside the visible
 * window/active scissor). The corrected version reads the REAL
 * current fbp/fbw/xyoffset/scissor from gif_get_state() right before
 * building the packet and offsets the hand-picked pixel coordinates
 * by the real xyoffset_x/y (apply_xyz2_kick's own
 * (raw_x-xyoffset_x)>>4 decode), then dumps from the real fbp/fbw
 * instead of a hardcoded 0/640 - this is what actually produced a
 * visible triangle (verified below: centroid pixel matches the
 * injected RGBAQ exactly, non_zero_pixel count grows by the expected
 * amount, and the dumped PPM shows a real filled triangle rendered on
 * top of the organic boot's existing line/sprite scene).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/hw/gs.h"
#include "core/hw/gs_mem.h"
#include "core/hw/gif.h"
#include "core/hw/vu.h"

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int main(void)
{
    bios_image_t bios;
    if (bios_load("/sessions/sharp-youthful-pascal/mnt/uploads/scph10000.bin", &bios) != 0) {
        fprintf(stderr, "FATAL: bios_load failed\n");
        return 1;
    }

    if (system_init(&bios, &bios) != 0) {
        fprintf(stderr, "FATAL: system_init failed\n");
        return 1;
    }
    /* Boot to the same steady-state idle point Round 586's survey
     * established, purely so this demo runs in a representative
     * post-boot machine state - the forced packet itself doesn't
     * depend on it. */
    system_run_interleaved(40000000ULL);

    gif_state_t *gif_before = gif_get_state();
    printf("[PRE-INJECT] quadwords_seen=%lu triangles_drawn=%lu unsupported_prims_seen=%lu fbp=%u fbw=%u xyoff=(0x%x,0x%x) scissor_configured=%d scissor=(%u,%u)-(%u,%u)\n",
           gif_before->quadwords_seen, gif_before->triangles_drawn, gif_before->unsupported_prims_seen,
           gif_before->fbp, gif_before->fbw, gif_before->xyoffset_x, gif_before->xyoffset_y, gif_before->scissor_configured,
           gif_before->scissor_x0, gif_before->scissor_y0, gif_before->scissor_x1, gif_before->scissor_y1);

    {
        long pre_non_bg = 0;
        uint32_t pfbp = gif_before->fbp, pfbw = gif_before->fbw ? gif_before->fbw : 640u;
        for (int yy = 0; yy < 224; yy++)
            for (int xx = 0; xx < 640; xx++)
                if (gs_mem_read_psmct32(pfbp, pfbw, (uint32_t)xx, (uint32_t)yy) != 0) pre_non_bg++;
        printf("[PRE-INJECT] framebuffer(fbp=%u,fbw=%u) non_zero_pixels_before=%ld\n", pfbp, pfbw, pre_non_bg);
        uint32_t centroid_before = gs_mem_read_psmct32(gif_before->fbp, gif_before->fbw ? gif_before->fbw : 640u, 320u, 140u);
        printf("[PRE-INJECT] centroid pixel (320,140) = 0x%08x\n", centroid_before);
    }

    uint8_t pkt[16 + 3 * 16];
    memset(pkt, 0, sizeof(pkt));

    uint32_t nloop = 1, nreg = 5, flg = 1 /* REGLIST */, eop = 1;
    uint32_t tag_w0 = (nloop & 0x7FFFu) | (eop << 15);
    uint32_t tag_w1 = ((flg & 0x3u) << 26) | ((nreg & 0xFu) << 28);
    uint32_t tag_w2 = (uint32_t)GS_REG_PRIM | ((uint32_t)GS_REG_RGBAQ << 4) |
                       ((uint32_t)GS_REG_XYZ2 << 8) | ((uint32_t)GS_REG_XYZ2 << 12) |
                       ((uint32_t)GS_REG_XYZ2 << 16);
    uint32_t tag_w3 = 0;

    wr_le32(pkt + 0, tag_w0);
    wr_le32(pkt + 4, tag_w1);
    wr_le32(pkt + 8, tag_w2);
    wr_le32(pkt + 12, tag_w3);

    uint32_t prim_val = PRIM_TYPE_TRIANGLE; /* IIP=0 flat, TME=0, FGE=0, ABE=0, AA1=0, FST=0, CTXT=0, FIX=0 */
    uint32_t r = 255, g = 40, b = 40, a = 128;
    uint32_t rgbaq_lo = r | (g << 8) | (b << 16) | (a << 24);
    float qf = 1.0f;
    uint32_t rgbaq_hi;
    memcpy(&rgbaq_hi, &qf, 4);

    /* Vertices in 12.4 fixed point (pixel * 16), offset by the REAL
     * xyoffset already configured by the organic boot (apply_xyz2_kick
     * computes x=(raw_x-xyoffset_x)>>4, so raw_x must be
     * xyoffset_x + pixel*16 to land at the intended screen pixel -
     * using bare pixel*16 without this offset, as this driver's first
     * attempt did, silently lands far off-screen/outside the active
     * scissor rect and draws nothing visible, even though
     * triangles_drawn still increments). */
    uint32_t v0_x = gif_before->xyoffset_x + (320u << 4), v0_y = gif_before->xyoffset_y + (60u << 4);
    uint32_t v1_x = gif_before->xyoffset_x + (220u << 4), v1_y = gif_before->xyoffset_y + (180u << 4);
    uint32_t v2_x = gif_before->xyoffset_x + (420u << 4), v2_y = gif_before->xyoffset_y + (180u << 4);

    uint8_t *d = pkt + 16;
    /* qword0: reg0=PRIM (lo=prim_val,hi=0), reg1=RGBAQ (lo=rgbaq_lo,hi=rgbaq_hi) */
    wr_le32(d + 0, prim_val);   wr_le32(d + 4, 0);
    wr_le32(d + 8, rgbaq_lo);   wr_le32(d + 12, rgbaq_hi);
    /* qword1: reg2=XYZ2 v0 (lo=X,hi=Y), reg3=XYZ2 v1 (lo=X,hi=Y) */
    wr_le32(d + 16, v0_x); wr_le32(d + 20, v0_y);
    wr_le32(d + 24, v1_x); wr_le32(d + 28, v1_y);
    /* qword2: reg4=XYZ2 v2 (lo=X,hi=Y), upper half unused padding */
    wr_le32(d + 32, v2_x); wr_le32(d + 36, v2_y);
    wr_le32(d + 40, 0);    wr_le32(d + 44, 0);

    uint32_t qwc = sizeof(pkt) / 16; /* 4 qwords total (tag + 3 data) */
    gif_process_quadwords(GIF_PATH_3, pkt, qwc);

    gif_state_t *gif_after = gif_get_state();
    printf("[POST-INJECT] quadwords_seen=%lu triangles_drawn=%lu unsupported_prims_seen=%lu fbp=%u fbw=%u\n",
           gif_after->quadwords_seen, gif_after->triangles_drawn, gif_after->unsupported_prims_seen,
           gif_after->fbp, gif_after->fbw);
    {
        uint32_t centroid_after = gs_mem_read_psmct32(gif_after->fbp, gif_after->fbw ? gif_after->fbw : 640u, 320u, 140u);
        printf("[POST-INJECT] centroid pixel (320,140) = 0x%08x\n", centroid_after);
    }

    /* Dump the forced framebuffer region (fbp=0, fbw=640, PSMCT32) to
     * a PPM, same technique as Round 586's survey driver / Round
     * 450's original DISPFB2 dump. */
    const int W = 640, H = 224;
    uint32_t dump_fbp = gif_after->fbp;
    uint32_t dump_fbw = gif_after->fbw ? gif_after->fbw : 640u;
    FILE *f = fopen("/tmp/round588_forced.ppm", "wb");
    if (!f) { fprintf(stderr, "FATAL: could not open output PPM\n"); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    long non_bg = 0;
    for (int yy = 0; yy < H; yy++) {
        for (int xx = 0; xx < W; xx++) {
            uint32_t px = gs_mem_read_psmct32(dump_fbp, dump_fbw, (uint32_t)xx, (uint32_t)yy);
            uint8_t pr = (uint8_t)(px & 0xFF);
            uint8_t pg = (uint8_t)((px >> 8) & 0xFF);
            uint8_t pb = (uint8_t)((px >> 16) & 0xFF);
            if (px != 0) non_bg++;
            fputc(pr, f); fputc(pg, f); fputc(pb, f);
        }
    }
    fclose(f);
    printf("[FRAMEBUFFER] non_zero_pixels=%ld / %d (fbp=%u fbw=%u)\n", non_bg, W * H, dump_fbp, dump_fbw);
    printf("[FRAMEBUFFER] dumped to /tmp/round588_forced.ppm\n");

    return 0;
}
