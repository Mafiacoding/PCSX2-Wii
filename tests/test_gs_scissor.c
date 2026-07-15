/*
 * test_gs_scissor.c - host-native test for Round 96's real GS
 * SCISSOR_1/SCISSOR_2 clipping (previously entirely unmodeled - see
 * the Round 28 comment on GS_REG_TEX1_1 in include/core/hw/gif.h:
 * "CLAMP/TEX2/SCISSOR/FBA remain entirely unmodeled"). Bit layout and
 * register address (0x40/0x41) cross-checked against the official
 * Sony GS Users Manual ("SCISSOR_1 / SCISSOR_2 : Setting for
 * Scissoring Area"), supplied to this project this round as a
 * legitimately public, official technical manual.
 *
 * Uses the same write_tag/append_ad A+D-mode packet-building
 * convention already established in tests/test_gs_alpha.c.
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
 * on the high edge, matching rasterize_sprite()'s half-open
 * convention), optionally configuring SCISSOR_1 beforehand
 * (have_scissor=0 skips it entirely, leaving scissor_configured=0 -
 * "not configured", the real-hardware-divergent-but-safe default
 * this project deliberately chose so pre-existing tests/demos keep
 * drawing everywhere unless they opt in). */
static void draw_sprite_scissor(uint32_t rgba, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                 int have_scissor, uint32_t scax0, uint32_t scax1,
                                 uint32_t scay0, uint32_t scay1)
{
    uint8_t buf[16 * 8];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    uint32_t nloop = 6 + (have_scissor ? 1 : 0); /* FRAME_1, XYOFFSET_1, [SCISSOR_1], PRIM, RGBAQ, XYZ2, XYZ2 */
    write_tag(buf, &off, nloop, 0xE);
    append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    if (have_scissor) {
        uint32_t lo = (scax0 & 0x7FFu) | ((scax1 & 0x7FFu) << 16);
        uint32_t hi = (scay0 & 0x7FFu) | ((scay1 & 0x7FFu) << 16);
        append_ad(buf, &off, lo, hi, GS_REG_SCISSOR_1);
    }
    append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE, 0, GS_REG_PRIM);
    append_ad(buf, &off, rgba, 0, GS_REG_RGBAQ);
    append_ad(buf, &off, (uint32_t)(x0 << 4), (uint32_t)(y0 << 4), GS_REG_XYZ2);
    append_ad(buf, &off, (uint32_t)(x1 << 4), (uint32_t)(y1 << 4), GS_REG_XYZ2);

    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));
}

int main(void)
{
    { /* No SCISSOR configured at all (default, real-hardware-
       * divergent safety gate): a sprite drawn well outside any
       * "typical" scissor rect still lands everywhere it should -
       * proves this round's change is a genuine no-op for every
       * pre-existing test/demo that never touches SCISSOR. */
        gs_mem_init(); gif_init();
        draw_sprite_scissor(0xFFu << 24 | 0x00FF00u, 0, 0, 20, 20, 0, 0, 0, 0, 0);
        CHECK(gs_mem_read_psmct32(0, 640, 5, 5) == (0xFFu << 24 | 0x00FF00u),
              "no SCISSOR configured: sprite draws normally (regression safety)");
        CHECK(gs_mem_read_psmct32(0, 640, 15, 15) == (0xFFu << 24 | 0x00FF00u),
              "no SCISSOR configured: sprite draws across its whole rect, unclipped");
    }

    { /* SCISSOR_1 narrows the drawable area to [2,2]-[8,8] inclusive
       * (real hardware: SCAX0/SCAY0 upper-left, SCAX1/SCAY1 lower-
       * right, BOTH inclusive per the GS Users Manual). A sprite
       * drawn from (0,0) to (20,20) should only actually appear
       * inside that scissor rect. */
        gs_mem_init(); gif_init();
        draw_sprite_scissor(0xFFu << 24 | 0x0000FFu, 0, 0, 20, 20, 1, 2, 8, 2, 8);
        CHECK(gs_mem_read_psmct32(0, 640, 5, 5) == (0xFFu << 24 | 0x0000FFu),
              "SCISSOR [2,2]-[8,8]: pixel (5,5) inside the rect is drawn");
        CHECK(gs_mem_read_psmct32(0, 640, 0, 0) == 0u,
              "SCISSOR [2,2]-[8,8]: pixel (0,0) outside the rect (left/above) is clipped");
        CHECK(gs_mem_read_psmct32(0, 640, 15, 15) == 0u,
              "SCISSOR [2,2]-[8,8]: pixel (15,15) outside the rect (right/below) is clipped");
        CHECK(gs_mem_read_psmct32(0, 640, 8, 8) == (0xFFu << 24 | 0x0000FFu),
              "SCISSOR [2,2]-[8,8]: pixel (8,8) on the inclusive lower-right edge is drawn");
        CHECK(gs_mem_read_psmct32(0, 640, 9, 5) == 0u,
              "SCISSOR [2,2]-[8,8]: pixel (9,5) one past the inclusive edge is clipped");
    }

    { /* SCISSOR is per-context (Round 27's dual-context model) -
       * SCISSOR_1 configures context 1's rect only; a sprite drawn
       * under context 2 (PRIM.CTXT=1) without ever writing SCISSOR_2
       * must NOT inherit context 1's rect (context 2's own
       * scissor_configured stays 0 - the same safety-gate default). */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 8];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        write_tag(buf, &off, 6, 0xE);
        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_2); /* context 2's own FRAME */
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_2);
        uint32_t lo = (2u & 0x7FFu) | ((8u & 0x7FFu) << 16);
        uint32_t hi = (2u & 0x7FFu) | ((8u & 0x7FFu) << 16);
        append_ad(buf, &off, lo, hi, GS_REG_SCISSOR_1); /* only context 1's rect is set */
        append_ad(buf, &off, (uint32_t)(PRIM_TYPE_SPRITE | PRIM_CTXT_MASK), 0, GS_REG_PRIM); /* select context 2 */
        append_ad(buf, &off, 0xFFu << 24 | 0xFF0000u, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));
        /* Note: SPRITE needs 2 XYZ2 corners; append the second corner
         * in a follow-up packet using the same already-active PRIM/context. */
        {
            uint8_t buf2[16 * 2];
            int off2 = 0;
            write_tag(buf2, &off2, 1, 0xE);
            append_ad(buf2, &off2, (uint32_t)(20 << 4), (uint32_t)(20 << 4), GS_REG_XYZ2);
            gif_process_quadwords(DMA_CHANNEL_GIF, buf2, (uint32_t)(off2 / 16));
        }
        CHECK(gs_mem_read_psmct32(0, 640, 15, 15) == (0xFFu << 24 | 0xFF0000u),
              "context 2 draw without its own SCISSOR_2: pixel outside context 1's rect is NOT clipped (correct per-context isolation)");
    }

    printf(failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
