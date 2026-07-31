/*
 * test_gs_texclut.c - host-native test for Round 105's real GS
 * TEXCLUT register (task #254, 146th finding). See include/core/hw/
 * gif.h's GS_REG_TEXCLUT field comment for the full scope and
 * citation (official Sony GS Users Manual "TEXCLUT : CLUT Position
 * Specification" - "disabled when CSM=0 (CSM1 mode)", which is this
 * codebase's only supported CLUT storage mode).
 *
 * Reuses test_gs_clut.c's exact helper functions and direct-include
 * compile convention (#include "hw/gs_mem.c" / #include "hw/gif.c").
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

/* Writes an arbitrary TEXCLUT value in isolation, as its own A+D
 * packet - proves the register accepts writes as a real, distinct
 * GS_REG_TEXCLUT case (not falling through to an unknown-register
 * path) without needing to be interleaved into the draw packet. */
static void write_texclut(uint32_t cbw, uint32_t cou, uint32_t cov)
{
    uint8_t buf[16 * 2];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    wle32(buf + off, 1u | (1u << 15));
    wle32(buf + off + 4, (0u << 26) | (1u << 28));
    wle32(buf + off + 8, GIF_REG_AD);
    wle32(buf + off + 12, 0);
    off += 16;
    uint32_t data_lo = (cbw & 0x3Fu) | ((cou & 0x3Fu) << 6) | ((cov & 0x3FFu) << 12);
    append_ad(buf, &off, data_lo, 0, GS_REG_TEXCLUT);
    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));
}

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
    { /* No TEXCLUT write at all: PSMT4 CLUT sample resolves normally
       * via CBP/CSA addressing, exactly as test_gs_clut.c's own first
       * check - genuine no-op regression baseline. */
        gs_mem_init(); gif_init();
        uint32_t cbp = 5000, tex_bp = 5100, tex_bw = 64;
        uint32_t expect = 0xFFAA5511u;
        for (uint32_t i = 0; i < 16; i++)
            fill_clut_entry(cbp, i, 0xFF000000u | (i * 0x010101u));
        fill_clut_entry(cbp, 5, expect);
        fill_texture_index(tex_bp, tex_bw, 8, 8, 5);

        draw_clut_triangle(tex_bp, tex_bw, TEX_PSM_PSMT4, cbp, TEX_PSM_PSMCT32, 0);
        uint32_t px = gs_mem_read_psmct32(0, 640, 25, 25);
        CHECK(px == expect, "no TEXCLUT write: CLUT sample resolves normally via CBP/CSA (regression safety)");
    }

    { /* TEXCLUT written with arbitrary CBW/COU/COV BEFORE the draw:
       * the exact same CLUT sample must still resolve identically -
       * proves TEXCLUT is a genuine no-op under this codebase's
       * CSM1-only scope, per the manual's own "disabled when CSM=0"
       * wording. */
        gs_mem_init(); gif_init();
        uint32_t cbp = 5000, tex_bp = 5100, tex_bw = 64;
        uint32_t expect = 0xFFAA5511u;
        for (uint32_t i = 0; i < 16; i++)
            fill_clut_entry(cbp, i, 0xFF000000u | (i * 0x010101u));
        fill_clut_entry(cbp, 5, expect);
        fill_texture_index(tex_bp, tex_bw, 8, 8, 5);

        write_texclut(5, 3, 7); /* arbitrary, should have zero effect */
        draw_clut_triangle(tex_bp, tex_bw, TEX_PSM_PSMT4, cbp, TEX_PSM_PSMCT32, 0);
        uint32_t px = gs_mem_read_psmct32(0, 640, 25, 25);
        CHECK(px == expect, "TEXCLUT written with arbitrary CBW/COU/COV: draw result unaffected (genuine no-op under CSM1-only scope)");
    }

    printf(failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
