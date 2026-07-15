/*
 * test_gs_clamp.c - host-native test for Round 98's real GS CLAMP_1/2
 * texture wrap-mode registers (task #254, 139th finding). See
 * include/core/hw/gif.h's clamp_wms/clamp_wmt/clamp_configured field
 * comments and gif.c's gs_apply_clamp_wrap() for the full scope and
 * citation (official Sony GS Users Manual "CLAMP_1/CLAMP_2: Texture
 * Wrap Mode").
 *
 * Uses a small 4-texel-wide (TW=2) synthetic texture with a distinct
 * marker color per texel, sampled via a 1x1 SPRITE in FST=1 (UV) mode
 * so exactly one texel is read per draw - same style as
 * tests/test_gs_scissor.c / tests/test_gs_fog.c.
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

#define TEX_BP 2000u
#define TEX_BW 64u

/* 4 texels, each a distinct marker color, row 0 only (V is always 0
 * in this test - only U/WMS is exercised, matching CLAMP_1's WMS/WMT
 * being independent per-axis fields with identical bit-layout shape). */
static void setup_texture(void)
{
    gs_mem_write_psmct32(TEX_BP, TEX_BW, 0, 0, 0x11111111u);
    gs_mem_write_psmct32(TEX_BP, TEX_BW, 1, 0, 0x22222222u);
    gs_mem_write_psmct32(TEX_BP, TEX_BW, 2, 0, 0x33333333u);
    gs_mem_write_psmct32(TEX_BP, TEX_BW, 3, 0, 0x44444444u);
}

/* Configures CLAMP_1 (WMS/MINU/MAXU; WMT/MINV/MAXV mirrored to 0/
 * don't-care since only U is exercised) then draws a 1x1 SPRITE at
 * framebuffer (0,0) sampling texel U (raw, not 12.4 fixed-point -
 * append_ad's GS_REG_UV case does the >>4 conversion, so pass u<<4). */
static uint32_t sample_u(uint32_t wms, uint32_t minu, uint32_t maxu, int32_t u)
{
    gs_mem_init();
    gif_init();
    setup_texture();

    uint8_t buf[16 * 9];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    write_tag(buf, &off, 8, 0xE);
    append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    /* TEX0_1: TBP0=TEX_BP, TBW=TEX_BW/64, TW=2 (4 texels wide), TH=0,
     * TFX=DECAL - same bit layout as tests/test_gif_texture.c. */
    uint32_t tex0_lo = (TEX_BP & 0x3FFFu) | (((TEX_BW / 64u) & 0x3Fu) << 14) | (2u << 26);
    uint32_t tex0_hi = (TEX_TFX_DECAL << 3);
    append_ad(buf, &off, tex0_lo, tex0_hi, GS_REG_TEX0_1);
    /* CLAMP_1: WMS in bits[1:0], WMT=0 (REPEAT, unused - V stays 0),
     * MINU in bits[13:4], MAXU in bits[23:14] (all within data_lo for
     * these small test values), MINV/MAXV=0 (unused). */
    uint32_t clamp_lo = (wms & 0x3u) | ((minu & 0x3FFu) << 4) | ((maxu & 0x3FFu) << 14);
    append_ad(buf, &off, clamp_lo, 0u, GS_REG_CLAMP_1);
    append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);
    append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ);
    append_ad(buf, &off, (uint32_t)(u << 4), 0u, GS_REG_UV);
    append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

    uint8_t buf2[16 * 3];
    int off2 = 0;
    write_tag(buf2, &off2, 2, 0xE);
    append_ad(buf2, &off2, (uint32_t)(u << 4), 0u, GS_REG_UV);
    append_ad(buf2, &off2, (uint32_t)(1 << 4), (uint32_t)(1 << 4), GS_REG_XYZ2);
    gif_process_quadwords(DMA_CHANNEL_GIF, buf2, (uint32_t)(off2 / 16));

    return gs_mem_read_psmct32(0, 640, 0, 0);
}

int main(void)
{
    /* REPEAT (WMS=00): U=5 wraps via bitmask to 5 & (4-1) = 1 -> texel1. */
    CHECK(sample_u(GS_CLAMP_REPEAT, 0, 0, 5) == 0x22222222u,
          "CLAMP REPEAT: U=5 wraps to U=1 (5 & 3) -> texel1 marker");

    /* CLAMP (WMS=01): U=10 clamps to the high end, size-1=3 -> texel3. */
    CHECK(sample_u(GS_CLAMP_CLAMP, 0, 0, 10) == 0x44444444u,
          "CLAMP CLAMP: U=10 clamps to U=3 (size-1) -> texel3 marker");

    /* REGION_CLAMP (WMS=10, MINU=1, MAXU=2): U=0 clamps up to MINU=1;
     * U=5 clamps down to MAXU=2. */
    CHECK(sample_u(GS_CLAMP_REGION_CLAMP, 1, 2, 0) == 0x22222222u,
          "CLAMP REGION_CLAMP: U=0 clamped up to MINU=1 -> texel1 marker");
    CHECK(sample_u(GS_CLAMP_REGION_CLAMP, 1, 2, 5) == 0x33333333u,
          "CLAMP REGION_CLAMP: U=5 clamped down to MAXU=2 -> texel2 marker");

    /* REGION_REPEAT (WMS=11, MINU=UMSK=1, MAXU=UFIX=2):
     * coord = (coord & UMSK) | UFIX.
     * U=5 (0b101): (5&1)|2 = 1|2 = 3 -> texel3.
     * U=4 (0b100): (4&1)|2 = 0|2 = 2 -> texel2. */
    CHECK(sample_u(GS_CLAMP_REGION_REPEAT, 1, 2, 5) == 0x44444444u,
          "CLAMP REGION_REPEAT: U=5 -> (5&1)|2=3 -> texel3 marker");
    CHECK(sample_u(GS_CLAMP_REGION_REPEAT, 1, 2, 4) == 0x33333333u,
          "CLAMP REGION_REPEAT: U=4 -> (4&1)|2=2 -> texel2 marker");

    printf(failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
