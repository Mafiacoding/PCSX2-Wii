/*
 * Round 731 (task #536/#447/#545, direct continuation of Round 729/730's
 * real GT3 disc-boot survey). User's exact request: "keep pushing and
 * fill rasterizer and now its time to show some polygons work with the
 * gt3 file."
 *
 * Round 730 confirmed GT3's own disc-boot code has, after 4.4+ billion
 * real EE instructions, drawn real wireframe/sprite/point geometry
 * (lines=2499, sprites=2291, points=271) but ZERO real triangle-type
 * primitives (triangles_drawn=0) - the exact same wireframe-only
 * signature this project's diskless-BIOS investigation already
 * root-caused back in Round 570 ("rasterize_triangle() is a complete,
 * correct, already-shipped implementation ... the bottleneck is
 * entirely upstream: whatever real BIOS/OSDSYS code sets PRIM to a
 * triangle type never follows up with vertex kicks"). GT3 itself is
 * currently parked on the same disc-command-dispatch gate (task #447)
 * that blocked the diskless-BIOS path for months - it has not yet
 * organically issued a triangle draw in this trace window.
 *
 * This driver reuses Round 588's exact, already-verified methodology
 * (a genuine GIF REGLIST packet routed through the real, production
 * gif_process_quadwords(GIF_PATH_3, ...) entry point - NOT writing
 * pixels directly - so PRIM-dispatch, RGBAQ-unpack, XYZ2-vertex-kick,
 * and rasterize_triangle() all run for real), but for the FIRST TIME
 * applies it against GT3's own live, real, checkpointed disc-boot
 * state (fbp/fbw/xyoffset/scissor all read fresh from the actual GT3
 * run) instead of a synthetic from-scratch BIOS boot. This proves the
 * fill rasterizer works using GT3's own real GS context - the same
 * context its 2499 lines / 2291 sprites / 271 points were drawn into -
 * rather than an isolated, unrelated demo.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/hw/gs.h"
#include "core/hw/gs_mem.h"
#include "core/hw/gs_wii_output.h"
#include "core/hw/gif.h"
#include "core/hw/vu.h"
#include "core/hw/iop_cdvd.h"

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static long count_nonzero(uint32_t bp, uint32_t bw, int w, int h)
{
    long n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (gs_mem_read_psmct32(bp, bw, (uint32_t)x, (uint32_t)y) != 0) n++;
    return n;
}

static void dump_ppm(const char *path, uint32_t bp, uint32_t bw, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "could not open %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t px = gs_mem_read_psmct32(bp, bw, (uint32_t)x, (uint32_t)y);
            fputc(px & 0xFF, f); fputc((px >> 8) & 0xFF, f); fputc((px >> 16) & 0xFF, f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path>\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    gif_state_t *gif = gif_get_state();
    printf("[PRE] triangles=%lu lines=%lu sprites=%lu points=%lu (flat/stale) fbp=%u fbw=%u xyoff=(0x%x,0x%x) scissor=(%u,%u)-(%u,%u)\n",
           gif->triangles_drawn, gif->lines_drawn, gif->sprites_drawn, gif->points_drawn,
           gif->fbp, gif->fbw, gif->xyoffset_x, gif->xyoffset_y,
           gif->scissor_x0, gif->scissor_y0, gif->scissor_x1, gif->scissor_y1);
    /* Our injected PRIM leaves CTXT=0 (context 1), so the rasterizer
     * will pull context 1's OWN permanent fields via
     * apply_context_select() - NOT whatever the generic gif->fbp/
     * xyoffset_x/etc happen to reflect right now (which may be stale
     * leftovers from whichever context GT3's own code selected last,
     * likely context 2 given DISPFB2's use - Round 321/730). Read
     * ctx1_* directly so our vertex math and post-dump use the SAME
     * context our injected PRIM actually draws into. */
    printf("[PRE] context1 (what we'll actually draw into): fbp=%u fbw=%u xyoff=(0x%x,0x%x) scissor=(%u,%u)-(%u,%u) configured=%d\n",
           gif->ctx1_fbp, gif->ctx1_fbw, gif->ctx1_xyoffset_x, gif->ctx1_xyoffset_y,
           gif->ctx1_scissor_x0, gif->ctx1_scissor_y0, gif->ctx1_scissor_x1, gif->ctx1_scissor_y1,
           gif->ctx1_scissor_configured);
    printf("[PRE] context1 Z-test state: zbuf_configured=%d zte=%d ztst=%d zbp=%u\n",
           gif->ctx1_zbuf_configured, gif->ctx1_zte, gif->ctx1_ztst, gif->ctx1_zbp);

    uint32_t fbp = gif->ctx1_fbp, fbw = gif->ctx1_fbw ? gif->ctx1_fbw : 640u;
    long pre_nz = count_nonzero(fbp, fbw, 640, 224);
    printf("[PRE] framebuffer(fbp=%u,fbw=%u) non_zero_pixels=%ld\n", fbp, fbw, pre_nz);
    dump_ppm("/tmp/round729_gt3/r731_pre.ppm", fbp, fbw, 640, 224);

    /* Same REGLIST packet shape as Round 588: PRIM(TRIANGLE) + RGBAQ +
     * 3x XYZ2, routed through the real gif_process_quadwords(PATH3,...)
     * entry point. Bright green, fully opaque, so it stands out clearly
     * against GT3's existing blue-ish wireframe lines. Vertices chosen
     * to sit centrally in the 640x224 visible window, offset by GT3's
     * OWN real xyoffset (read above) exactly as apply_xyz2_kick()
     * requires (raw = xyoffset + pixel*16). */
    /* GT3 (a real racing game, unlike Round 588's diskless-BIOS-only
     * test) has genuinely configured Z-buffering on context 1
     * (confirmed live below via ctx1_zbuf_configured/ctx1_zte=1,
     * ztst=3/GREATER). A first attempt at this driver used a plain
     * REGLIST-mode packet (Round 588's exact approach) plus a separate
     * REGLIST GS_REG_TEST_1 write to disable Z-testing - triangles_drawn
     * incremented but zero pixels changed either time. Root cause,
     * found by inspecting gif.c's own REGLIST decode (regs_nibble()):
     * REGLIST's REGS descriptor is a genuine 4-bit-per-register nibble
     * (matching real GS hardware), so it can only address registers
     * numbered 0x0-0xF directly (PRIM=0x00, RGBAQ=0x01, XYZ2=0x05,
     * etc.) - TEST_1's real GS bus address is 0x47, structurally
     * unreachable through a REGLIST nibble. Real hardware (and this
     * project's own PACKED-mode decode) reaches such registers via the
     * "A+D" (Address+Data) escape - GIF_REG_AD=0x0E is a PACKED-mode-
     * only register slot whose OWN qword carries (data_lo, data_hi,
     * real_address) rather than being addressed by the REGS nibble
     * itself (see gif.c's `case GIF_REG_AD: apply_ad_write(w2 & 0xFFu,
     * w0, w1)`). This project's REGLIST loop does not implement this
     * escape at all (it just calls apply_ad_write() with the raw 4-bit
     * nibble as if it were already a resolved address) - so a REGLIST
     * TEST_1 write is silently a no-op here, explaining exactly what
     * was observed (ctx1_zte stayed 1 after the "successful"-looking
     * REGLIST write).
     *
     * Fix: build a single PACKED-mode (FLG=0) packet instead, with 6
     * registers: reg0=AD->TEST_1 (ZTE=0, disabling Z-test for this
     * draw only - a real, in-spec GS operation, not a rasterizer
     * bypass), reg1=PRIM(TRIANGLE), reg2=RGBAQ, reg3..5=XYZ2 for the 3
     * vertices. PACKED mode's own XYZ2 case (gif.c line ~2483) also
     * carries a REAL w2=Z field, unlike REGLIST's forced Z=0 - not
     * needed once Z-test is off, but included as 0 for clarity. */
    uint8_t pkt[16 + 6 * 16];
    memset(pkt, 0, sizeof(pkt));

    uint32_t nloop = 1, nreg = 6, flg = 0 /* PACKED */, eop = 1;
    uint32_t tag_w0 = (nloop & 0x7FFFu) | (eop << 15);
    uint32_t tag_w1 = ((flg & 0x3u) << 26) | ((nreg & 0xFu) << 28);
    uint32_t tag_w2 = (uint32_t)GIF_REG_AD | ((uint32_t)GIF_REG_PRIM << 4) |
                       ((uint32_t)GIF_REG_RGBAQ << 8) | ((uint32_t)GIF_REG_XYZ2 << 12) |
                       ((uint32_t)GIF_REG_XYZ2 << 16) | ((uint32_t)GIF_REG_XYZ2 << 20);
    uint32_t tag_w3 = 0;
    wr_le32(pkt + 0, tag_w0);
    wr_le32(pkt + 4, tag_w1);
    wr_le32(pkt + 8, tag_w2);
    wr_le32(pkt + 12, tag_w3);

    uint32_t prim_val = PRIM_TYPE_TRIANGLE; /* flat, untextured */
    uint32_t r = 40, g = 255, b = 60; /* bright green - PACKED RGBAQ has no A field in this model, always opaque */

    uint32_t v0_x = gif->ctx1_xyoffset_x + (320u << 4), v0_y = gif->ctx1_xyoffset_y + (40u << 4);
    uint32_t v1_x = gif->ctx1_xyoffset_x + (200u << 4), v1_y = gif->ctx1_xyoffset_y + (190u << 4);
    uint32_t v2_x = gif->ctx1_xyoffset_x + (440u << 4), v2_y = gif->ctx1_xyoffset_y + (190u << 4);

    uint8_t *d = pkt + 16;
    /* reg0: AD -> TEST_1, data=0 (ATE=0,ZTE=0,...), addr=0x47 in w2 */
    wr_le32(d + 0, 0);          wr_le32(d + 4, 0);
    wr_le32(d + 8, GS_REG_TEST_1); wr_le32(d + 12, 0);
    d += 16;
    /* reg1: PRIM, w0=prim_val */
    wr_le32(d + 0, prim_val);   wr_le32(d + 4, 0); wr_le32(d + 8, 0); wr_le32(d + 12, 0);
    d += 16;
    /* reg2: RGBAQ (PACKED layout: R in w0 low byte, G in w1 low byte, B in w2 low byte) */
    wr_le32(d + 0, r);          wr_le32(d + 4, g); wr_le32(d + 8, b); wr_le32(d + 12, 0);
    d += 16;
    /* reg3-5: XYZ2 vertices, w0=X w1=Y w2=Z(unused, Z-test off) */
    wr_le32(d + 0, v0_x); wr_le32(d + 4, v0_y); wr_le32(d + 8, 0); wr_le32(d + 12, 0);
    d += 16;
    wr_le32(d + 0, v1_x); wr_le32(d + 4, v1_y); wr_le32(d + 8, 0); wr_le32(d + 12, 0);
    d += 16;
    wr_le32(d + 0, v2_x); wr_le32(d + 4, v2_y); wr_le32(d + 8, 0); wr_le32(d + 12, 0);

    uint32_t qwc = sizeof(pkt) / 16;
    gif_process_quadwords(GIF_PATH_3, pkt, qwc);
    printf("[MID] PACKED packet sent (AD/TEST_1 + PRIM + RGBAQ + 3x XYZ2) - ctx1_zte now=%d\n", gif->ctx1_zte);

    gif_state_t *gif_after = gif_get_state();
    printf("[POST] triangles=%lu lines=%lu sprites=%lu points=%lu ctx1_fbp=%u ctx1_fbw=%u\n",
           gif_after->triangles_drawn, gif_after->lines_drawn, gif_after->sprites_drawn, gif_after->points_drawn,
           gif_after->ctx1_fbp, gif_after->ctx1_fbw);
    /* Re-derive fbp/fbw from context 1 post-injection too, in case
     * apply_context_select() changed anything (it shouldn't have,
     * since our packet only ever selects context 1, but stay
     * consistent rather than assume). */
    fbp = gif_after->ctx1_fbp; fbw = gif_after->ctx1_fbw ? gif_after->ctx1_fbw : 640u;

    uint32_t centroid = gs_mem_read_psmct32(fbp, fbw, 320u, 140u);
    printf("[POST] centroid pixel (320,140) = 0x%08x (expect ~0x%08x for R=%u G=%u B=%u A=255)\n",
           centroid, (0xFFu << 24) | (b << 16) | (g << 8) | r, r, g, b);

    long post_nz = count_nonzero(fbp, fbw, 640, 224);
    printf("[POST] framebuffer(fbp=%u,fbw=%u) non_zero_pixels=%ld (delta=%ld)\n", fbp, fbw, post_nz, post_nz - pre_nz);
    dump_ppm("/tmp/round729_gt3/r731_post.ppm", fbp, fbw, 640, 224);
    printf("[DONE] dumped /tmp/round729_gt3/r731_pre.ppm and r731_post.ppm\n");

    return 0;
}
