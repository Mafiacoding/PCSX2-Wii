/* test_gs_clut.c - host-native test for Round 24's CLUT/paletted
 * texture support (PSMT8/PSMT4), driven by TEX0's PSM/CBP/CPSM/CSA
 * fields. See include/core/hw/gif.h's TEX_PSM_xxx/CLUT_ROW_WIDTH/
 * CLUT_CSA_UNIT comments and source/hw/gif.c's gs_sample_texel()/
 * gs_sample_clut() for the full scope, addressing scheme, and this
 * round's citation-honesty note (live source-fetch research hit a
 * session limit before it could run - the CLUT addressing scheme and
 * the PSMT8 CSM1 index-swizzle are sourced from established PS2 GS
 * knowledge rather than a fresh citation trail this round).
 *
 * Uses TRIANGLE + UV-mode (FST=1) texturing, reusing the same
 * single-shared-UV-across-all-3-vertices convention established in
 * test_gif_texture.c's own DECAL/MODULATE tests - a deliberate
 * simplification that samples exactly one texel across the whole
 * triangle, avoiding any per-pixel-interpolation noise since this
 * test cares about CLUT lookup correctness, not coordinate math
 * (already covered by test_gif_texture.c/test_gif_stq_sprite.c).
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

/* Writes CLUT entry `flat_index` (0-based, CSA already folded in by
 * the caller) at CLUT storage base `cbp`, per this project's
 * CLUT_ROW_WIDTH-wide row convention (see gif.h). */
static void fill_clut_entry(uint32_t cbp, uint32_t flat_index, uint32_t rgba)
{
    gs_mem_write_psmct32_blk(cbp, CLUT_ROW_WIDTH, flat_index % CLUT_ROW_WIDTH, flat_index / CLUT_ROW_WIDTH, rgba);
}

/* Fills a solid rectangle of raw palette-index values into gs_mem, at
 * the texture's own (bp,bw) - mirroring this project's established
 * "no real texture-upload path yet, gs_mem_write_psmct32 populates
 * textures directly" convention (see test_gif_texture.c). */
static void fill_texture_index(uint32_t bp, uint32_t bw, uint32_t w, uint32_t h, uint32_t index)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            gs_mem_write_psmct32_blk(bp, bw, x, y, index);
}

/* Draws a single flat, DECAL-textured triangle sampling exactly one
 * texel (same UV at all 3 vertices), with TEX0 fully configured
 * (PSM/CBP/CPSM/CSA). Framebuffer fixed at bp=0/bw=640 (FRAME_1
 * FBW field=10 -> guarded default 640, FBP field=0), matching this
 * project's other GS tests' convention. */
static void draw_clut_triangle(uint32_t tex_bp, uint32_t tex_bw, uint32_t psm,
                                uint32_t cbp, uint32_t cpsm, uint32_t csa)
{
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

    uint32_t tex0_lo = (tex_bp & 0x3FFFu) | (((tex_bw / 64u) & 0x3Fu) << 14) | ((psm & 0x3Fu) << 20);
    uint32_t tex0_hi = (TEX_TFX_DECAL << 3) | ((cbp & 0x3FFFu) << 5) | ((cpsm & 0xFu) << 19) | ((csa & 0x1Fu) << 24);
    append_ad(buf, &off, tex0_lo, tex0_hi, GS_REG_TEX0_1);
    append_ad(buf, &off, (uint32_t)PRIM_TYPE_TRIANGLE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);

    uint32_t dummy_color = 0xFF000000u; /* should be fully replaced by DECAL */
    int32_t verts[3][2] = { { 10, 10 }, { 60, 10 }, { 10, 60 } };
    for (int i = 0; i < 3; i++) {
        append_ad(buf, &off, dummy_color, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (2u << 4), (2u << 4), GS_REG_UV);
        append_ad(buf, &off, (uint32_t)(verts[i][0] << 4), (uint32_t)(verts[i][1] << 4), GS_REG_XYZ2);
    }

    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));
}

int main(void)
{
    { /* PSMT4: 16-entry palette, no swizzle needed. Texture holds raw
       * index 5 everywhere; CLUT entry 5 (at CBP, CSA=0) is a known
       * color - sampled pixel must match it exactly. */
        gs_mem_init(); gif_init();
        uint32_t cbp = 5000, tex_bp = 5100, tex_bw = 64;
        uint32_t expect = 0xFFAA5511u;
        for (uint32_t i = 0; i < 16; i++)
            fill_clut_entry(cbp, i, 0xFF000000u | (i * 0x010101u)); /* distinct filler per entry */
        fill_clut_entry(cbp, 5, expect);
        fill_texture_index(tex_bp, tex_bw, 8, 8, 5);

        draw_clut_triangle(tex_bp, tex_bw, TEX_PSM_PSMT4, cbp, TEX_PSM_PSMCT32, 0);
        uint32_t px = gs_mem_read_psmct32(0, 640, 25, 25);
        CHECK(px == expect, "PSMT4: sampled pixel matches CLUT entry 5's known color");
    }

    { /* PSMT4 with CSA=2: a second, independent 16-entry palette
       * lives at flat offset CSA*16=32 in the SAME CLUT storage
       * region - proves CSA actually selects a different bank rather
       * than being ignored. */
        gs_mem_init(); gif_init();
        uint32_t cbp = 6000, tex_bp = 6100, tex_bw = 64;
        uint32_t bank0_color = 0xFF111111u;
        uint32_t bank2_color = 0xFF999999u;
        fill_clut_entry(cbp, 5, bank0_color);       /* CSA=0 bank, entry 5 */
        fill_clut_entry(cbp, 2 * 16 + 5, bank2_color); /* CSA=2 bank, entry 5 */
        fill_texture_index(tex_bp, tex_bw, 8, 8, 5);

        draw_clut_triangle(tex_bp, tex_bw, TEX_PSM_PSMT4, cbp, TEX_PSM_PSMCT32, 2);
        uint32_t px = gs_mem_read_psmct32(0, 640, 25, 25);
        CHECK(px == bank2_color, "PSMT4 CSA=2: samples the SECOND palette bank, not CSA=0's");
    }

    { /* PSMT8, index 8: real CSM1 swizzle swaps index bits 3/4, so
       * raw texture index 8 (binary 00001000) must resolve to CLUT
       * entry 16 (00010000), NOT entry 8 - proves the swizzle is
       * actually applied, not just a straight passthrough. */
        gs_mem_init(); gif_init();
        uint32_t cbp = 7000, tex_bp = 7200, tex_bw = 64;
        uint32_t entry8_color  = 0xFF0000FFu; /* decoy - must NOT be selected */
        uint32_t entry16_color = 0xFF00FF00u; /* expected */
        fill_clut_entry(cbp, 8, entry8_color);
        fill_clut_entry(cbp, 16, entry16_color);
        fill_texture_index(tex_bp, tex_bw, 8, 8, 8);

        draw_clut_triangle(tex_bp, tex_bw, TEX_PSM_PSMT8, cbp, TEX_PSM_PSMCT32, 0);
        uint32_t px = gs_mem_read_psmct32(0, 640, 25, 25);
        CHECK(px == entry16_color, "PSMT8: raw index 8 resolves through the CSM1 swizzle to CLUT entry 16, not entry 8");
    }

    { /* PSMT8, index 16: the symmetric swap (16 -> 8), confirming the
       * swizzle is a true bit-swap and not a one-directional quirk. */
        gs_mem_init(); gif_init();
        uint32_t cbp = 8000, tex_bp = 8200, tex_bw = 64;
        uint32_t entry8_color  = 0xFF0000FFu; /* expected */
        uint32_t entry16_color = 0xFF00FF00u; /* decoy - must NOT be selected */
        fill_clut_entry(cbp, 8, entry8_color);
        fill_clut_entry(cbp, 16, entry16_color);
        fill_texture_index(tex_bp, tex_bw, 8, 8, 16);

        draw_clut_triangle(tex_bp, tex_bw, TEX_PSM_PSMT8, cbp, TEX_PSM_PSMCT32, 0);
        uint32_t px = gs_mem_read_psmct32(0, 640, 25, 25);
        CHECK(px == entry8_color, "PSMT8: raw index 16 resolves through the CSM1 swizzle to CLUT entry 8 (symmetric swap)");
    }

    { /* PSMT8, an index untouched by the swizzle (e.g. index 3, bits
       * 3/4 both 0): must resolve to itself unchanged. */
        gs_mem_init(); gif_init();
        uint32_t cbp = 9000, tex_bp = 9200, tex_bw = 64;
        uint32_t expect = 0xFF123456u;
        fill_clut_entry(cbp, 3, expect);
        fill_texture_index(tex_bp, tex_bw, 8, 8, 3);

        draw_clut_triangle(tex_bp, tex_bw, TEX_PSM_PSMT8, cbp, TEX_PSM_PSMCT32, 0);
        uint32_t px = gs_mem_read_psmct32(0, 640, 25, 25);
        CHECK(px == expect, "PSMT8: an index with both swizzle bits clear (3) is unaffected by the swap");
    }

    { /* Regression: PSMCT32 (PSM left at its default 0) is completely
       * unaffected by any of the new CLUT machinery - direct texel
       * color sampling, exactly like every pre-existing texture
       * test. */
        gs_mem_init(); gif_init();
        uint32_t tex_bp = 10200, tex_bw = 64;
        uint32_t direct_color = 0xFFCAFEBAu;
        fill_texture_index(tex_bp, tex_bw, 8, 8, direct_color); /* "index" fill is really just a raw write here */

        draw_clut_triangle(tex_bp, tex_bw, TEX_PSM_PSMCT32, 0, 0, 0);
        uint32_t px = gs_mem_read_psmct32(0, 640, 25, 25);
        CHECK(px == direct_color, "PSMCT32 regression: default PSM samples the texel directly, CLUT path not engaged");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
