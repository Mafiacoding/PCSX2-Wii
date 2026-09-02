/* test_gs_mipmap_triangle.c - host-native test for Round 29
 * continued's 14th change: extending Round 28's mipmap support
 * (previously SPRITE-only) to TRIANGLE. See
 * source/hw/gif.c's rasterize_triangle() mip-level-selection block
 * (added this round, mirroring rasterize_sprite()'s own) for the
 * scope/citation - same honest simplifications: per-PRIMITIVE (not
 * per-pixel/trilinear) LOD selection, only MTBA=0 (explicit MIPTBP)
 * is modeled, and the triangle's screen-space BOUNDING BOX
 * (maxx-minx, maxy-miny) stands in for SPRITE's well-defined
 * width/height (a triangle has no single natural "size" the way an
 * axis-aligned SPRITE rectangle does).
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

/* Builds and processes a solid-UV (constant texel everywhere),
 * DECAL-textured triangle with bounding box [0,0]-(size,size), using
 * whatever TEX0/TEX1/MIPTBP1 state the caller already configured. */
static void draw_test_triangle(int32_t size)
{
    uint8_t buf[16 * 20];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    int nloop = 1 + 3 * 3; /* PRIM + (RGBAQ+UV+XYZ2) * 3 vertices */
    wle32(buf + off, (uint32_t)nloop | (1u << 15));
    wle32(buf + off + 4, (0u << 26) | (1u << 28));
    wle32(buf + off + 8, GIF_REG_AD);
    wle32(buf + off + 12, 0);
    off += 16;

    append_ad(buf, &off, (uint32_t)PRIM_TYPE_TRIANGLE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);
    int32_t verts[3][2] = { { 0, 0 }, { size, 0 }, { 0, size } };
    for (int i = 0; i < 3; i++) {
        append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (2u << 4), (2u << 4), GS_REG_UV); /* same texel everywhere */
        append_ad(buf, &off, (uint32_t)(verts[i][0] << 4), (uint32_t)(verts[i][1] << 4), GS_REG_XYZ2);
    }
    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));
}

int main(void)
{
    { /* Computed LOD: 64x64 base texture, triangle bounding box 8x8
       * (ratio 8, log2=3) - must sample mip level 3, not the base. */
        gs_mem_init(); gif_init();
        uint32_t base_bp = 1000, base_bw = 64;
        uint32_t mip3_bp = 5000, mip3_bw = 64;
        uint32_t base_color = 0xFF0000FFu;
        uint32_t mip3_color = 0xFF00FF00u;
        fill_texture_solid(base_bp, base_bw, 64, 64, base_color);
        fill_texture_solid(mip3_bp, mip3_bw, 8, 8, mip3_color);

        uint8_t buf[16 * 8];
        int off = 0;
        wle32(buf + off, (uint32_t)5 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;
        append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        uint32_t tex0_lo = (base_bp & 0x3FFFu) | (((base_bw / 64u) & 0x3Fu) << 14) | (6u << 26) | (2u << 30);
        uint32_t tex0_hi = (TEX_TFX_DECAL << 3) | 1u;
        append_ad(buf, &off, tex0_lo, tex0_hi, GS_REG_TEX0_1);
        append_ad(buf, &off, (3u << 2) | (2u << 10), 0, GS_REG_TEX1_1); /* MXL=3, MMIN=2 */
        uint32_t tbw3_field = mip3_bw / 64u;
        append_ad(buf, &off, 0u, ((mip3_bp & 0x3FFFu) << 8) | ((tbw3_field & 0x3Fu) << 22), GS_REG_MIPTBP1_1);
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        draw_test_triangle(8);

        uint32_t px = gs_mem_read_psmct32(0, 640, 3, 3);
        CHECK(px == mip3_color,
              "TRIANGLE computed LOD: 64x64 texture, 8x8 bounding box (ratio 8, log2=3) samples mip level 3, not base");
    }

    { /* MXL clamp: same setup (would compute LOD 3), but MXL=1 - must
       * clamp down to level 1's buffer. */
        gs_mem_init(); gif_init();
        uint32_t base_bp = 1000, base_bw = 64;
        uint32_t mip1_bp = 3000, mip1_bw = 64;
        uint32_t base_color = 0xFF0000FFu;
        uint32_t mip1_color = 0xFFFF00FFu;
        fill_texture_solid(base_bp, base_bw, 64, 64, base_color);
        fill_texture_solid(mip1_bp, mip1_bw, 32, 32, mip1_color);

        uint8_t buf[16 * 8];
        int off = 0;
        wle32(buf + off, (uint32_t)5 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;
        append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        uint32_t tex0_lo = (base_bp & 0x3FFFu) | (((base_bw / 64u) & 0x3Fu) << 14) | (6u << 26) | (2u << 30);
        uint32_t tex0_hi = (TEX_TFX_DECAL << 3) | 1u;
        append_ad(buf, &off, tex0_lo, tex0_hi, GS_REG_TEX0_1);
        append_ad(buf, &off, (1u << 2) | (2u << 10), 0, GS_REG_TEX1_1); /* MXL=1 this time */
        uint32_t tbw1_field = mip1_bw / 64u;
        append_ad(buf, &off, (mip1_bp & 0x3FFFu) | ((tbw1_field & 0x3Fu) << 14), 0, GS_REG_MIPTBP1_1);
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        draw_test_triangle(8);

        uint32_t px = gs_mem_read_psmct32(0, 640, 3, 3);
        CHECK(px == mip1_color, "TRIANGLE MXL clamp: computed LOD 3 clamps down to MXL=1, sampling level 1's buffer");
    }

    { /* Magnification (texture SMALLER than the triangle's bounding
       * box): must always use the base level. */
        gs_mem_init(); gif_init();
        uint32_t base_bp = 1000, base_bw = 64;
        uint32_t mip1_bp = 3000, mip1_bw = 64;
        uint32_t base_color = 0xFF0000FFu;
        uint32_t mip1_color = 0xFFFF00FFu; /* must NOT be sampled */
        fill_texture_solid(base_bp, base_bw, 8, 8, base_color);
        fill_texture_solid(mip1_bp, mip1_bw, 4, 4, mip1_color);

        uint8_t buf[16 * 8];
        int off = 0;
        wle32(buf + off, (uint32_t)5 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;
        append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        uint32_t tex0_lo = (base_bp & 0x3FFFu) | (((base_bw / 64u) & 0x3Fu) << 14) | (3u << 26) | (0u << 30);
        uint32_t tex0_hi = (TEX_TFX_DECAL << 3) | 0u;
        append_ad(buf, &off, tex0_lo, tex0_hi, GS_REG_TEX0_1);
        append_ad(buf, &off, (3u << 2) | (2u << 10), 0, GS_REG_TEX1_1); /* MXL=3, MMIN=2 */
        uint32_t tbw1_field = mip1_bw / 64u;
        append_ad(buf, &off, (mip1_bp & 0x3FFFu) | ((tbw1_field & 0x3Fu) << 14), 0, GS_REG_MIPTBP1_1);
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        draw_test_triangle(64); /* huge bounding box relative to the 8x8 texture - magnification */

        uint32_t px = gs_mem_read_psmct32(0, 640, 20, 20);
        CHECK(px == base_color, "TRIANGLE magnification: texture smaller than bounding box always uses base level");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
