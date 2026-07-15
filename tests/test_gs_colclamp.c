/*
 * test_gs_colclamp.c - host-native test for Round 101's real GS
 * COLCLAMP register (task #254, 142nd finding). See include/core/hw/
 * gif.h's GS_REG_COLCLAMP/colclamp field comments and gif.c's
 * gs_colclamp_channel() for the full scope and citation (official
 * Sony GS Users Manual "COLCLAMP : Color Clamp Control").
 *
 * Uses a 1x1 MODULATE-textured SPRITE (TFX=0, the default - unlike
 * test_gs_clut.c's DECAL tests) so the (tex*color)/128 formula can
 * genuinely produce an out-of-[0,255]-range intermediate: a texel of
 * (255,255,255) modulated by a vertex color of (255,255,255) gives
 * 255*255/128 = 508 per channel (integer truncating division) -
 * clamps to 255 under the default CLAMP mode, but wraps to 508&0xFF
 * = 252 under MASK mode. This is the cleanest, most direct way to
 * distinguish the two real hardware modes without relying on
 * floating-point rounding edge cases.
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

#define TEX_BP 3000u
#define TEX_BW 64u

static void setup_white_texel(void)
{
    gs_mem_write_psmct32(TEX_BP, TEX_BW, 0, 0, 0xFFFFFFFFu);
}

/* Draws a 1x1 MODULATE-textured SPRITE with white texel * white vertex
 * color (both 255,255,255) - overflows to 508 per channel before any
 * clamp/mask is applied. Optionally writes COLCLAMP first. */
static uint32_t sample_modulate(int write_colclamp, uint32_t clamp_bit)
{
    gs_mem_init();
    gif_init();
    setup_white_texel();

    uint8_t buf[16 * 10];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    uint32_t nloop = 4 + (write_colclamp ? 1 : 0) + 3;
    write_tag(buf, &off, nloop, 0xE);
    append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    uint32_t tex0_lo = (TEX_BP & 0x3FFFu) | (((TEX_BW / 64u) & 0x3Fu) << 14) | (0u << 26); /* TW=0 (1 texel) */
    uint32_t tex0_hi = (TEX_TFX_MODULATE << 3);
    append_ad(buf, &off, tex0_lo, tex0_hi, GS_REG_TEX0_1);
    if (write_colclamp)
        append_ad(buf, &off, clamp_bit & 0x1u, 0u, GS_REG_COLCLAMP);
    append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);
    append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ); /* white vertex color */
    append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_UV);
    append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);

    uint8_t buf2[16 * 2];
    int off2 = 0;
    write_tag(buf2, &off2, 1, 0xE);
    append_ad(buf2, &off2, (uint32_t)(1 << 4), (uint32_t)(1 << 4), GS_REG_XYZ2);

    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));
    gif_process_quadwords(DMA_CHANNEL_GIF, buf2, (uint32_t)(off2 / 16));

    return gs_mem_read_psmct32(0, 640, 0, 0);
}

int main(void)
{
    { /* No COLCLAMP write at all (default, safety gate): the overflow
       * (255*255/128=508) clamps to 255 exactly as before this round -
       * a genuine no-op regression check for every pre-existing test/
       * demo that never touches COLCLAMP. */
        uint32_t px = sample_modulate(0, 0);
        CHECK(px == 0xFFFFFFFFu,
              "no COLCLAMP write: modulate overflow (508) clamps to 255 (regression safety)");
    }

    { /* COLCLAMP.CLAMP=1 written explicitly: same clamped behavior. */
        uint32_t px = sample_modulate(1, 1);
        CHECK(px == 0xFFFFFFFFu,
              "COLCLAMP CLAMP=1: modulate overflow clamps to 255");
    }

    { /* COLCLAMP.CLAMP=0 (MASK): the same overflow now wraps via the
       * low 8 bits: 508 & 0xFF = 252 (0xFC) per channel, NOT 255 -
       * proves MASK mode genuinely took effect. */
        uint32_t px = sample_modulate(1, 0);
        /* All 4 channels (including alpha - this project's modulate
         * formula treats alpha the same as R/G/B, and the test's
         * texel/vertex alpha is also 255) wrap identically. */
        uint32_t expect = 0xFCFCFCFCu;
        CHECK(px == expect,
              "COLCLAMP MASK (CLAMP=0): modulate overflow (508) wraps to 252 (0xFC) on all channels, not clamped to 255");
    }

    printf(failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
