/*
 * test_gs_dither.c - host-native test for Round 102's real GS
 * TEXFLUSH/DTHE/DIMX registers (task #254, 143rd finding). See
 * include/core/hw/gif.h's GS_REG_TEXFLUSH/GS_REG_DTHE/GS_REG_DIMX
 * field comments and gif.c's gs_finish_pixel() dithering block for
 * the full scope and citation (official Sony GS Users Manual
 * "DTHE : Dither Control" / "DIMX : Dither Matrix Setting" /
 * "TEXFLUSH : Texture Page Buffer Disabling").
 *
 * Draws flat-colored 1x1 SPRITEs at chosen (x,y) so exactly one
 * DIMX[y%4][x%4] entry applies, per the manual's own
 * Rout=Rin+DIMX[Y%4][X%4] formula.
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

/* Draws a flat 1x1 SPRITE with color (r,g,b,255) at (x,y), optionally
 * configuring DTHE/DIMX first via a raw 16-entry table. */
static uint32_t sample(uint32_t r, uint32_t g, uint32_t b, int32_t x, int32_t y,
                        int configure_dither, uint32_t dthe, const int32_t dm[16], uint32_t texflush)
{
    gs_mem_init();
    gif_init();

    uint8_t buf[16 * 12];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    uint32_t nloop = 6 + (configure_dither ? 2 : 0) + (texflush ? 1 : 0); /* FRAME_1,XYOFFSET_1,PRIM,RGBAQ,XYZ2,XYZ2 = 6 base */
    write_tag(buf, &off, nloop, 0xE);
    append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    if (configure_dither) {
        uint32_t dimx_lo = 0, dimx_hi = 0;
        for (int i = 0; i < 8; i++) dimx_lo |= ((uint32_t)dm[i] & 0x7u) << (i * 4);
        for (int i = 0; i < 8; i++) dimx_hi |= ((uint32_t)dm[8 + i] & 0x7u) << (i * 4);
        append_ad(buf, &off, dimx_lo, dimx_hi, GS_REG_DIMX);
        append_ad(buf, &off, dthe & 0x1u, 0u, GS_REG_DTHE);
    }
    if (texflush)
        append_ad(buf, &off, 0xDEADBEEFu, 0xCAFEBABEu, GS_REG_TEXFLUSH); /* "any data" per the manual */
    append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE, 0, GS_REG_PRIM);
    append_ad(buf, &off, 0xFFu << 24 | (b << 16) | (g << 8) | r, 0, GS_REG_RGBAQ);
    append_ad(buf, &off, (uint32_t)(x << 4), (uint32_t)(y << 4), GS_REG_XYZ2);
    append_ad(buf, &off, (uint32_t)((x + 1) << 4), (uint32_t)((y + 1) << 4), GS_REG_XYZ2);

    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

    return gs_mem_read_psmct32(0, 640, (uint32_t)x, (uint32_t)y);
}

int main(void)
{
    /* DM(row)(col) = row*4+col naming: index 5 = row1,col1; index 10 = row2,col2. */
    int32_t dm[16] = { 0 };
    dm[5] = 3;   /* DM11 = +3 */
    dm[10] = -4; /* DM22 = -4 */

    { /* No DTHE/DIMX configured at all (default, safety gate): plain
       * flat color at (1,1) - a genuine no-op regression check. */
        uint32_t px = sample(100, 100, 100, 1, 1, 0, 0, dm, 0);
        CHECK(px == (0xFFu << 24 | 100u << 16 | 100u << 8 | 100u),
              "no DTHE/DIMX configured: pixel is the plain flat color (regression safety)");
    }

    { /* DIMX configured but DTHE=0 (dithering off): DIMX's own values
       * must NOT apply even though they're loaded - proves DTHE is a
       * genuine independent gate, not just "DIMX written implies
       * dithering on". */
        uint32_t px = sample(100, 100, 100, 1, 1, 1, 0, dm, 0);
        CHECK(px == (0xFFu << 24 | 100u << 16 | 100u << 8 | 100u),
              "DIMX configured, DTHE=0: dithering does not apply (DTHE is the real gate)");
    }

    { /* DTHE=1, pixel at (1,1) -> DIMX[1][1] = DM11 = +3: color 100
       * becomes 103 on R/G/B, alpha untouched. */
        uint32_t px = sample(100, 100, 100, 1, 1, 1, 1, dm, 0);
        CHECK(px == (0xFFu << 24 | 103u << 16 | 103u << 8 | 103u),
              "DTHE=1 at (1,1): DIMX[1][1]=+3 dithers 100 -> 103 on R/G/B");
    }

    { /* DTHE=1, pixel at (2,2) -> DIMX[2][2] = DM22 = -4: color 100
       * becomes 96. Also proves the Y%4/X%4 wraparound: (6,6) maps to
       * the same DIMX[2][2] entry as (2,2). */
        uint32_t px = sample(100, 100, 100, 6, 6, 1, 1, dm, 0);
        CHECK(px == (0xFFu << 24 | 96u << 16 | 96u << 8 | 96u),
              "DTHE=1 at (6,6) [wraps to DIMX[2][2] via %4]: dithers 100 -> 96");
    }

    { /* TEXFLUSH is a genuine no-op: writing it (with arbitrary data,
       * per the manual's own "any data can be written" note) changes
       * nothing about a subsequent draw. */
        uint32_t px = sample(50, 60, 70, 3, 3, 0, 0, dm, 1);
        CHECK(px == (0xFFu << 24 | 70u << 16 | 60u << 8 | 50u),
              "TEXFLUSH write: subsequent draw is completely unaffected (genuine no-op)");
    }

    printf(failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
