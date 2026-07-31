/* test_gs_mipmap.c - host-native test for Round 28's mipmap support
 * (TEX1 + MIPTBP1/MIPTBP2 registers, SPRITE-only per-primitive LOD
 * selection). See include/core/hw/gif.h's GS_REG_TEX1_1/MIPTBP1_1/
 * MIPTBP2_1/GS_MMIN_MIPMAP_THRESHOLD comments and
 * source/hw/gif.c's rasterize_sprite() mip-level-selection block for
 * the full scope and this round's citation-honesty note (live
 * source-fetch research hit a session limit again this round, same
 * caveat as Rounds 24-27).
 */
#include <stdio.h>
#include <string.h>
#include "hw/gs_mem.c"
#include "hw/gif.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

static void append_ad(uint8_t *buf, int *off, uint32_t data_lo, uint32_t data_hi, uint32_t addr)
{
    wle32(buf + *off, data_lo);
    wle32(buf + *off + 4, data_hi);
    wle32(buf + *off + 8, addr);
    wle32(buf + *off + 12, 0);
    *off += 16;
}

static void fill_texture_solid(uint32_t bp, uint32_t bw, uint32_t w, uint32_t h, uint32_t rgba)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            gs_mem_write_psmct32(bp, bw, x, y, rgba);
}

int main(void)
{
    { /* TEX1 round-trip: known LCM/MXL/MMAG/MMIN/MTBA/L/K values
       * decode correctly, including a NEGATIVE K (sign-extension). */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 2];
        int off = 0;
        wle32(buf + off, (uint32_t)1 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;
        /* LCM=1, MXL=5, MMAG=1, MMIN=3, MTBA=1, L=2, K=-16 (=-1.0 in 1/16 units) */
        uint32_t word0 = 1u /* LCM */ | (5u << 2) /* MXL */ | (1u << 9) /* MMAG */ | (3u << 10) /* MMIN */ | (1u << 14) /* MTBA */;
        int32_t k_val = -16;
        uint32_t word1 = (2u & 0x3u) /* L */ | (((uint32_t)k_val & 0xFFFu) << 2) /* K, 12-bit field */;
        append_ad(buf, &off, word0, word1, GS_REG_TEX1_1);
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        gif_state_t *st = gif_get_state();
        CHECK(st->tex1_lcm == 1, "TEX1: LCM decoded correctly");
        CHECK(st->tex1_mxl == 5, "TEX1: MXL decoded correctly");
        CHECK(st->tex1_mmag == 1, "TEX1: MMAG decoded correctly");
        CHECK(st->tex1_mmin == 3, "TEX1: MMIN decoded correctly");
        CHECK(st->tex1_mtba == 1, "TEX1: MTBA decoded correctly");
        CHECK(st->tex1_l == 2, "TEX1: L decoded correctly");
        CHECK(st->tex1_k == -16, "TEX1: K decoded correctly as a NEGATIVE, sign-extended 12-bit value");
    }

    { /* MIPTBP1/MIPTBP2 round-trip: 6 mip levels' TBP/TBW decode
       * correctly from the sequential-64-bit-bitfield layout,
       * including the two fields (TBP2, TBP3-equivalent... actually
       * TBP2 in MIPTBP1, TBP5 in MIPTBP2) that straddle the word0/
       * word1 boundary. */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 3];
        int off = 0;
        wle32(buf + off, (uint32_t)2 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        /* MIPTBP1: TBP1=100,TBW1_field=1(64px), TBP2=8000 (needs the
         * full 14 bits, straddling word0/word1), TBW2_field=2(128px),
         * TBP3=50, TBW3_field=3(192px). */
        uint32_t tbp1 = 100, tbw1_field = 1, tbp2 = 8000, tbw2_field = 2, tbp3 = 50, tbw3_field = 3;
        uint32_t m1_lo = (tbp1 & 0x3FFFu) | ((tbw1_field & 0x3Fu) << 14) | ((tbp2 & 0xFFFu) << 20);
        uint32_t m1_hi = ((tbp2 >> 12) & 0x3u) | ((tbw2_field & 0x3Fu) << 2) | ((tbp3 & 0x3FFFu) << 8) | ((tbw3_field & 0x3Fu) << 22);
        append_ad(buf, &off, m1_lo, m1_hi, GS_REG_MIPTBP1_1);

        uint32_t tbp4 = 200, tbw4_field = 4, tbp5 = 9000, tbw5_field = 5, tbp6 = 60, tbw6_field = 6;
        uint32_t m2_lo = (tbp4 & 0x3FFFu) | ((tbw4_field & 0x3Fu) << 14) | ((tbp5 & 0xFFFu) << 20);
        uint32_t m2_hi = ((tbp5 >> 12) & 0x3u) | ((tbw5_field & 0x3Fu) << 2) | ((tbp6 & 0x3FFFu) << 8) | ((tbw6_field & 0x3Fu) << 22);
        append_ad(buf, &off, m2_lo, m2_hi, GS_REG_MIPTBP2_1);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        gif_state_t *st = gif_get_state();
        CHECK(st->tex_mip_tbp[0] == tbp1, "MIPTBP1: level 1 TBP decoded correctly");
        CHECK(st->tex_mip_tbw[0] == tbw1_field * 64u, "MIPTBP1: level 1 TBW decoded correctly");
        CHECK(st->tex_mip_tbp[1] == tbp2, "MIPTBP1: level 2 TBP decoded correctly (straddles word0/word1)");
        CHECK(st->tex_mip_tbw[1] == tbw2_field * 64u, "MIPTBP1: level 2 TBW decoded correctly");
        CHECK(st->tex_mip_tbp[2] == tbp3, "MIPTBP1: level 3 TBP decoded correctly");
        CHECK(st->tex_mip_tbw[2] == tbw3_field * 64u, "MIPTBP1: level 3 TBW decoded correctly");
        CHECK(st->tex_mip_tbp[3] == tbp4, "MIPTBP2: level 4 TBP decoded correctly");
        CHECK(st->tex_mip_tbw[3] == tbw4_field * 64u, "MIPTBP2: level 4 TBW decoded correctly");
        CHECK(st->tex_mip_tbp[4] == tbp5, "MIPTBP2: level 5 TBP decoded correctly (straddles word0/word1)");
        CHECK(st->tex_mip_tbw[4] == tbw5_field * 64u, "MIPTBP2: level 5 TBW decoded correctly");
        CHECK(st->tex_mip_tbp[5] == tbp6, "MIPTBP2: level 6 TBP decoded correctly");
        CHECK(st->tex_mip_tbw[5] == tbw6_field * 64u, "MIPTBP2: level 6 TBW decoded correctly");
    }

    { /* Computed LOD (LCM=0): a 64x64 texture drawn into an 8x8
       * screen rectangle (ratio=8, log2(8)=3) -> LOD 3 selected. Level
       * 3's mip buffer (tex_mip_tbp[2]) is filled with a distinctive
       * color; level 0's buffer has a DIFFERENT color. The drawn
       * pixel must be level 3's color, proving mip selection actually
       * changed which buffer was sampled. */
        gs_mem_init(); gif_init();
        uint32_t base_bp = 1000, base_bw = 64;
        uint32_t mip3_bp = 2000, mip3_bw = 64;
        uint32_t base_color = 0xFF0000FFu;   /* should NOT be sampled */
        uint32_t mip3_color = 0xFF00FF00u;   /* should be sampled */
        fill_texture_solid(base_bp, base_bw, 64, 64, base_color);
        fill_texture_solid(mip3_bp, mip3_bw, 8, 8, mip3_color);

        uint8_t buf[16 * 20];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        wle32(buf + off, (uint32_t)11 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        /* TEX0: TW=6 (64), TH=6 (64), PSM=PSMCT32, base texture at base_bp/base_bw */
        uint32_t tex0_lo = (base_bp & 0x3FFFu) | (((base_bw / 64u) & 0x3Fu) << 14) | (6u << 26);
        uint32_t tex0_hi = (TEX_TFX_DECAL << 3) | (6u & 0x3u); /* TH low 2 bits = 6&0x3=2; see below for TH high bits */
        /* TH is 4 bits total: 2 from word0 bits30-31, 2 from word1 bits0-1. TH=6 = 0b0110 -> low2=0b10(2), high2=0b01(1). */
        tex0_lo |= (2u << 30); /* TH low 2 bits */
        tex0_hi = (TEX_TFX_DECAL << 3) | 1u; /* TH high 2 bits = 1 (word1 bits0-1) */
        append_ad(buf, &off, tex0_lo, tex0_hi, GS_REG_TEX0_1);
        /* TEX1: LCM=0 (computed), MXL=3, MMIN=2 (mipmap engaged), MTBA=0 */
        append_ad(buf, &off, (3u << 2) | (2u << 10), 0, GS_REG_TEX1_1);
        /* MIPTBP1: level3 (3rd slot) = mip3_bp/mip3_bw; levels1/2 unused (0) */
        uint32_t tbw3_field = mip3_bw / 64u;
        append_ad(buf, &off, 0u, ((mip3_bp & 0x3FFFu) << 8) | ((tbw3_field & 0x3Fu) << 22), GS_REG_MIPTBP1_1);

        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (2u << 4), (2u << 4), GS_REG_UV);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(8 << 4), (uint32_t)(8 << 4), GS_REG_XYZ2); /* 8x8 screen rect */

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        uint32_t px = gs_mem_read_psmct32(0, 640, 4, 4);
        CHECK(px == mip3_color, "computed LOD: 64x64 texture into 8x8 screen rect (ratio 8, log2=3) samples mip level 3, not the base level");
    }

    { /* MXL clamp: same 64x64-into-8x8 setup (would compute LOD 3),
       * but MXL=1 - the selected level must clamp down to 1, sampling
       * from tex_mip_tbp[0] (level 1), not level 3 or the base. */
        gs_mem_init(); gif_init();
        uint32_t base_bp = 1000, base_bw = 64;
        uint32_t mip1_bp = 3000, mip1_bw = 64;
        uint32_t base_color = 0xFF0000FFu;
        uint32_t mip1_color = 0xFFFF00FFu;
        fill_texture_solid(base_bp, base_bw, 64, 64, base_color);
        fill_texture_solid(mip1_bp, mip1_bw, 32, 32, mip1_color);

        uint8_t buf[16 * 20];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        wle32(buf + off, (uint32_t)10 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        uint32_t tex0_lo = (base_bp & 0x3FFFu) | (((base_bw / 64u) & 0x3Fu) << 14) | (6u << 26) | (2u << 30);
        uint32_t tex0_hi = (TEX_TFX_DECAL << 3) | 1u;
        append_ad(buf, &off, tex0_lo, tex0_hi, GS_REG_TEX0_1);
        /* TEX1: MXL=1 this time (clamp target) */
        append_ad(buf, &off, (1u << 2) | (2u << 10), 0, GS_REG_TEX1_1);
        uint32_t tbw1_field = mip1_bw / 64u;
        append_ad(buf, &off, (mip1_bp & 0x3FFFu) | ((tbw1_field & 0x3Fu) << 14), 0, GS_REG_MIPTBP1_1);

        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (2u << 4), (2u << 4), GS_REG_UV);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(8 << 4), (uint32_t)(8 << 4), GS_REG_XYZ2);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        uint32_t px = gs_mem_read_psmct32(0, 640, 4, 4);
        CHECK(px == mip1_color, "MXL clamp: computed LOD 3 clamps down to MXL=1, sampling level 1's buffer");
    }

    { /* Magnification (texture SMALLER than the screen rect): must
       * always use the base level, regardless of mipmap configuration. */
        gs_mem_init(); gif_init();
        uint32_t base_bp = 1000, base_bw = 64;
        uint32_t mip1_bp = 3000, mip1_bw = 64;
        uint32_t base_color = 0xFF0000FFu;
        uint32_t mip1_color = 0xFFFF00FFu; /* must NOT be sampled */
        fill_texture_solid(base_bp, base_bw, 8, 8, base_color);
        fill_texture_solid(mip1_bp, mip1_bw, 4, 4, mip1_color);

        uint8_t buf[16 * 20];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        wle32(buf + off, (uint32_t)10 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        /* TW=3(8),TH=3(8) this time - small texture */
        uint32_t tex0_lo = (base_bp & 0x3FFFu) | (((base_bw / 64u) & 0x3Fu) << 14) | (3u << 26) | (0u << 30);
        uint32_t tex0_hi = (TEX_TFX_DECAL << 3) | 0u;
        append_ad(buf, &off, tex0_lo, tex0_hi, GS_REG_TEX0_1);
        append_ad(buf, &off, (3u << 2) | (2u << 10), 0, GS_REG_TEX1_1); /* MXL=3, MMIN=2 */
        uint32_t tbw1_field = mip1_bw / 64u;
        append_ad(buf, &off, (mip1_bp & 0x3FFFu) | ((tbw1_field & 0x3Fu) << 14), 0, GS_REG_MIPTBP1_1);

        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (2u << 4), (2u << 4), GS_REG_UV);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(64 << 4), (uint32_t)(64 << 4), GS_REG_XYZ2); /* huge screen rect - magnification */

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        uint32_t px = gs_mem_read_psmct32(0, 640, 4, 4);
        CHECK(px == base_color, "magnification (texture smaller than screen): base level used, mip level 1 NOT sampled");
    }

    { /* Mipmapping disabled (MMIN < threshold): even with a large
       * minification ratio and MXL configured, the base level must
       * always be used. */
        gs_mem_init(); gif_init();
        uint32_t base_bp = 1000, base_bw = 64;
        uint32_t mip3_bp = 2000, mip3_bw = 64;
        uint32_t base_color = 0xFF0000FFu;
        uint32_t mip3_color = 0xFF00FF00u; /* must NOT be sampled */
        fill_texture_solid(base_bp, base_bw, 64, 64, base_color);
        fill_texture_solid(mip3_bp, mip3_bw, 8, 8, mip3_color);

        uint8_t buf[16 * 20];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        wle32(buf + off, (uint32_t)11 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        uint32_t tex0_lo = (base_bp & 0x3FFFu) | (((base_bw / 64u) & 0x3Fu) << 14) | (6u << 26) | (2u << 30);
        uint32_t tex0_hi = (TEX_TFX_DECAL << 3) | 1u;
        append_ad(buf, &off, tex0_lo, tex0_hi, GS_REG_TEX0_1);
        /* MXL=3 but MMIN=1 (LINEAR, below GS_MMIN_MIPMAP_THRESHOLD - mipmapping OFF) */
        append_ad(buf, &off, (3u << 2) | (1u << 10), 0, GS_REG_TEX1_1);
        uint32_t tbw3_field = mip3_bw / 64u;
        append_ad(buf, &off, 0u, ((mip3_bp & 0x3FFFu) << 8) | ((tbw3_field & 0x3Fu) << 22), GS_REG_MIPTBP1_1);

        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (2u << 4), (2u << 4), GS_REG_UV);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(8 << 4), (uint32_t)(8 << 4), GS_REG_XYZ2);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        uint32_t px = gs_mem_read_psmct32(0, 640, 4, 4);
        CHECK(px == base_color, "MMIN below threshold: mipmapping disabled, base level used despite a large minification ratio");
    }

    { /* Fixed LOD (LCM=1): K=32 (32/16=2.0) selects level 2
       * regardless of actual screen/texture size ratio (drawn at a
       * screen size that would compute a totally different LOD under
       * LCM=0, proving K genuinely overrides the computed formula). */
        gs_mem_init(); gif_init();
        uint32_t base_bp = 1000, base_bw = 64;
        uint32_t mip2_bp = 4000, mip2_bw = 64;
        uint32_t base_color = 0xFF0000FFu;
        uint32_t mip2_color = 0xFF123456u;
        fill_texture_solid(base_bp, base_bw, 64, 64, base_color);
        fill_texture_solid(mip2_bp, mip2_bw, 16, 16, mip2_color);

        uint8_t buf[16 * 20];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        wle32(buf + off, (uint32_t)11 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        uint32_t tex0_lo = (base_bp & 0x3FFFu) | (((base_bw / 64u) & 0x3Fu) << 14) | (6u << 26) | (2u << 30);
        uint32_t tex0_hi = (TEX_TFX_DECAL << 3) | 1u;
        append_ad(buf, &off, tex0_lo, tex0_hi, GS_REG_TEX0_1);
        /* LCM=1, MXL=3, MMIN=2, K=32 (2.0 in 1/16 units) */
        uint32_t tex1_word0 = 1u /* LCM */ | (3u << 2) /* MXL */ | (2u << 10) /* MMIN */;
        uint32_t tex1_word1 = (32u & 0xFFFu) << 2; /* K=32 */
        append_ad(buf, &off, tex1_word0, tex1_word1, GS_REG_TEX1_1);
        uint32_t tbw2_field = mip2_bw / 64u;
        append_ad(buf, &off, ((mip2_bp & 0xFFFu) << 20), ((mip2_bp >> 12) & 0x3u) | ((tbw2_field & 0x3Fu) << 2), GS_REG_MIPTBP1_1);

        /* Drawn at a screen size (32x32) that, under LCM=0, would
         * compute log2(64/32)=1, NOT 2 - proving K=32 (fixed LOD 2)
         * really did override the computed formula. */
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (2u << 4), (2u << 4), GS_REG_UV);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(32 << 4), (uint32_t)(32 << 4), GS_REG_XYZ2);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        uint32_t px = gs_mem_read_psmct32(0, 640, 16, 16);
        CHECK(px == mip2_color, "fixed LOD (LCM=1, K=32=2.0): samples level 2's buffer, overriding what the computed formula (LCM=0) would have picked");
    }

    { /* MTBA=1 (auto address calculation): documented, unimplemented
       * gap - must safely fall back to the base level rather than
       * misbehave, even with mipmapping otherwise fully configured. */
        gs_mem_init(); gif_init();
        uint32_t base_bp = 1000, base_bw = 64;
        uint32_t mip3_bp = 2000, mip3_bw = 64;
        uint32_t base_color = 0xFF0000FFu;
        uint32_t mip3_color = 0xFF00FF00u; /* must NOT be sampled */
        fill_texture_solid(base_bp, base_bw, 64, 64, base_color);
        fill_texture_solid(mip3_bp, mip3_bw, 8, 8, mip3_color);

        uint8_t buf[16 * 20];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        wle32(buf + off, (uint32_t)11 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        uint32_t tex0_lo = (base_bp & 0x3FFFu) | (((base_bw / 64u) & 0x3Fu) << 14) | (6u << 26) | (2u << 30);
        uint32_t tex0_hi = (TEX_TFX_DECAL << 3) | 1u;
        append_ad(buf, &off, tex0_lo, tex0_hi, GS_REG_TEX0_1);
        /* MXL=3, MMIN=2, MTBA=1 (auto - unimplemented) */
        append_ad(buf, &off, (3u << 2) | (2u << 10) | (1u << 14), 0, GS_REG_TEX1_1);
        uint32_t tbw3_field = mip3_bw / 64u;
        append_ad(buf, &off, 0u, ((mip3_bp & 0x3FFFu) << 8) | ((tbw3_field & 0x3Fu) << 22), GS_REG_MIPTBP1_1);

        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (2u << 4), (2u << 4), GS_REG_UV);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(8 << 4), (uint32_t)(8 << 4), GS_REG_XYZ2);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        uint32_t px = gs_mem_read_psmct32(0, 640, 4, 4);
        CHECK(px == base_color, "MTBA=1 (unimplemented auto address calc): safely falls back to the base level rather than misbehaving");
    }

    { /* Regression: a textured SPRITE with NO TEX1 configuration at
       * all (mxl defaults to 0 from gif_init()'s zero-init) draws
       * exactly as before this round - base level only, matching
       * every pre-existing texturing test's behavior. */
        gs_mem_init(); gif_init();
        uint32_t tex_bp = 5000, tex_bw = 64;
        uint32_t tex_color = 0xFFAABBCCu;
        fill_texture_solid(tex_bp, tex_bw, 8, 8, tex_color);

        uint8_t buf[16 * 10];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        wle32(buf + off, (uint32_t)7 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, (tex_bp & 0x3FFFu) | (((tex_bw / 64u) & 0x3Fu) << 14), (TEX_TFX_DECAL << 3), GS_REG_TEX0_1);
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_TME_MASK | PRIM_FST_MASK, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(64 << 4), (uint32_t)(64 << 4), GS_REG_XYZ2); /* big rect, would minify heavily if mipmapping were mistakenly engaged */

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        uint32_t px = gs_mem_read_psmct32(0, 640, 4, 4);
        CHECK(px == tex_color, "regression: no TEX1 configured (MXL=0 default) - base level used, matching pre-Round-28 behavior exactly");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
