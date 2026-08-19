/* test_gs_context2_mipmap.c - host-native test for Round 29
 * continued's 15th change: making TEX1/MIPTBP1/MIPTBP2 genuinely
 * per-context (previously context-1-only, closing part of the gap
 * Round 27 explicitly left open - see include/core/hw/gif.h's
 * GS_REG_TEX1_2/MIPTBP1_2/MIPTBP2_2 comment and
 * source/hw/gif.c's gs_activate_context() for the full design).
 *
 * Strategy: configure context 1 WITH mipmapping engaged (MXL=3,
 * MMIN=2, MIPTBP1 pointing at a distinct level-3 texture) and context
 * 2 WITHOUT (TEX1_2 never written - MXL defaults to 0, mipmapping
 * disabled) against the SAME base texture and the SAME minifying
 * screen rect size. Before this round, both contexts shared the same
 * single tex1_xxx/tex_mip_tbp state, so a context-2 draw would
 * incorrectly inherit context 1's mip configuration (or vice versa,
 * depending on draw order). After this round, each context's own
 * configuration is genuinely independent: context 1's draw must
 * sample the mip level, context 2's draw must use the base level.
 */
#include <stdio.h>
#include <string.h>
#include "hw/gs_mem.c"
#include "hw/gif.c"

/* Round 640: seed texture/CLUT data via the _blk (real 256-bytes/unit
 * BITBLTBUF/TEX0-style) addressing helper, matching gif.c's gs_sample_
 * texel()/gs_sample_clut() which now read TBP0/CBP through the same
 * _blk scale. Framebuffer output reads below stay on the plain,
 * unchanged gs_mem_read_psmct32() - FBP/output addressing is untouched
 * by this round's fix. See docs/STATUS.md Round 639/640. */

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

static void append_ad(uint8_t *buf, int *off, uint32_t data_lo, uint32_t data_hi, uint32_t addr)
{
    wle32(buf + *off, data_lo);
    wle32(buf + *off + 4, data_hi);
    wle32(buf + *off + 8, addr);
    wle32(buf + *off + 12, 0);
    *off += 16;
}

static void fill_texture_solid(uint32_t bp, uint32_t bw, uint32_t w, uint32_t h, uint32_t rgba)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            gs_mem_write_psmct32_blk(bp, bw, x, y, rgba);
}

int main(void)
{
    gs_mem_init(); gif_init();
    uint32_t base_bp = 1000, base_bw = 64;
    uint32_t mip3_bp = 5000, mip3_bw = 64;
    uint32_t base_color = 0xFF0000FFu;
    uint32_t mip3_color = 0xFF00FF00u;
    fill_texture_solid(base_bp, base_bw, 64, 64, base_color);
    fill_texture_solid(mip3_bp, mip3_bw, 8, 8, mip3_color);

    uint8_t buf[16 * 40];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    /* FRAME_1/XYOFFSET_1/TEX0_1/TEX1_1/MIPTBP1_1 (5) +
     * FRAME_2/XYOFFSET_2/TEX0_2 (3, NO TEX1_2 - context 2 stays
     * unconfigured/default) + ctx1 draw (PRIM+RGBAQ+UV+XYZ2*2 = 5) +
     * ctx2 draw (5) = 18 */
    wle32(buf + off, (uint32_t)18 | (1u << 15));
    wle32(buf + off + 4, (0u << 26) | (1u << 28));
    wle32(buf + off + 8, GIF_REG_AD);
    wle32(buf + off + 12, 0);
    off += 16;

    /* --- context 1: mipmapping engaged --- */
    append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    uint32_t tex0_lo = (base_bp & 0x3FFFu) | (((base_bw / 64u) & 0x3Fu) << 14) | (6u << 26) | (2u << 30);
    uint32_t tex0_hi = (TEX_TFX_DECAL << 3) | 1u;
    append_ad(buf, &off, tex0_lo, tex0_hi, GS_REG_TEX0_1);
    append_ad(buf, &off, (3u << 2) | (2u << 10), 0, GS_REG_TEX1_1); /* MXL=3, MMIN=2 */
    uint32_t tbw3_field = mip3_bw / 64u;
    append_ad(buf, &off, 0u, ((mip3_bp & 0x3FFFu) << 8) | ((tbw3_field & 0x3Fu) << 22), GS_REG_MIPTBP1_1);

    /* --- context 2: same base texture, but NO TEX1_2 write at all -
     * mipmapping stays disabled (MXL defaults to 0). --- */
    /* Round 640: FBP occupies bits 0-8 of FRAME's data_lo (FBW is bits
     * 9-14) per gif.c's GS_REG_FRAME_2 case - this was previously
     * mis-encoded as (20u << 9), which actually wrote FBP=0/FBW=1280
     * (a latent bug that happened to stay invisible under the old
     * addressing scheme via an incidental FBW-stride separation; the
     * Round 640 TBP0/CBP real-scale fix changed downstream memory
     * layout enough to expose it as a real check failure). Corrected
     * to plain 20u so FBP really is 20, matching the ctx2_px readback
     * below and the "different target: FBP=20" intent. */
    append_ad(buf, &off, 20u, 0, GS_REG_FRAME_2);
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_2);
    append_ad(buf, &off, tex0_lo, tex0_hi, GS_REG_TEX0_2); /* same base texture as ctx1 */

    /* --- draw with context 1 (CTXT=0): 8x8 rect, 64x64 texture,
     * ratio 8 -> must sample the mip level-3 buffer. --- */
    append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);
    append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ);
    append_ad(buf, &off, (2u << 4), (2u << 4), GS_REG_UV);
    append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
    append_ad(buf, &off, (uint32_t)(8 << 4), (uint32_t)(8 << 4), GS_REG_XYZ2);

    /* --- draw with context 2 (CTXT=1): same 8x8 rect size, same
     * base texture, DIFFERENT target buffer (FBP=20) - since context
     * 2's own TEX1 was never configured, this must sample the BASE
     * level, not context 1's mip level. --- */
    append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_TME_MASK | PRIM_FST_MASK | PRIM_CTXT_MASK, 0, GS_REG_PRIM);
    append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ);
    append_ad(buf, &off, (2u << 4), (2u << 4), GS_REG_UV);
    append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
    append_ad(buf, &off, (uint32_t)(8 << 4), (uint32_t)(8 << 4), GS_REG_XYZ2);

    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

    uint32_t ctx1_px = gs_mem_read_psmct32(0, 640, 3, 3);
    /* FBP=20 -> block-based address; read back through the same
     * gs_mem addressing this project already uses elsewhere (bp=20). */
    uint32_t ctx2_px = gs_mem_read_psmct32(20, 640, 3, 3);

    CHECK(ctx1_px == mip3_color,
          "context 1 (mipmapping configured via TEX1_1/MIPTBP1_1) samples its own mip level 3, unaffected by context 2's config");
    CHECK(ctx2_px == base_color,
          "context 2 (TEX1_2 never written) uses the base level, NOT context 1's mip level - genuinely independent state");

    gif_state_t *st = gif_get_state();
    CHECK(st->ctx1_tex1_mxl == 3, "ctx1_tex1_mxl permanent storage holds context 1's own MXL=3");
    CHECK(st->ctx2_tex1_mxl == 0, "ctx2_tex1_mxl permanent storage stays 0 - context 2's TEX1 was never written");
    CHECK(st->ctx1_tex_mip_tbp[2] == mip3_bp, "ctx1_tex_mip_tbp[2] (level 3) holds context 1's configured mip buffer");
    CHECK(st->ctx2_tex_mip_tbp[2] == 0, "ctx2_tex_mip_tbp[2] stays 0 - context 2 never received a MIPTBP1_2 write");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
