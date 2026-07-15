/*
 * test_gs_prmode.c - host-native test for Round 99's real GS
 * PRMODECONT/PRMODE registers (task #254, 140th finding). See
 * include/core/hw/gif.h's GS_REG_PRMODECONT/GS_REG_PRMODE field
 * comments and gif.c's gs_effective_attr_prim() for the full scope
 * and citation (official Sony GS Users Manual "PRMODE : Setting for
 * Attributes of Drawing Primitives" / "PRMODECONT : Specification of
 * Primitive Attribute Setting Method").
 *
 * Uses the same write_tag/append_ad A+D-mode packet-building
 * convention already established in tests/test_gs_alpha.c /
 * tests/test_gs_clamp.c.
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

/* Draws a 2-vertex Gouraud-shaded LINE from c0 (top-left-ish) to c1
 * (bottom-right-ish) so IIP's effect (flat vs. interpolated shading)
 * is directly observable at the line's midpoint. PRIM.IIP is always
 * set to 1 (Gouraud) in the packet; PRMODECONT/PRMODE, when supplied,
 * are configured to override it back to flat (IIP=0) via PRMODE - the
 * real hardware mechanism this round implements. */
static uint32_t sample_line_midpoint_color(int use_prmode_override)
{
    gs_mem_init();
    gif_init();

    uint8_t buf[16 * 10]; /* max case: tag + 9 registers */
    memset(buf, 0, sizeof(buf));
    int off = 0;
    uint32_t nloop = 7 + (use_prmode_override ? 2 : 0); /* FRAME_1, XYOFFSET_1, PRIM, RGBAQ, XYZ2, RGBAQ, XYZ2 = 7 base */
    write_tag(buf, &off, nloop, 0xE);
    append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    if (use_prmode_override) {
        /* PRMODE: IIP=0 (flat), all other mirrored bits left 0. */
        append_ad(buf, &off, 0u, 0u, GS_REG_PRMODE);
        /* PRMODECONT: AC=0 -> use PRMODE instead of PRIM's attribute bits. */
        append_ad(buf, &off, 0u, 0u, GS_REG_PRMODECONT);
    }
    /* PRIM: LINE type (1), IIP=1 (Gouraud) - bit 3 = 0x8. */
    append_ad(buf, &off, (uint32_t)(1u | 0x8u), 0, GS_REG_PRIM);
    append_ad(buf, &off, 0xFFu << 24 | 0xFF0000u, 0, GS_REG_RGBAQ); /* c0: pure blue */
    append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
    append_ad(buf, &off, 0xFFu << 24 | 0x0000FFu, 0, GS_REG_RGBAQ); /* c1: pure red */
    append_ad(buf, &off, (uint32_t)(10 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);

    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

    return gs_mem_read_psmct32(0, 640, 5, 0); /* midpoint of the 0..10 line */
}

int main(void)
{
    { /* No PRMODECONT configured at all (default, safety gate): PRIM's
       * own IIP=1 (Gouraud) takes effect exactly as every pre-existing
       * test/demo already expects - a genuine no-op regression check. */
        uint32_t mid = sample_line_midpoint_color(0);
        uint32_t r = (mid >> 16) & 0xFFu, b = mid & 0xFFu;
        CHECK(r > 0 && r < 255 && b > 0 && b < 255,
              "no PRMODECONT configured: PRIM.IIP=1 Gouraud-shades the line midpoint (regression safety)");
    }

    { /* PRMODECONT.AC=0 + PRMODE.IIP=0: overrides PRIM's IIP=1 back to
       * flat shading - midpoint should equal the LAST vertex's pure
       * color (c1 = pure red), not a blended mix, proving PRMODE's
       * attribute bits genuinely took over from PRIM's. */
        uint32_t mid = sample_line_midpoint_color(1);
        CHECK(mid == (0xFFu << 24 | 0x0000FFu),
              "PRMODECONT.AC=0 + PRMODE.IIP=0: overrides PRIM.IIP=1, line renders flat-shaded (last vertex color)");
    }

    { /* PRMODECONT is a real, standalone A+D register write path -
       * confirm AC itself round-trips (write AC=0 then AC=1 and check
       * gs_effective_attr_prim() semantics indirectly via the same
       * IIP-override behavior toggling back off). */
        gs_mem_init();
        gif_init();
        uint8_t buf[16 * 11]; /* tag + 10 registers */
        memset(buf, 0, sizeof(buf));
        int off = 0;
        write_tag(buf, &off, 10, 0xE);
        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, 0u, 0u, GS_REG_PRMODE);       /* IIP=0 */
        append_ad(buf, &off, 0u, 0u, GS_REG_PRMODECONT);   /* AC=0: use PRMODE */
        append_ad(buf, &off, 1u, 0u, GS_REG_PRMODECONT);   /* AC=1: back to PRIM */
        append_ad(buf, &off, (uint32_t)(1u | 0x8u), 0, GS_REG_PRIM); /* LINE, IIP=1 */
        append_ad(buf, &off, 0xFFu << 24 | 0xFF0000u, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, 0xFFu << 24 | 0x0000FFu, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (uint32_t)(10 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));
        uint32_t mid = gs_mem_read_psmct32(0, 640, 5, 0);
        uint32_t r = (mid >> 16) & 0xFFu, b = mid & 0xFFu;
        CHECK(r > 0 && r < 255 && b > 0 && b < 255,
              "PRMODECONT AC toggled back to 1: PRIM.IIP=1 Gouraud shading resumes (AC round-trips correctly)");
    }

    printf(failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
