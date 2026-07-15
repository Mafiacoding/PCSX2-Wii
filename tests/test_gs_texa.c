/*
 * test_gs_texa.c - host-native test for Round 107's real GS TEXA
 * register (task #254, 148th finding). See include/core/hw/gif.h's
 * GS_REG_TEXA field comment for the full scope and citation
 * (official Sony GS Users Manual "TEXA : Texture Alpha Value
 * Setting" - only relevant for texture formats lacking a full 8-bit
 * alpha channel, RGBA16/RGB24, neither of which this codebase's
 * PSMCT32/PSMT8/PSMT4-only sampler supports).
 *
 * Uses the same write_tag/append_ad A+D-mode packet-building
 * convention already established in tests/test_gs_alpha.c /
 * tests/test_gs_fba.c, plus a DECAL-textured SPRITE sampling a
 * PSMCT32 texel with a known alpha (matching test_gif_texture.c's
 * own texturing convention).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "core/hw/gif.h"
#include "core/hw/gs_mem.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static void wle32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void write_tag(uint8_t *buf, int *off, uint32_t nloop, uint32_t regs_nibble0)
{
    uint32_t w0 = nloop & 0x7FFFu;
    uint32_t w1 = (0u << 26) | (1u << 28);
    uint32_t w2 = regs_nibble0 & 0xFu;
    uint32_t w3 = 0u;
    wle32(buf + *off, w0); wle32(buf + *off + 4, w1);
    wle32(buf + *off + 8, w2); wle32(buf + *off + 12, w3);
    *off += 16;
}

static void append_ad(uint8_t *buf, int *off, uint32_t data_lo, uint32_t data_hi, uint32_t addr)
{
    wle32(buf + *off, data_lo); wle32(buf + *off + 4, data_hi);
    wle32(buf + *off + 8, addr); wle32(buf + *off + 12, 0);
    *off += 16;
}

/* Draws a DECAL-textured SPRITE over screen (0,0)-(10,10), sampling
 * an 11x11 PSMCT32 texture where texel(x,y) carries a known,
 * distinctive alpha of (x+y) plus a fixed 0x40 base (so texel(5,5)'s
 * alpha is unambiguous: 0x40+10=0x4A), optionally writing an
 * arbitrary TEXA value first. Mirrors tests/test_gif_stq_sprite.c's
 * own SPRITE-texturing packet convention exactly (same TEX0/UV/XYZ2
 * layout) so the interpolation behavior is already proven correct by
 * that pre-existing test - this test only adds the TEXA angle.
 * Returns the midpoint (5,5) pixel's raw RGBA32 value - since DECAL
 * replaces the fragment color/alpha entirely with the sampled texel,
 * the written alpha must always be exactly the texel's own real
 * alpha (0x4A), proving TEXA never substitutes anything for this
 * codebase's PSMCT32 texture path. */
static uint32_t sample_decal_pixel(int write_texa)
{
    gs_mem_init();
    gif_init();

    uint32_t tex_bp = 3200, tex_bw = 64;
    for (uint32_t y = 0; y < 11; y++)
        for (uint32_t x = 0; x < 11; x++)
            gs_mem_write_psmct32(tex_bp, tex_bw, x, y, ((0x40u + x + y) << 24) | (y * 10u << 8) | (x * 10u));

    uint8_t buf[16 * 24];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    /* base regs: FRAME_1, XYOFFSET_1, TEX0_1, PRIM, RGBAQ, UV, XYZ2, RGBAQ, UV, XYZ2 = 10 */
    uint32_t nloop = 10 + (write_texa ? 1 : 0);
    write_tag(buf, &off, nloop, 0xE);
    append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    append_ad(buf, &off, (tex_bp & 0x3FFFu) | (((tex_bw / 64u) & 0x3Fu) << 14), (TEX_TFX_DECAL << 3), GS_REG_TEX0_1);
    if (write_texa) {
        /* Arbitrary TA0/AEM/TA1 - should have zero effect on the
         * PSMCT32 DECAL path since the format already has real alpha. */
        uint32_t ta0 = 0xAAu, aem = 1u, ta1 = 0x11u;
        uint32_t data_lo = ta0 | (aem << 15);
        uint32_t data_hi = ta1;
        append_ad(buf, &off, data_lo, data_hi, GS_REG_TEXA);
    }
    /* PRIM: SPRITE type, TME=1, FST=1 (UV mode) - identical to
     * test_gif_stq_sprite.c's own SPRITE-texturing check. */
    append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);

    uint32_t dummy_color = 0xFF7F7F7Fu;
    append_ad(buf, &off, dummy_color, 0, GS_REG_RGBAQ);
    append_ad(buf, &off, (0u << 4) | ((0u << 4) << 16), 0, GS_REG_UV);
    append_ad(buf, &off, (0u << 4), (0u << 4), GS_REG_XYZ2);
    append_ad(buf, &off, dummy_color, 0, GS_REG_RGBAQ);
    append_ad(buf, &off, (10u << 4) | ((10u << 4) << 16), 0, GS_REG_UV);
    append_ad(buf, &off, (10u << 4), (10u << 4), GS_REG_XYZ2);

    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

    return gs_mem_read_psmct32(0, 640, 5, 5);
}

int main(void)
{
    { /* No TEXA write at all: DECAL samples texel(5,5)'s own real
       * alpha (0x4A) unmodified - genuine no-op regression baseline. */
        uint32_t px = sample_decal_pixel(0);
        uint32_t a = (px >> 24) & 0xFFu;
        CHECK(a == 0x4Au,
              "no TEXA write: DECAL texel's own real alpha (0x4A) passes through unmodified (regression safety)");
    }

    { /* TEXA written with arbitrary TA0/AEM/TA1 BEFORE the draw: the
       * exact same alpha (0x4A) must still come through unmodified -
       * proves TEXA is a genuine no-op for this codebase's
       * PSMCT32-only DECAL path, per the manual's own scope (TEXA
       * only matters for RGBA16/RGB24, which this codebase doesn't
       * support). */
        uint32_t px = sample_decal_pixel(1);
        uint32_t a = (px >> 24) & 0xFFu;
        CHECK(a == 0x4Au,
              "TEXA written with arbitrary TA0/AEM/TA1: draw result unaffected (genuine no-op under PSMCT32-only scope)");
    }

    printf(failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
