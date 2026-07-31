/*
 * test_gs_pabe.c - host-native test for Round 104's real GS PABE
 * register (task #254, 145th finding). See include/core/hw/gif.h's
 * GS_REG_PABE field comment and gif.c's gs_finish_pixel() PABE-gating
 * check for the full scope and citation (official Sony GS Users
 * Manual "PABE : Alpha Blending Control in Units of Pixels").
 *
 * Uses the same write_tag/append_ad A+D-mode packet-building
 * convention already established in tests/test_gs_alpha.c /
 * tests/test_gs_fba.c.
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

/* Pre-fills the destination pixel at (5,5) with a known dark-blue
 * background, then draws a 1x1 flat-shaded SPRITE with ABE enabled
 * (PRIM.ABE=1), ALPHA_1 configured as a 50/50 src/dst blend, and a
 * chosen fragment alpha (frag_alpha_msb_set selects whether the
 * fragment alpha's bit 7 is set). Optionally configures PABE=1
 * first. Returns the written pixel so the test can tell whether
 * blending actually happened (mixed color) or was skipped (pure
 * fragment color, e.g. pure white passed straight through). */
static uint32_t sample_pabe(int configure_pabe, int frag_alpha_msb_set)
{
    gs_mem_init();
    gif_init();

    /* Pre-fill background at (5,5): pure black (0,0,0,0xFF alpha so
     * a later alpha-test/etc is irrelevant here - we only read RGB). */
    gs_mem_write_psmct32(0, 640, 5, 5, 0x000000FFu);

    uint8_t buf[16 * 16];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    /* base regs: FRAME_1, XYOFFSET_1, ALPHA_1, PRIM, RGBAQ, XYZ2, XYZ2 = 7 */
    uint32_t nloop = 7 + (configure_pabe ? 1 : 0);
    write_tag(buf, &off, nloop, 0xE);
    append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    /* ALPHA_1: A=Cs(0), B=Cd(1), C=Fix(2), D=Cd(1), FIX irrelevant
     * since C=Fix uses alpha_fix but we want a clean 50% look - use
     * C=As(0)/Ad(1)? Simplify: A=Cs(0),B=Cd(1),C=Fix(2),D=Cd(1),
     * FIX=0x40 (~50%) -> Color = ((Cs-Cd)*0x40)>>7 + Cd. */
    {
        uint32_t a = 0u, b = 1u, c = 2u, d = 1u, fix = 0x40u;
        uint32_t data_lo = a | (b << 2) | (c << 4) | (d << 6);
        uint32_t data_hi = fix;
        append_ad(buf, &off, data_lo, data_hi, GS_REG_ALPHA_1);
    }
    if (configure_pabe) {
        append_ad(buf, &off, 1u, 0u, GS_REG_PABE);
    }
    /* PRIM: SPRITE type (6), ABE bit (bit 6, 0x40). */
    append_ad(buf, &off, 6u | 0x40u, 0, GS_REG_PRIM);
    /* Fragment: pure white RGB, alpha MSB per param. */
    uint32_t frag_a = frag_alpha_msb_set ? 0xFFu : 0x7Fu;
    append_ad(buf, &off, frag_a << 24 | 0xFFFFFFu, 0, GS_REG_RGBAQ);
    append_ad(buf, &off, (uint32_t)(5 << 4), (uint32_t)(5 << 4), GS_REG_XYZ2);
    append_ad(buf, &off, (uint32_t)(6 << 4), (uint32_t)(6 << 4), GS_REG_XYZ2);

    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

    return gs_mem_read_psmct32(0, 640, 5, 5);
}

int main(void)
{
    { /* No PABE configured (default, safety gate): ABE=1 alone
       * blends regardless of the fragment alpha's MSB - background
       * (black) mixes with fragment (white) to a mid-gray, not pure
       * white - genuine no-op regression check. */
        uint32_t px = sample_pabe(0, 0); /* frag alpha MSB clear (0x7F) */
        uint32_t r = (px >> 16) & 0xFFu;
        CHECK(r > 0 && r < 255,
              "no PABE configured: ABE blending happens regardless of fragment alpha MSB (regression safety)");
    }

    { /* PABE=1 + fragment alpha MSB=0 (0x7F): blending is skipped -
       * pixel becomes the pure fragment color (white, r=255). */
        uint32_t px = sample_pabe(1, 0);
        uint32_t r = (px >> 16) & 0xFFu;
        CHECK(r == 255u,
              "PABE=1 + fragment alpha MSB=0: blending skipped, pure fragment color written");
    }

    { /* PABE=1 + fragment alpha MSB=1 (0xFF): blending still happens
       * normally - mixed color, not pure white. */
        uint32_t px = sample_pabe(1, 1);
        uint32_t r = (px >> 16) & 0xFFu;
        CHECK(r > 0 && r < 255,
              "PABE=1 + fragment alpha MSB=1: blending happens normally (mixed color)");
    }

    printf(failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
