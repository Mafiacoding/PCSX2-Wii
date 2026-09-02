/* test_gs_context2.c - host-native test for Round 27's GS Context 2
 * (dual-context) support. See include/core/hw/gif.h's
 * PRIM_CTXT_MASK/GS_REG_FRAME_2/XYOFFSET_2/TEX0_2/TEST_2/ALPHA_2/
 * ZBUF_2 comments and gif_state_t's ctx1_xxx/ctx2_xxx field comment,
 * plus source/hw/gif.c's gs_activate_context() for the full design
 * and this round's citation-honesty note (live source-fetch research
 * hit a session limit again this round, same caveat as Rounds 24-26,
 * mitigated here by an internal self-consistency check: every _2
 * register address used below is context-1's own address + 1, a
 * pattern independently confirmed by this project's own prior-round
 * additions - see the header comment for the cross-check list).
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

int main(void)
{
    { /* Basic dual-target test: configure FRAME_1 at bp=0 and
       * FRAME_2 at a DIFFERENT bp, then draw one sprite with
       * PRIM.CTXT=0 and another with PRIM.CTXT=1 - each must land in
       * its own, independent target buffer. */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 20];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        int nloop = 4 + 2 + 4 + 4; /* FRAME_1,XYOFFSET_1,FRAME_2,XYOFFSET_2 (4) + PRIM/RGBAQ/XYZ2/XYZ2 ctx1 (4) + same ctx2 (4) -- fixed below */
        (void)nloop;
        wle32(buf + off, (uint32_t)12 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);   /* ctx1 target: bp=0, fbw guarded default 640 */
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, 20u, 0, GS_REG_FRAME_2);  /* ctx2 target: FBP=20 (bits 0-8, no shift), FBW field=0 -> guarded default 640 - DIFFERENT from ctx1 */
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_2);

        uint32_t red = 0xFF0000FFu, blue = 0xFFFF0000u;

        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE, 0, GS_REG_PRIM); /* CTXT=0 */
        append_ad(buf, &off, red, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(10 << 4), (uint32_t)(10 << 4), GS_REG_XYZ2);

        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_CTXT_MASK, 0, GS_REG_PRIM); /* CTXT=1 */
        append_ad(buf, &off, blue, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(10 << 4), (uint32_t)(10 << 4), GS_REG_XYZ2);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        /* Round 748: FRAME.FBP is real-hardware page-granularity
         * (Address/2048 words) - gif.c's FRAME_1/FRAME_2 handlers now
         * scale the raw field by *2048 at decode time (matching
         * gs_decode_dispfb()'s existing DISPFB convention - see
         * docs/STATUS.md Round 748), so raw field 20 lands at word
         * offset 20*2048, not 20 itself. */
        CHECK(gs_mem_read_psmct32(0, 640, 5, 5) == red, "context1 sprite (CTXT=0) landed in FRAME_1's target (bp=0)");
        CHECK(gs_mem_read_psmct32(20u * 2048u, 640, 5, 5) == blue, "context2 sprite (CTXT=1) landed in FRAME_2's target (bp=20), NOT FRAME_1's");
        CHECK(gs_mem_read_psmct32(20u * 2048u, 640, 5, 5) != red, "context2's target buffer does not contain context1's color (genuinely separate)");
    }

    { /* Context 1's own state is completely unaffected by ANY amount
       * of FRAME_2/XYOFFSET_2 configuration, as long as no CTXT=1
       * primitive is ever drawn - proves ctx2 registers are inert
       * until actually selected (matches real hardware: writing a
       * context's registers has no effect until a primitive is
       * dispatched with that context selected). */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 12];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        wle32(buf + off, (uint32_t)8 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, 99u, 0, GS_REG_FRAME_2); /* FBP=99, configured but never selected */
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_2);

        uint32_t green = 0xFF00FF00u;
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE, 0, GS_REG_PRIM); /* CTXT=0 (default) */
        append_ad(buf, &off, green, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(10 << 4), (uint32_t)(10 << 4), GS_REG_XYZ2);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        CHECK(gs_mem_read_psmct32(0, 640, 5, 5) == green, "context1 draw unaffected by an unused, configured-but-never-selected FRAME_2");
        CHECK(gs_mem_read_psmct32(99u * 2048u, 640, 5, 5) == 0, "FRAME_2's never-selected target buffer was never actually written to");
    }

    { /* Alpha test/blend state is genuinely per-context: configure
       * TEST_1 with ATST_NEVER (always fails, AFAIL_KEEP - discards
       * everything) and TEST_2 with ATST_ALWAYS (always passes) at
       * the SAME target buffer/position, drawing two sprites (one
       * per context) - the CTXT=0 sprite must be fully discarded,
       * the CTXT=1 sprite must draw normally. */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 14];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        wle32(buf + off, (uint32_t)10 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        /* TEST_1: ATE=1, ATST=NEVER(0), AFAIL=KEEP(0) */
        append_ad(buf, &off, TEST_ATE_MASK | (GS_ATST_NEVER << TEST_ATST_SHIFT), 0, GS_REG_TEST_1);
        /* TEST_2: ATE=1, ATST=ALWAYS(1) */
        append_ad(buf, &off, TEST_ATE_MASK | (GS_ATST_ALWAYS << TEST_ATST_SHIFT), 0, GS_REG_TEST_2);

        uint32_t magenta = 0xFFFF00FFu;
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE, 0, GS_REG_PRIM); /* CTXT=0: uses TEST_1 (ATST_NEVER) */
        append_ad(buf, &off, magenta, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(10 << 4), (uint32_t)(10 << 4), GS_REG_XYZ2);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));
        CHECK(gs_mem_read_psmct32(0, 640, 5, 5) == 0, "CTXT=0 sprite discarded by TEST_1's ATST_NEVER (context1's own alpha test)");

        int off2 = 0;
        uint8_t buf2[16 * 6];
        memset(buf2, 0, sizeof(buf2));
        wle32(buf2 + off2, (uint32_t)4 | (1u << 15));
        wle32(buf2 + off2 + 4, (0u << 26) | (1u << 28));
        wle32(buf2 + off2 + 8, GIF_REG_AD);
        wle32(buf2 + off2 + 12, 0);
        off2 += 16;
        append_ad(buf2, &off2, (uint32_t)PRIM_TYPE_SPRITE | PRIM_CTXT_MASK, 0, GS_REG_PRIM); /* CTXT=1: uses TEST_2 (ATST_ALWAYS) */
        append_ad(buf2, &off2, magenta, 0, GS_REG_RGBAQ);
        append_ad(buf2, &off2, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf2, &off2, (uint32_t)(10 << 4), (uint32_t)(10 << 4), GS_REG_XYZ2);
        gif_process_quadwords(DMA_CHANNEL_GIF, buf2, (uint32_t)(off2 / 16));

        CHECK(gs_mem_read_psmct32(0, 640, 5, 5) == magenta, "CTXT=1 sprite (same target, same position) draws normally under TEST_2's ATST_ALWAYS (context2's own, independent alpha test)");
    }

    { /* Interleaved draws (ctx1, ctx2, ctx1 again) prove state
       * doesn't leak or get clobbered between context switches -
       * matches real hardware, where games freely interleave
       * primitives across both contexts within a single frame. */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 20];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        wle32(buf + off, (uint32_t)16 | (1u << 15));
        wle32(buf + off + 4, (0u << 26) | (1u << 28));
        wle32(buf + off + 8, GIF_REG_AD);
        wle32(buf + off + 12, 0);
        off += 16;

        append_ad(buf, &off, (0u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, 50u, 0, GS_REG_FRAME_2); /* FBP=50 (bits 0-8, no shift) */
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_2);

        uint32_t c1 = 0xFF111111u, c2 = 0xFF222222u, c3 = 0xFF333333u;

        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE, 0, GS_REG_PRIM);
        append_ad(buf, &off, c1, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(5 << 4), (uint32_t)(5 << 4), GS_REG_XYZ2);

        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_CTXT_MASK, 0, GS_REG_PRIM);
        append_ad(buf, &off, c2, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(5 << 4), (uint32_t)(5 << 4), GS_REG_XYZ2);

        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE, 0, GS_REG_PRIM);
        append_ad(buf, &off, c3, 0, GS_REG_RGBAQ);
        append_ad(buf, &off, (uint32_t)(20 << 4), (uint32_t)(20 << 4), GS_REG_XYZ2);
        append_ad(buf, &off, (uint32_t)(25 << 4), (uint32_t)(25 << 4), GS_REG_XYZ2);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        CHECK(gs_mem_read_psmct32(0, 640, 2, 2) == c1, "interleaved: first ctx1 draw's pixel still correct after a ctx2 draw ran in between");
        CHECK(gs_mem_read_psmct32(50u * 2048u, 640, 2, 2) == c2, "interleaved: ctx2 draw landed in FRAME_2's target");
        CHECK(gs_mem_read_psmct32(0, 640, 22, 22) == c3, "interleaved: second ctx1 draw (after the ctx2 draw) still uses FRAME_1's target correctly - ctx1 state was not clobbered by the ctx2 draw in between");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
