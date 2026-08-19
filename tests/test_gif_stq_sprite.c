/* test_gif_stq_sprite.c - host-native test for task #88: perspective-
 * correct (ST+Q, PRIM's FST=0 mode) texture coordinates on triangles,
 * and texturing support for the SPRITE rasterizer. See
 * include/core/hw/gif.h's scope comment and gif.c's rasterize_sprite()
 * for exactly what's modeled and the SPRITE approximation's rationale.
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

static uint8_t chan(uint32_t rgba, int shift) { return (uint8_t)((rgba >> shift) & 0xFFu); }

static uint32_t float_bits(float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof(v));
    return v;
}

static void fill_texture_gradient_red(uint32_t bp, uint32_t bw, uint32_t w)
{
    for (uint32_t x = 0; x < w; x++)
        gs_mem_write_psmct32_blk(bp, bw, x, 0, ((uint32_t)0xFFu << 24) | (x * 20u)); /* r=x*20, rest 0, a=255 */
}

int main(void)
{
    /* --- ST+Q perspective-correct triangle: differing Q per vertex
     * must produce a genuinely different result than plain affine
     * interpolation would. Uses the exact-centroid trick: for ANY
     * triangle, the centroid (average of the 3 vertices) has
     * barycentric weights of exactly (1/3, 1/3, 1/3) - a well-known,
     * hand-verifiable geometric fact, independent of the rasterizer's
     * own edge-function internals. Vertices A=(0,0), B=(9,0), C=(0,9)
     * give an exact integer centroid (3,3). TEX0's TW/TH are both set
     * to 0 (scale factor 1<<0=1) so the chosen S values map directly
     * to texel-space units, keeping the by-hand arithmetic exact:
     *   A: s=0, q=1   B: s=9, q=1   C: s=0, q=4
     *   naive affine (ignoring Q) at centroid: (0+9+0)/3 = 3.0
     *   perspective-correct: inv_q_avg = (1+1+0.25)/3 = 0.75
     *                        s_over_q_avg = (0+9+0)/3 = 3.0
     *                        q_at_pixel = 1/0.75 = 1.3333...
     *                        s_norm = 3.0 * 1.3333... = 4.0 (exact)
     * So the correct, perspective-corrected sample is texel 4, NOT
     * texel 3 (which a plain-affine implementation would wrongly
     * produce) - this specifically distinguishes genuine 1/Q
     * perspective correction from an affine fallback. --- */
    gs_mem_init();
    gif_init();
    {
        uint32_t tex_bp = 3000, tex_bw = 64;
        fill_texture_gradient_red(tex_bp, tex_bw, 10);

        uint8_t buf[16 * (1 + 4 + 3 * 3)]; /* tag + FRAME_1/XYOFFSET_1/TEX0_1/PRIM + 3*(RGBAQ+ST+XYZ2) */
        memset(buf, 0, sizeof(buf));
        int off = 0;
        int nloop = 4 + 3 * 3;
        wle32(buf + off, (uint32_t)nloop | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        /* TW=0, TH=0: word0 bits 26-29 (TW) = 0, bits 30-31 = 0; word1 bits 0-1 (TH high bits) = 0. */
        append_ad(buf, &off, (tex_bp & 0x3FFFu) | (((tex_bw / 64u) & 0x3Fu) << 14), (TEX_TFX_DECAL << 3), GS_REG_TEX0_1);
        /* No PRIM_FST_MASK: FST=0, ST+Q mode. */
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_TRIANGLE | PRIM_TME_MASK, 0, GS_REG_PRIM);

        uint32_t dummy_color = 0xFF7F7F7Fu; /* irrelevant under DECAL */
        int32_t verts[3][2] = { { 0, 0 }, { 9, 0 }, { 0, 9 } };
        float ss[3] = { 0.0f, 9.0f, 0.0f };
        float qq[3] = { 1.0f, 1.0f, 4.0f };
        for (int i = 0; i < 3; i++) {
            /* RGBAQ: R/G/B/A in word0, Q as a real float in word1. */
            append_ad(buf, &off, dummy_color, float_bits(qq[i]), GS_REG_RGBAQ);
            /* ST: S in word0, T in word1 (both real floats; T=0 for all - unused this test). */
            append_ad(buf, &off, float_bits(ss[i]), float_bits(0.0f), GS_REG_ST);
            append_ad(buf, &off, (uint32_t)(verts[i][0] << 4), (uint32_t)(verts[i][1] << 4), GS_REG_XYZ2);
        }

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        uint32_t px = gs_mem_read_psmct32(0, 640, 3, 3); /* the exact centroid */
        CHECK(chan(px, 0) == 4u * 20u,
              "ST+Q perspective-correct: centroid samples texel 4 (genuine 1/Q divide), not texel 3 (plain affine would be wrong here)");
    }

    /* --- ST+Q with EQUAL Q at every vertex: perspective-correct math
     * must reduce to the same answer plain affine would give (a
     * sanity check that the divide/multiply round-trip is exact when
     * there's nothing to correct for). --- */
    gs_mem_init();
    gif_init();
    {
        uint32_t tex_bp = 3100, tex_bw = 64;
        fill_texture_gradient_red(tex_bp, tex_bw, 10);

        uint8_t buf[16 * (1 + 4 + 3 * 3)];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        int nloop = 4 + 3 * 3;
        wle32(buf + off, (uint32_t)nloop | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, (tex_bp & 0x3FFFu) | (((tex_bw / 64u) & 0x3Fu) << 14), (TEX_TFX_DECAL << 3), GS_REG_TEX0_1);
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_TRIANGLE | PRIM_TME_MASK, 0, GS_REG_PRIM); /* FST=0 */

        uint32_t dummy_color = 0xFF7F7F7Fu;
        int32_t verts[3][2] = { { 0, 0 }, { 9, 0 }, { 0, 9 } };
        float ss[3] = { 0.0f, 9.0f, 0.0f };
        for (int i = 0; i < 3; i++) {
            append_ad(buf, &off, dummy_color, float_bits(1.0f), GS_REG_RGBAQ); /* Q=1.0 everywhere */
            append_ad(buf, &off, float_bits(ss[i]), float_bits(0.0f), GS_REG_ST);
            append_ad(buf, &off, (uint32_t)(verts[i][0] << 4), (uint32_t)(verts[i][1] << 4), GS_REG_XYZ2);
        }

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        uint32_t px = gs_mem_read_psmct32(0, 640, 3, 3);
        CHECK(chan(px, 0) == 3u * 20u,
              "ST+Q with equal Q at every vertex: centroid samples texel 3, matching plain affine (0+9+0)/3");
    }

    /* --- SPRITE texturing, FST=1 (UV): axis-aligned bilinear
     * interpolation between the 2 corners' UV values. Identity
     * mapping (corner0 u=0,v=0 at (0,0); corner1 u=10,v=10 at
     * (10,10)) over a texture where texel(x,y) red=x*10, green=y*10 -
     * midpoint (5,5) must sample texel (5,5) exactly. --- */
    gs_mem_init();
    gif_init();
    {
        uint32_t tex_bp = 3200, tex_bw = 64;
        for (uint32_t y = 0; y < 11; y++)
            for (uint32_t x = 0; x < 11; x++)
                gs_mem_write_psmct32_blk(tex_bp, tex_bw, x, y, ((uint32_t)0xFFu << 24) | (y * 10u << 8) | (x * 10u));

        uint8_t buf[16 * (1 + 4 + 2 * 3)]; /* tag + FRAME_1/XYOFFSET_1/TEX0_1/PRIM + 2*(RGBAQ+UV+XYZ2) */
        memset(buf, 0, sizeof(buf));
        int off = 0;
        int nloop = 4 + 2 * 3;
        wle32(buf + off, (uint32_t)nloop | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, (tex_bp & 0x3FFFu) | (((tex_bw / 64u) & 0x3Fu) << 14), (TEX_TFX_DECAL << 3), GS_REG_TEX0_1);
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);

        uint32_t dummy_color = 0xFF7F7F7Fu;
        /* GS_REG_UV real packing (GIFRegUV: u16 U; u16 V; u32 _PAD -
         * BOTH U and V live in word0/data_lo, U in the low 16 bits, V
         * in the high 16 bits; word1/data_hi is unused padding). */
        append_ad(buf, &off, dummy_color, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (0u << 4) | ((0u << 4) << 16), 0, GS_REG_UV);
        append_ad(buf, &off, (0u << 4), (0u << 4), GS_REG_XYZ2);
        append_ad(buf, &off, dummy_color, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (10u << 4) | ((10u << 4) << 16), 0, GS_REG_UV);
        append_ad(buf, &off, (10u << 4), (10u << 4), GS_REG_XYZ2);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        gif_state_t *gs = gif_get_state();
        uint32_t px = gs_mem_read_psmct32(0, 640, 5, 5);
        CHECK(gs->sprites_drawn == 1, "SPRITE texturing: exactly one sprite drawn");
        CHECK(chan(px, 0) == 50 && chan(px, 8) == 50,
              "SPRITE texturing (FST=1/UV): midpoint (5,5) samples texel (5,5) via axis-aligned bilinear interpolation");
    }

    /* --- SPRITE with TME=0: still flat-colored, unaffected by the
     * new texturing code path (regression check). --- */
    gs_mem_init();
    gif_init();
    {
        uint8_t buf[16 * (1 + 3 + 2 * 2)]; /* tag + FRAME_1/XYOFFSET_1/PRIM + 2*(RGBAQ+XYZ2) */
        memset(buf, 0, sizeof(buf));
        int off = 0;
        int nloop = 3 + 2 * 2;
        wle32(buf + off, (uint32_t)nloop | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE, 0, GS_REG_PRIM); /* no TME */

        uint32_t yellow = 0xFF00FFFFu;
        append_ad(buf, &off, yellow, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (0u << 4), (0u << 4), GS_REG_XYZ2);
        append_ad(buf, &off, yellow, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (10u << 4), (10u << 4), GS_REG_XYZ2);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        uint32_t px = gs_mem_read_psmct32(0, 640, 5, 5);
        CHECK(px == yellow, "SPRITE TME=0 regression: still flat-colored, unaffected by the new texturing code path");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
