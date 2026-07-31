/*
 * test_gs_output_pipeline_isolation.c - Round 189 (task #355).
 *
 * User directive: "bypass the bios boot path and force so we can fix
 * any issues inside so we can trace any other things which are
 * holding pmode etc back". The single most stable blocker in this
 * project's history (94th finding, Round 62, through the 223rd
 * finding, Round 183 - 121+ rounds) is that real BIOS boot code never
 * writes PMODE/DISPFB1/DISPLAY1 (the GS display-enable/framebuffer-
 * pointer registers). Every prior round tested this exclusively
 * THROUGH the real boot trace, which left one question permanently
 * unanswered: if PMODE/DISPFB1 ever DID get written for real, does
 * the downstream display pipeline (register gate -> DISPFB1 decode ->
 * GS memory read -> RGB->YCbCr -> XFB write) actually work correctly?
 *
 * This test answers that directly, independent of the BIOS/EE/IOP
 * entirely: it force-writes realistic PMODE/DISPFB1 register values
 * (bypassing boot), force-writes a known test pattern into GS memory
 * using gs_mem_write_psmct32 (confirmed via grep this round to be the
 * SAME function gif.c's real rasterizer uses - not a hypothetical
 * write path), then runs the EXACT SAME control-flow sequence
 * main.c's run_real_boot_flow() runs every frame once display_active
 * becomes true (PMODE bit check -> gs_decode_dispfb -> gs_blit_
 * psmct32_to_xfb), and verifies the resulting synthetic XFB buffer
 * contains the correct, exact expected YCbCr-encoded pixels.
 *
 * Result (this round): all checks pass. This is real, permanent
 * regression coverage for a control-flow combination (the register-
 * gated production display sequence) that was previously completely
 * untested - individual leaf functions (gs_decode_dispfb, gs_rgb8_
 * pair_to_ycbcr, gs_blit_psmct32_to_xfb) already had unit coverage
 * in test_gs_output.c, but the combined, register-driven sequence
 * itself never did. See docs/STATUS.md's 229th finding for the full
 * writeup and conclusion this test's passing result supports.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "hw/gs.c"
#include "hw/gs_mem.c"
#include "hw/gs_wii_output.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void)
{
    gs_init();
    gs_mem_init();
    gs_state_t *gs = gs_get_state();

    CHECK(gs->pmode == 0, "initial state: PMODE == 0 (matches every real boot trace measurement since the 94th finding)");
    CHECK((gs->pmode & 0x3u) == 0, "initial state: display_active (PMODE bit 0/1) is false - main.c would skip the blit, exactly as observed live");

    /* Force-write a known test pattern into GS memory using
     * gs_mem_write_psmct32 - the same function gif.c's real
     * rasterizer (fed by real GIF/DMA packets) uses (grep-verified
     * this round: gif.c never calls the separate _swizzled variant). */
    const uint32_t FBW_PIXELS = 640u;
    const uint32_t FBP_WORDS  = 0u;
    const uint32_t TEST_W = 64, TEST_H = 8;
    const uint32_t RED   = 0x000000FFu; /* project's own 0xAABBGGRR PSMCT32 convention */
    const uint32_t GREEN = 0x0000FF00u;
    const uint32_t BLUE  = 0x00FF0000u;
    for (uint32_t y = 0; y < TEST_H; y++) {
        for (uint32_t x = 0; x < TEST_W; x++) {
            uint32_t color = (x < TEST_W/3) ? RED : (x < 2*TEST_W/3) ? GREEN : BLUE;
            gs_mem_write_psmct32(FBP_WORDS, FBW_PIXELS, x, y, color);
        }
    }
    CHECK(gs_mem_read_psmct32(FBP_WORDS, FBW_PIXELS, 5, 0) == RED,    "GS memory write/read round-trip: red band");
    CHECK(gs_mem_read_psmct32(FBP_WORDS, FBW_PIXELS, 30, 0) == GREEN, "GS memory write/read round-trip: green band");
    CHECK(gs_mem_read_psmct32(FBP_WORDS, FBW_PIXELS, 60, 0) == BLUE,  "GS memory write/read round-trip: blue band");

    /* Force-write realistic real PMODE/DISPFB1 values, bypassing
     * BIOS/EE entirely - as if real boot code HAD reached display
     * setup. Real bit layout (already cited in gs_decode_dispfb's own
     * doc comment / main.c's own comment): PMODE bit 0 = EN1.
     * DISPFB1 bits 0-8 = FBP (units of 2048 words), bits 9-14 = FBW
     * (units of 64 pixels). */
    gs->pmode = 0x1u;
    uint32_t fbp_field = FBP_WORDS / 2048u;
    uint32_t fbw_field  = FBW_PIXELS / 64u;
    CHECK(FBP_WORDS % 2048u == 0, "test setup: FBP_WORDS exactly representable in the real 2048-word FBP unit");
    CHECK(FBW_PIXELS % 64u == 0, "test setup: FBW_PIXELS exactly representable in the real 64-pixel FBW unit");
    gs->dispfb1 = ((uint64_t)fbw_field << 9) | (uint64_t)fbp_field;

    /* Run the EXACT SAME logic main.c's run_real_boot_flow() runs
     * every frame once display_active becomes true. */
    int display_active = (gs->pmode & 0x3u) != 0;
    CHECK(display_active, "after force-write: display_active is now true");

    uint32_t bp_words, bw_pixels;
    gs_decode_dispfb(gs->dispfb1, &bp_words, &bw_pixels);
    CHECK(bp_words == FBP_WORDS, "gs_decode_dispfb: FBP decodes back to the exact encoded value");
    CHECK(bw_pixels == FBW_PIXELS, "gs_decode_dispfb: FBW decodes back to the exact encoded value");

    const uint32_t XFB_W = FBW_PIXELS, XFB_H = 64;
    uint32_t *xfb = calloc((size_t)XFB_W/2 * XFB_H, sizeof(uint32_t));
    if (!xfb) { printf("FAIL: calloc failed\n"); return 1; }

    gs_blit_psmct32_to_xfb(xfb, XFB_W, 0, 0, bp_words, bw_pixels, 0, 0, XFB_W, TEST_H);

    uint32_t expected_red_pair   = gs_rgb8_pair_to_ycbcr(0xFF,0,0, 0xFF,0,0);
    uint32_t expected_green_pair = gs_rgb8_pair_to_ycbcr(0,0xFF,0, 0,0xFF,0);
    uint32_t expected_blue_pair  = gs_rgb8_pair_to_ycbcr(0,0,0xFF, 0,0,0xFF);

    uint32_t xfb_words_per_row = XFB_W / 2;
    CHECK(xfb[0 * xfb_words_per_row + (5/2)]  == expected_red_pair,   "XFB row0 red band matches expected YCbCr encoding");
    CHECK(xfb[0 * xfb_words_per_row + (30/2)] == expected_green_pair, "XFB row0 green band matches expected YCbCr encoding");
    CHECK(xfb[0 * xfb_words_per_row + (60/2)] == expected_blue_pair,  "XFB row0 blue band matches expected YCbCr encoding");

    int all_rows_nonzero = 1;
    for (uint32_t y = 0; y < TEST_H; y++) {
        int row_has_data = 0;
        for (uint32_t w = 0; w < xfb_words_per_row; w++)
            if (xfb[y * xfb_words_per_row + w] != 0) { row_has_data = 1; break; }
        if (!row_has_data) { all_rows_nonzero = 0; break; }
    }
    CHECK(all_rows_nonzero, "every row of the blit region has non-zero XFB data (no off-by-one row gap)");

    int rows_outside_untouched = 1;
    for (uint32_t y = TEST_H; y < XFB_H; y++) {
        for (uint32_t w = 0; w < xfb_words_per_row; w++)
            if (xfb[y * xfb_words_per_row + w] != 0) { rows_outside_untouched = 0; break; }
        if (!rows_outside_untouched) break;
    }
    CHECK(rows_outside_untouched, "rows beyond the intended blit height were NOT touched (no out-of-bounds write)");

    free(xfb);

    if (failures == 0)
        printf("ALL CHECKS PASSED (0 failures)\n");
    else
        printf("%d check(s) FAILED\n", failures);
    return failures != 0;
}
