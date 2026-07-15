/*
 * test_gs_scanmsk.c - host-native test for Round 106's real GS
 * SCANMSK register (task #254, 147th finding). See include/core/hw/
 * gif.h's GS_REG_SCANMSK field comment and gif.c's
 * scanmsk_allows_y() for the full scope and citation (official Sony
 * GS Users Manual "SCANMSK : Raster Address Mask Setting").
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

/* Draws two individual 1x1 flat-shaded SPRITEs, one at an even Y
 * (4,4)-(5,5) and one at an odd Y (4,5)-(5,6) - actually simplify:
 * draw a single-pixel SPRITE at a chosen (x,y), optionally
 * configuring SCANMSK first. Returns the drawn pixel's raw value
 * (0 if never written, since gs_mem starts zeroed and we never
 * pre-fill it - a real write always sets a non-zero alpha=0xFF). */
static uint32_t sample_pixel(uint32_t scanmsk_val, int configure_scanmsk, int32_t y)
{
    gs_mem_init();
    gif_init();

    uint8_t buf[16 * 8];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    /* base regs: FRAME_1, XYOFFSET_1, PRIM, RGBAQ, XYZ2, XYZ2 = 6 */
    uint32_t nloop = 6 + (configure_scanmsk ? 1 : 0);
    write_tag(buf, &off, nloop, 0xE);
    append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    if (configure_scanmsk) {
        append_ad(buf, &off, scanmsk_val & 0x3u, 0, GS_REG_SCANMSK);
    }
    append_ad(buf, &off, 6u, 0, GS_REG_PRIM); /* SPRITE */
    append_ad(buf, &off, 0xFFu << 24 | 0xFFFFFFu, 0, GS_REG_RGBAQ); /* white, alpha=0xFF */
    append_ad(buf, &off, (uint32_t)(4 << 4), (uint32_t)(y << 4), GS_REG_XYZ2);
    append_ad(buf, &off, (uint32_t)(5 << 4), (uint32_t)((y + 1) << 4), GS_REG_XYZ2);

    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

    return gs_mem_read_psmct32(0, 640, 4, y);
}

int main(void)
{
    { /* No SCANMSK configured (default, safety gate): pixel at even
       * Y=10 draws normally - genuine no-op regression check. */
        uint32_t px = sample_pixel(0, 0, 10);
        CHECK(px != 0u, "no SCANMSK configured: pixel at even Y draws normally (regression safety)");
    }

    { /* SCANMSK MSK=2 (prohibit even Y): pixel at even Y=10 must NOT
       * be drawn. */
        uint32_t px = sample_pixel(2, 1, 10);
        CHECK(px == 0u, "SCANMSK MSK=2: pixel at even Y is NOT drawn (prohibited)");
    }

    { /* SCANMSK MSK=2 (prohibit even Y): pixel at odd Y=11 must still
       * draw normally - proves the mask is Y-parity-specific, not a
       * blanket suppression. */
        uint32_t px = sample_pixel(2, 1, 11);
        CHECK(px != 0u, "SCANMSK MSK=2: pixel at odd Y still draws normally");
    }

    { /* SCANMSK MSK=3 (prohibit odd Y): pixel at odd Y=11 must NOT be
       * drawn. */
        uint32_t px = sample_pixel(3, 1, 11);
        CHECK(px == 0u, "SCANMSK MSK=3: pixel at odd Y is NOT drawn (prohibited)");
    }

    { /* SCANMSK MSK=3 (prohibit odd Y): pixel at even Y=10 still
       * draws normally. */
        uint32_t px = sample_pixel(3, 1, 10);
        CHECK(px != 0u, "SCANMSK MSK=3: pixel at even Y still draws normally");
    }

    printf(failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
