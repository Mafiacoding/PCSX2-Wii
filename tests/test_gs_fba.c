/*
 * test_gs_fba.c - host-native test for Round 103's real GS FBA_1/
 * FBA_2 registers (task #254, 144th finding). See include/core/hw/
 * gif.h's GS_REG_FBA_1/GS_REG_FBA_2 field comments and gif.c's
 * gs_finish_pixel() FBA alpha-correction block for the full scope
 * and citation (official Sony GS Users Manual "FBA_1 / FBA_2 : Alpha
 * Correction Value" - A = As | (FBA<<7) for RGBA32 mode).
 *
 * Uses the same write_tag/append_ad A+D-mode packet-building
 * convention already established in tests/test_gs_alpha.c /
 * tests/test_gs_dither.c.
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

/* Draws a single 1x1 flat-shaded SPRITE with a low alpha (0x40, MSB
 * clear) at (5,5) on context 1 or 2, optionally configuring FBA_1/
 * FBA_2 = 1 first. Returns the written pixel's raw RGBA32 value so
 * the test can inspect the alpha channel's MSB directly. */
static uint32_t sample_sprite_alpha(int use_ctx2, int configure_fba, uint32_t fba_val)
{
    gs_mem_init();
    gif_init();

    uint8_t buf[16 * 16];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    uint32_t nloop = 6 + (configure_fba ? 1 : 0); /* FRAME_x, XYOFFSET_x, PRIM, RGBAQ, XYZ2, XYZ2 = 6 base */
    write_tag(buf, &off, nloop, 0xE);
    if (use_ctx2) {
        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_2);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_2);
    } else {
        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    }
    if (configure_fba) {
        append_ad(buf, &off, fba_val & 0x1u, 0,
                   use_ctx2 ? GS_REG_FBA_2 : GS_REG_FBA_1);
    }
    /* PRIM: SPRITE type (6), CTXT bit (bit 9, 0x200) selects context. */
    uint32_t prim = 6u | (use_ctx2 ? 0x200u : 0u);
    append_ad(buf, &off, prim, 0, GS_REG_PRIM);
    /* alpha=0x40 (MSB clear), color arbitrary (white RGB). */
    append_ad(buf, &off, 0x40u << 24 | 0xFFFFFFu, 0, GS_REG_RGBAQ);
    append_ad(buf, &off, (uint32_t)(5 << 4), (uint32_t)(5 << 4), GS_REG_XYZ2);
    append_ad(buf, &off, (uint32_t)(6 << 4), (uint32_t)(6 << 4), GS_REG_XYZ2);

    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

    return gs_mem_read_psmct32(0, 640, 5, 5);
}

int main(void)
{
    { /* No FBA_1/FBA_2 configured at all (default, safety gate):
       * alpha=0x40 passes through unmodified - genuine no-op
       * regression check matching every pre-existing test/demo. */
        uint32_t px = sample_sprite_alpha(0, 0, 0);
        uint32_t a = (px >> 24) & 0xFFu;
        CHECK(a == 0x40u,
              "no FBA_1 configured: alpha=0x40 passes through unmodified (regression safety)");
    }

    { /* FBA_1=1 on context 1: forces bit 7 of the written alpha on -
       * 0x40 | 0x80 = 0xC0. */
        uint32_t px = sample_sprite_alpha(0, 1, 1);
        uint32_t a = (px >> 24) & 0xFFu;
        CHECK(a == 0xC0u,
              "FBA_1=1: alpha=0x40 becomes 0xC0 (bit 7 forced on) on context 1");
    }

    { /* FBA_1=0 explicit pass-through: alpha stays 0x40. */
        uint32_t px = sample_sprite_alpha(0, 1, 0);
        uint32_t a = (px >> 24) & 0xFFu;
        CHECK(a == 0x40u,
              "FBA_1=0 explicit: alpha=0x40 stays unmodified (pass-through)");
    }

    { /* FBA_2=1 on context 2: same correction, independently
       * configurable from context 1's FBA (dual-context isolation -
       * context 1's FBA is untouched/still 0 here since this is a
       * fresh gif_init()). */
        uint32_t px = sample_sprite_alpha(1, 1, 1);
        uint32_t a = (px >> 24) & 0xFFu;
        CHECK(a == 0xC0u,
              "FBA_2=1: alpha=0x40 becomes 0xC0 (bit 7 forced on) on context 2 (dual-context)");
    }

    printf(failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
