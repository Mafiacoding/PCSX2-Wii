/* test_gs_tex2.c - host-native test for Round 100's real GS TEX2_1/
 * TEX2_2 registers (task #254, 141st finding). See include/core/hw/
 * gif.h's GS_REG_TEX2_1/GS_REG_TEX2_2 comment and gif.c's
 * apply_ad_write() TEX2 case for the full scope and citation
 * (official Sony GS Users Manual "TEX2_1 / TEX2_2 : Texture
 * Information Setting" - "These registers set texture information.
 * They are subsets of the TEX0 register.").
 *
 * Reuses test_gs_clut.c's exact draw_clut_triangle/fill_clut_entry/
 * fill_texture_index helpers and direct-include compile convention,
 * since this test is really "configure via TEX0 once, then swap only
 * the CLUT via TEX2" - a direct extension of the existing CLUT test.
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

static void fill_clut_entry(uint32_t cbp, uint32_t flat_index, uint32_t rgba)
{
    gs_mem_write_psmct32(cbp, CLUT_ROW_WIDTH, flat_index % CLUT_ROW_WIDTH, flat_index / CLUT_ROW_WIDTH, rgba);
}

static void fill_texture_index(uint32_t bp, uint32_t bw, uint32_t w, uint32_t h, uint32_t index)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            gs_mem_write_psmct32(bp, bw, x, y, index);
}

/* Draws a single flat, DECAL-textured triangle sampling exactly one
 * texel, first configuring TEX0 fully (as usual), then OPTIONALLY
 * writing TEX2_1 to override just the CLUT fields (new_cbp/new_cpsm/
 * new_csa) - same packet-building convention as test_gs_clut.c's
 * draw_clut_triangle(), just with an extra register slot. */
static void draw_clut_triangle_tex2(uint32_t tex_bp, uint32_t tex_bw, uint32_t psm,
                                     uint32_t cbp, uint32_t cpsm, uint32_t csa,
                                     int use_tex2, uint32_t new_cbp, uint32_t new_cpsm, uint32_t new_csa)
{
    uint8_t buf[16 * (1 + 5 + 3 * 3)];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    int nloop = 4 + (use_tex2 ? 1 : 0) + 3 * 3;
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

    if (use_tex2) {
        /* TEX2_1: same PSM/CBP/CPSM/CSA bit positions as TEX0, but
         * TBP0/TBW/TW/TH bits (data_lo bits 0-19, 26-31) are simply
         * left at 0 here to prove they're genuinely IGNORED by the
         * real TEX2 parser - if they were mistakenly applied, the
         * texture's base pointer/size would be corrupted and the
         * sample would come back wrong/black instead of the new
         * CLUT's color. */
        uint32_t tex2_lo = (psm & 0x3Fu) << 20;
        uint32_t tex2_hi = (new_cbp & 0x3FFFu) << 5 | ((new_cpsm & 0xFu) << 19) | ((new_csa & 0x1Fu) << 24);
        append_ad(buf, &off, tex2_lo, tex2_hi, GS_REG_TEX2_1);
    }

    append_ad(buf, &off, (uint32_t)PRIM_TYPE_TRIANGLE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);

    uint32_t dummy_color = 0xFF000000u;
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
    { /* No TEX2 write at all (default/regression safety): TEX0's own
       * CBP is used exactly as before this round - a genuine no-op
       * for every pre-existing test/demo. */
        gs_mem_init(); gif_init();
        uint32_t cbp = 5000, tex_bp = 5100, tex_bw = 64;
        uint32_t expect = 0xFFAA5511u;
        fill_clut_entry(cbp, 5, expect);
        fill_texture_index(tex_bp, tex_bw, 8, 8, 5);
        draw_clut_triangle_tex2(tex_bp, tex_bw, TEX_PSM_PSMT4, cbp, TEX_PSM_PSMCT32, 0,
                                 0, 0, 0, 0);
        uint32_t px = gs_mem_read_psmct32(0, 640, 25, 25);
        CHECK(px == expect, "no TEX2 write: TEX0's own CBP used (regression safety)");
    }

    { /* TEX2_1 overrides CBP to a second, independent palette buffer -
       * same texture (tex_bp/tex_bw unchanged, same raw index 5 in
       * the texture), but the sampled color must now come from the
       * NEW palette, proving TEX2's CBP override genuinely took
       * effect over TEX0's original CBP. */
        gs_mem_init(); gif_init();
        uint32_t cbp_a = 6000, cbp_b = 6500, tex_bp = 6100, tex_bw = 64;
        uint32_t color_a = 0xFF111111u;
        uint32_t color_b = 0xFF999999u;
        fill_clut_entry(cbp_a, 5, color_a);
        fill_clut_entry(cbp_b, 5, color_b);
        fill_texture_index(tex_bp, tex_bw, 8, 8, 5);
        draw_clut_triangle_tex2(tex_bp, tex_bw, TEX_PSM_PSMT4, cbp_a, TEX_PSM_PSMCT32, 0,
                                 1, cbp_b, TEX_PSM_PSMCT32, 0);
        uint32_t px = gs_mem_read_psmct32(0, 640, 25, 25);
        CHECK(px == color_b, "TEX2_1 CBP override: samples the NEW palette (cbp_b), not TEX0's original (cbp_a)");
    }

    { /* TEX2_1's CSA override: same CBP, but a different CLUT bank
       * (CSA=2 instead of CSA=0's original) - proves TEX2 can change
       * CSA independently too, not just CBP. */
        gs_mem_init(); gif_init();
        uint32_t cbp = 7000, tex_bp = 7100, tex_bw = 64;
        uint32_t bank0_color = 0xFF010101u;
        uint32_t bank2_color = 0xFF020202u;
        fill_clut_entry(cbp, 5, bank0_color);
        fill_clut_entry(cbp, 2 * 16 + 5, bank2_color);
        fill_texture_index(tex_bp, tex_bw, 8, 8, 5);
        draw_clut_triangle_tex2(tex_bp, tex_bw, TEX_PSM_PSMT4, cbp, TEX_PSM_PSMCT32, 0,
                                 1, cbp, TEX_PSM_PSMCT32, 2);
        uint32_t px = gs_mem_read_psmct32(0, 640, 25, 25);
        CHECK(px == bank2_color, "TEX2_1 CSA override: samples bank 2, not TEX0's original CSA=0 bank");
    }

    printf(failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
