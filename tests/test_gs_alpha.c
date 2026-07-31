/*
 * test_gs_alpha.c - host-native test for Round 23's real GS alpha
 * test (TEST_1's ATE, ATST, AREF, AFAIL fields) and alpha blending
 * (ALPHA_1, gated by PRIM's ABE bit). See include/core/hw/gif.h's
 * TEST_xxx, GS_ATST_xxx, GS_AFAIL_xxx, ALPHA_xxx, GS_ALPHA_xxx field
 * comments and gif.c's gs_finish_pixel() for the full scope and
 * citations (PCSX2's GS/GSRegs.h + GSDrawScanline.cpp, cross-checked
 * via a dedicated research pass this round).
 *
 * Uses SPRITE (flat, filled, axis-aligned rectangle) via this
 * project's established A+D-mode XYZ2 convention (matches every
 * pre-existing GIF test except test_z_buffer.c/test_gif_line.c,
 * which need real per-vertex Z via PACKED mode instead - not needed
 * here, since none of these checks exercise the Z buffer itself).
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

/* Draws a flat, untextured SPRITE covering [x0,y0]-[x1,y1] (exclusive
 * on the high edge, matching rasterize_sprite()'s own half-open
 * convention) with the given RGBA color, optionally configuring
 * TEST_1 (alpha test) and ALPHA_1 (blend) beforehand. Pass 0 for
 * test_lo/alpha_lo to skip configuring that register (leaving
 * whatever the previous draw in this same gif_init() left behind -
 * every call site below does a fresh gif_init() first, so 0 always
 * means "real hardware reset default: disabled"). */
static void draw_sprite_ext(uint32_t rgba, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                             int have_test, uint32_t test_lo,
                             int have_alpha, uint32_t alpha_lo, uint32_t alpha_hi)
{
    uint8_t buf[16 * 12];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    uint32_t nloop = 3 + (have_test ? 1 : 0) + (have_alpha ? 1 : 0) + 3;
    write_tag(buf, &off, nloop, 0xE);
    append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    if (have_test)
        append_ad(buf, &off, test_lo, 0, GS_REG_TEST_1);
    if (have_alpha)
        append_ad(buf, &off, alpha_lo, alpha_hi, GS_REG_ALPHA_1);
    append_ad(buf, &off, (uint32_t)(PRIM_TYPE_SPRITE | (have_alpha ? PRIM_ABE_MASK : 0u)), 0, GS_REG_PRIM);
    append_ad(buf, &off, rgba, 0, GS_REG_RGBAQ);
    append_ad(buf, &off, (uint32_t)(x0 << 4), (uint32_t)(y0 << 4), GS_REG_XYZ2);
    append_ad(buf, &off, (uint32_t)(x1 << 4), (uint32_t)(y1 << 4), GS_REG_XYZ2);

    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));
}

static uint32_t make_test_lo(int ate, uint32_t atst, uint32_t aref, uint32_t afail)
{
    return (ate ? TEST_ATE_MASK : 0u) | ((atst & TEST_ATST_MASK) << TEST_ATST_SHIFT) |
           ((aref & TEST_AREF_MASK) << TEST_AREF_SHIFT) | ((afail & TEST_AFAIL_MASK) << TEST_AFAIL_SHIFT);
}

static uint32_t make_alpha_lo(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return (a << ALPHA_A_SHIFT) | (b << ALPHA_B_SHIFT) | (c << ALPHA_C_SHIFT) | (d << ALPHA_D_SHIFT);
}

int main(void)
{
    { /* Alpha test ATST_NEVER: every fragment fails; AFAIL default
       * (KEEP=0): the pixel is left completely untouched. */
        gs_mem_init(); gif_init();
        gs_mem_write_psmct32(0, 640, 5, 5, 0x11223344u); /* pre-fill so we can prove it's untouched */
        draw_sprite_ext(0xFFu << 24 | 0xAAu, 0, 0, 10, 10,
                         1, make_test_lo(1, GS_ATST_NEVER, 0, GS_AFAIL_KEEP), 0, 0, 0);
        CHECK(gs_mem_read_psmct32(0, 640, 5, 5) == 0x11223344u,
              "ATST_NEVER + AFAIL_KEEP: pre-filled pixel is completely untouched");
        CHECK(gif_get_state()->pixels_atest_failed > 0,
              "ATST_NEVER: failed-fragment counter incremented");
    }

    { /* Alpha test ATST_GEQUAL: fragment alpha (0x80) >= AREF (0x40)
       * passes and writes normally. */
        gs_mem_init(); gif_init();
        draw_sprite_ext((0x80u << 24) | 0x00FF00u, 0, 0, 10, 10,
                         1, make_test_lo(1, GS_ATST_GEQUAL, 0x40, GS_AFAIL_KEEP), 0, 0, 0);
        CHECK(gs_mem_read_psmct32(0, 640, 5, 5) == ((0x80u << 24) | 0x00FF00u),
              "ATST_GEQUAL passing fragment (alpha 0x80 >= aref 0x40) writes normally");
    }

    { /* Alpha test ATST_GEQUAL failing case: alpha 0x20 < aref 0x40 -
       * AFAIL_KEEP discards the whole fragment. */
        gs_mem_init(); gif_init();
        gs_mem_write_psmct32(0, 640, 5, 5, 0x99887766u);
        draw_sprite_ext((0x20u << 24) | 0x00FF00u, 0, 0, 10, 10,
                         1, make_test_lo(1, GS_ATST_GEQUAL, 0x40, GS_AFAIL_KEEP), 0, 0, 0);
        CHECK(gs_mem_read_psmct32(0, 640, 5, 5) == 0x99887766u,
              "ATST_GEQUAL failing fragment (alpha 0x20 < aref 0x40) + AFAIL_KEEP leaves pixel untouched");
    }

    { /* AFAIL_FB_ONLY: failing fragment still writes color, but Z
       * write is suppressed (checked via zbuf_configured + a
       * pre-filled Z value that must survive). */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 16];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        write_tag(buf, &off, 8, 0xE);
        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, 200u, 0, GS_REG_ZBUF_1); /* real 9-bit field, far from color bp=10's drawn span */
        append_ad(buf, &off, make_test_lo(1, GS_ATST_NEVER, 0, GS_AFAIL_FB_ONLY), 0, GS_REG_TEST_1);
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0xFFu << 24 | 0x0000FFu, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(10 << 4), (uint32_t)(10 << 4), GS_REG_XYZ2);

        gs_mem_write_psmct32(200, 640, 5, 5, 777u); /* pre-existing Z value that must survive */
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        CHECK(gs_mem_read_psmct32(0, 640, 5, 5) == (0xFFu << 24 | 0x0000FFu),
              "AFAIL_FB_ONLY: color IS written even though the alpha test failed (ATST_NEVER)");
        CHECK(gs_mem_read_psmct32(200, 640, 5, 5) == 777u,
              "AFAIL_FB_ONLY: Z write was correctly suppressed (old Z value survives)");
    }

    { /* AFAIL_RGB_ONLY: failing fragment writes RGB but preserves the
       * framebuffer's OLD alpha byte. */
        gs_mem_init(); gif_init();
        gs_mem_write_psmct32(0, 640, 5, 5, 0x42000000u); /* old alpha=0x42, RGB=0 */
        draw_sprite_ext((0x00u << 24) | 0x00FF00u, 0, 0, 10, 10, /* fragment alpha=0 -> ATST_NEVER always fails anyway */
                         1, make_test_lo(1, GS_ATST_NEVER, 0, GS_AFAIL_RGB_ONLY), 0, 0, 0);
        uint32_t px = gs_mem_read_psmct32(0, 640, 5, 5);
        CHECK((px & 0x00FFFFFFu) == 0x00FF00u, "AFAIL_RGB_ONLY: RGB channels were written from the fragment");
        CHECK(((px >> 24) & 0xFFu) == 0x42u, "AFAIL_RGB_ONLY: alpha byte preserved the OLD framebuffer value (0x42), not the fragment's");
    }

    { /* Alpha blending: 50% linear blend of a red fragment over a
       * pre-filled blue background. ALPHA_1: A=Cs, B=Cd, C=Af(FIX),
       * D=Cd, FIX=64 (64/128 = 0.5) - real equation Color=((Cs-Cd)*
       * FIX)/128+Cd. Hand-computed expected result: R=127 (not 127.5 -
       * plain truncating >>7-equivalent divide, no rounding bias, per
       * gs_finish_pixel()'s citation), G=0, B=128 (not 127, since
       * (0-255)*64/128 truncates toward zero as -127, then +255). */
        gs_mem_init(); gif_init();
        gs_mem_write_psmct32(0, 640, 5, 5, (0xFFu << 24) | (0xFFu << 16) | 0x0000u); /* pre-fill: opaque blue */
        uint32_t alpha_lo = make_alpha_lo(GS_ALPHA_CS, GS_ALPHA_CD, GS_ALPHA_AFIX, GS_ALPHA_CD);
        draw_sprite_ext((0xFFu << 24) | 0x0000FFu /* opaque red */, 0, 0, 10, 10,
                         0, 0, 1, alpha_lo, 64u);
        uint32_t px = gs_mem_read_psmct32(0, 640, 5, 5);
        uint32_t r = px & 0xFFu, g = (px >> 8) & 0xFFu, b = (px >> 16) & 0xFFu, a = (px >> 24) & 0xFFu;
        CHECK(r == 127u, "alpha blend (FIX=64, 50%): red channel == 127 (hand-computed, truncating divide)");
        CHECK(g == 0u,   "alpha blend: green channel == 0 (both inputs were 0)");
        CHECK(b == 128u, "alpha blend (FIX=64, 50%): blue channel == 128 (hand-computed, truncating divide)");
        CHECK(a == 0xFFu, "alpha blend: written alpha is always the fragment's OWN source alpha (0xFF), never blended");
    }

    { /* Alpha blending gated by PRIM.ABE: with ABE=0 (not set), the
       * ALPHA_1 register may be configured but must have NO effect -
       * the fragment's own color writes verbatim. */
        gs_mem_init(); gif_init();
        gs_mem_write_psmct32(0, 640, 5, 5, (0xFFu << 24) | (0xFFu << 16) | 0x0000u);
        uint8_t buf[16 * 16];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        write_tag(buf, &off, 7, 0xE);
        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, make_alpha_lo(GS_ALPHA_CS, GS_ALPHA_CD, GS_ALPHA_AFIX, GS_ALPHA_CD), 64u, GS_REG_ALPHA_1);
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE /* no PRIM_ABE_MASK */, 0, GS_REG_PRIM);
        append_ad(buf, &off, (0xFFu << 24) | 0x0000FFu, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(10 << 4), (uint32_t)(10 << 4), GS_REG_XYZ2);
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        CHECK(gs_mem_read_psmct32(0, 640, 5, 5) == ((0xFFu << 24) | 0x0000FFu),
              "ALPHA_1 configured but PRIM.ABE=0: fragment writes verbatim (blending correctly not applied)");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
