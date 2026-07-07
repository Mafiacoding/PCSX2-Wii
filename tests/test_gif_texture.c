/* test_gif_texture.c - host-native test for gif.c's texturing support
 * on TRIANGLE primitives (task #85), driven by PRIM's real TME bit
 * (bit 4) and TEX0's TBP0/TBW/TFX fields - both cross-checked against
 * PCSX2's own GS/GSRegs.h (GIFRegPRIM, GIFRegTEX0). See
 * include/core/hw/gif.h's scope comment for exactly what's modeled
 * (nearest-neighbor sampling, UV-only "FST=1" coordinates, DECAL and
 * MODULATE TFX modes, no CLAMP/wrap).
 */
#include <stdio.h>
#include <string.h>
#include "hw/gs_mem.c"
#include "hw/gif.c"

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

/* Fills a solid-color rectangle directly into GS memory at (bp,bw) -
 * used to set up a synthetic "texture" without needing a separate
 * textured-upload path (this project doesn't have TRXDIR/BITBLTBUF
 * texture uploads yet - textures are just pre-existing GS memory
 * content here, exactly like the framebuffer itself). */
static void fill_texture_solid(uint32_t bp, uint32_t bw, uint32_t w, uint32_t h, uint32_t rgba)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            gs_mem_write_psmct32(bp, bw, x, y, rgba);
}

int main(void)
{
    /* --- DECAL: texture color replaces vertex/shaded color entirely --- */
    gs_mem_init();
    gif_init();
    {
        uint32_t tex_bp = 2000, tex_bw = 64;
        uint32_t tex_blue = 0xFFFF0000u; /* r=0,g=0,b=255,a=255 */
        fill_texture_solid(tex_bp, tex_bw, 8, 8, tex_blue);

        uint8_t buf[16 * (1 + 4 + 3 * 3)]; /* tag + FRAME_1/XYOFFSET_1/TEX0_1/PRIM + 3*(RGBAQ+UV+XYZ2) */
        memset(buf, 0, sizeof(buf));
        int off = 0;
        int nloop = 4 + 3 * 3; /* FRAME_1, XYOFFSET_1, TEX0_1, PRIM (4) + (RGBAQ+UV+XYZ2)*3 vertices (9) = 13 */
        wle32(buf + off, (uint32_t)nloop | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        /* TEX0_1: TBP0=tex_bp, TBW field = tex_bw/64, TFX = DECAL (bits 3-4 of word1) */
        append_ad(buf, &off, (tex_bp & 0x3FFFu) | (((tex_bw / 64u) & 0x3Fu) << 14), (TEX_TFX_DECAL << 3), GS_REG_TEX0_1);
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_TRIANGLE | PRIM_IIP_MASK | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM); /* FST=1: UV mode (task #88 added FST=0/ST+Q as an alternative) */

        uint32_t red = 0xFF0000FFu; /* vertex color - should be IGNORED under DECAL */
        int32_t verts[3][2] = { { 10, 10 }, { 60, 10 }, { 10, 60 } };
        for (int i = 0; i < 3; i++) {
            append_ad(buf, &off, red, 0, GS_REG_RGBAQ);
            append_ad(buf, &off, (2u << 4), (2u << 4), GS_REG_UV); /* same UV for all 3 verts - samples one texel everywhere */
            append_ad(buf, &off, (uint32_t)(verts[i][0] << 4), (uint32_t)(verts[i][1] << 4), GS_REG_XYZ2);
        }

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        gif_state_t *st = gif_get_state();
        CHECK(st->triangles_drawn == 1, "DECAL: exactly one triangle drawn");
        uint32_t px = gs_mem_read_psmct32(0, 640, 25, 25);
        CHECK(px == tex_blue, "DECAL: drawn pixel is the texture's blue, NOT the vertex's red - color fully replaced");
    }

    /* --- MODULATE: texture blended with shaded (flat) vertex color,
     * using the standard (tex*color)/128 per-channel formula. --- */
    gs_mem_init();
    gif_init();
    {
        uint32_t tex_bp = 3000, tex_bw = 64;
        uint32_t tex_color = ((uint32_t)255 << 24) | ((uint32_t)50 << 16) | ((uint32_t)100 << 8) | 200u; /* r=200,g=100,b=50,a=255 */
        fill_texture_solid(tex_bp, tex_bw, 8, 8, tex_color);

        uint8_t buf[16 * (1 + 4 + 3 * 3)]; /* tag + FRAME_1/XYOFFSET_1/TEX0_1/PRIM + 3*(RGBAQ+UV+XYZ2) */
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
        append_ad(buf, &off, (tex_bp & 0x3FFFu) | (((tex_bw / 64u) & 0x3Fu) << 14), (TEX_TFX_MODULATE << 3), GS_REG_TEX0_1);
        /* IIP not set (flat shading) - proves MODULATE works with flat color too, not just Gouraud. */
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_TRIANGLE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM); /* FST=1: UV mode */

        /* vertex color r=128(1.0x), g=64(0.5x), b=32(0.25x), a=128(1.0x) */
        uint32_t vcolor = ((uint32_t)128 << 24) | ((uint32_t)32 << 16) | ((uint32_t)64 << 8) | 128u;
        int32_t verts[3][2] = { { 10, 10 }, { 60, 10 }, { 10, 60 } };
        for (int i = 0; i < 3; i++) {
            append_ad(buf, &off, vcolor, 0, GS_REG_RGBAQ);
            append_ad(buf, &off, (2u << 4), (2u << 4), GS_REG_UV);
            append_ad(buf, &off, (uint32_t)(verts[i][0] << 4), (uint32_t)(verts[i][1] << 4), GS_REG_XYZ2);
        }

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        uint32_t px = gs_mem_read_psmct32(0, 640, 25, 25);
        /* Expected: r=(200*128)/128=200, g=(100*64)/128=50, b=(50*32)/128=12, a=(255*128)/128=255 */
        CHECK(chan(px, 0) == 200, "MODULATE: red channel = (200*128)/128 = 200 exactly (1.0x vertex multiplier)");
        CHECK(chan(px, 8) == 50, "MODULATE: green channel = (100*64)/128 = 50 (0.5x vertex multiplier)");
        CHECK(chan(px, 16) == 12, "MODULATE: blue channel = (50*32)/128 = 12 (0.25x vertex multiplier, truncated)");
        CHECK(chan(px, 24) == 255, "MODULATE: alpha channel = (255*128)/128 = 255 exactly");
    }

    /* --- UV interpolation: a real per-pixel-varying texture coordinate,
     * not just a single constant-UV sample - proves the barycentric UV
     * interpolation itself works, using a small horizontal-gradient
     * texture (4 distinct 1-texel-wide columns) and 3 vertices whose
     * UV values span across it. --- */
    gs_mem_init();
    gif_init();
    {
        uint32_t tex_bp = 4000, tex_bw = 64;
        uint32_t col_red   = 0xFF0000FFu;
        uint32_t col_green = 0xFF00FF00u;
        uint32_t col_blue  = 0xFFFF0000u;
        gs_mem_write_psmct32(tex_bp, tex_bw, 0, 0, col_red);
        gs_mem_write_psmct32(tex_bp, tex_bw, 5, 0, col_green);
        gs_mem_write_psmct32(tex_bp, tex_bw, 10, 0, col_blue);

        uint8_t buf[16 * (1 + 4 + 3 * 3)]; /* tag + FRAME_1/XYOFFSET_1/TEX0_1/PRIM + 3*(RGBAQ+UV+XYZ2) */
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
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_TRIANGLE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM); /* FST=1: UV mode */

        /* Right triangle (0,0)-(60,0)-(0,60), same closed-form
         * barycentric weights as test_gif_gouraud.c: vertex0 dominant
         * near (0,0), vertex1 near (60,0), vertex2 near (0,60). Map
         * vertex0->u=0 (red), vertex1->u=5 (green), vertex2->u=10 (blue). */
        uint32_t dummy_color = 0xFF7F7F7Fu; /* irrelevant under DECAL */
        int32_t verts[3][2] = { { 0, 0 }, { 60, 0 }, { 0, 60 } };
        int32_t uvs[3][2] = { { 0, 0 }, { 5, 0 }, { 10, 0 } };
        for (int i = 0; i < 3; i++) {
            append_ad(buf, &off, dummy_color, 0, GS_REG_RGBAQ);
            append_ad(buf, &off, (uint32_t)(uvs[i][0] << 4), (uint32_t)(uvs[i][1] << 4), GS_REG_UV);
            append_ad(buf, &off, (uint32_t)(verts[i][0] << 4), (uint32_t)(verts[i][1] << 4), GS_REG_XYZ2);
        }

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        /* Sample EXACTLY at each vertex's own screen coordinate -
         * unlike Gouraud color (which blends continuously), nearest-
         * neighbor texture sampling snaps to discrete texels, so a
         * point merely "near" a vertex can round to an adjacent texel
         * instead. At a vertex's own coordinate the barycentric
         * weights are exact (1,0,0) by construction (two of the three
         * edge-function terms are exactly zero when evaluated at a
         * point that IS one of the triangle's own vertices), so the
         * interpolated UV is exactly that vertex's own UV value with
         * no rounding ambiguity. */
        uint32_t at_v0 = gs_mem_read_psmct32(0, 640, 0, 0);
        uint32_t at_v1 = gs_mem_read_psmct32(0, 640, 60, 0);
        uint32_t at_v2 = gs_mem_read_psmct32(0, 640, 0, 60);
        CHECK(at_v0 == col_red, "UV interpolation: sample at vertex0's own coordinate reads back the red texel (u=0 exactly)");
        CHECK(at_v1 == col_green, "UV interpolation: sample at vertex1's own coordinate reads back the green texel (u=5 exactly)");
        CHECK(at_v2 == col_blue, "UV interpolation: sample at vertex2's own coordinate reads back the blue texel (u=10 exactly)");
    }

    /* --- Regression: TME=0 (untextured) still works exactly as
     * before - flat/Gouraud color only, texturing code path not
     * accidentally engaged. --- */
    gs_mem_init();
    gif_init();
    {
        uint8_t buf[16 * (1 + 3 + 3 * 2)]; /* tag + FRAME_1/XYOFFSET_1/PRIM + 3*(RGBAQ+XYZ2) */
        memset(buf, 0, sizeof(buf));
        int off = 0;
        int nloop = 3 + 3 * 2; /* FRAME_1, XYOFFSET_1, PRIM (3) + (RGBAQ+XYZ2)*3 vertices (6) = 9 */
        wle32(buf + off, (uint32_t)nloop | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_TRIANGLE, 0, GS_REG_PRIM); /* no TME, no IIP */

        uint32_t green = 0xFF00FF00u;
        int32_t verts[3][2] = { { 10, 10 }, { 60, 10 }, { 10, 60 } };
        for (int i = 0; i < 3; i++) {
            append_ad(buf, &off, green, 0, GS_REG_RGBAQ);
            append_ad(buf, &off, (uint32_t)(verts[i][0] << 4), (uint32_t)(verts[i][1] << 4), GS_REG_XYZ2);
        }

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        CHECK(gs_mem_read_psmct32(0, 640, 25, 25) == green, "TME=0 regression: flat triangle still draws the plain vertex color, unaffected by texturing code");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
