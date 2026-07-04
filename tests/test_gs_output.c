#include <stdio.h>
#include <string.h>
#include "hw/gs_mem.c"
#include "hw/gs_wii_output.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    /* Hand-verified anchor points (computed independently against the
     * libogc RGB8_to_YCbCr formula before writing this test - see
     * commit message / tests/README.md for the arithmetic):
     * clamped white (240,240,240) -> Y=240, Cb=128, Cr=128 (achromatic, max luma)
     * clamped black (16,16,16)    -> Y=16,  Cb=128, Cr=128 (achromatic, min luma) */
    uint32_t white = gs_rgb8_pair_to_ycbcr(255,255,255,255,255,255);
    CHECK(((white >> 24) & 0xFF) == 240, "white pair: Y1 = 240 (clamped)");
    CHECK(((white >> 16) & 0xFF) == 128, "white pair: Cb = 128 (achromatic)");
    CHECK((white & 0xFF) == 128, "white pair: Cr = 128 (achromatic)");
    CHECK(((white >> 8) & 0xFF) == 240, "white pair: Y2 = 240 (clamped)");

    uint32_t black = gs_rgb8_pair_to_ycbcr(0,0,0,0,0,0);
    CHECK(((black >> 24) & 0xFF) == 16, "black pair: Y1 = 16 (clamped)");
    CHECK(((black >> 16) & 0xFF) == 128, "black pair: Cb = 128 (achromatic)");
    CHECK((black & 0xFF) == 128, "black pair: Cr = 128 (achromatic)");

    /* Blit test: fill a 4x2 region of GS memory with a known solid
     * color, blit it into a mock XFB, and verify the packed YUV words
     * landed at the right offsets. */
    gs_mem_init();
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 4; x++)
            gs_mem_write_psmct32(0, 640, x, y, 0x00FFFFFFu); /* opaque white, RGBA-ish 0xAABBGGRR = white */

    uint32_t mock_xfb[8 * 4]; /* room for an 8-pixel-wide, 4-row XFB (4 YUV words/row) */
    memset(mock_xfb, 0xAA, sizeof(mock_xfb)); /* poison value to catch under-writes */

    gs_blit_psmct32_to_xfb(mock_xfb, /*xfb_width_px=*/8, /*dst_x=*/0, /*dst_y=*/0,
                            /*gs_bp=*/0, /*gs_bw=*/640, /*src_x=*/0, /*src_y=*/0,
                            /*width=*/4, /*height=*/2);

    uint32_t expect_white_pair = gs_rgb8_pair_to_ycbcr(255,255,255,255,255,255);
    CHECK(mock_xfb[0] == expect_white_pair, "row 0, pixel pair 0-1 blitted correctly");
    CHECK(mock_xfb[1] == expect_white_pair, "row 0, pixel pair 2-3 blitted correctly");
    CHECK(mock_xfb[2] == 0xAAAAAAAAu, "row 0, pixel pair 4-5 (outside blit width) untouched");
    CHECK(mock_xfb[4] == expect_white_pair, "row 1, pixel pair 0-1 blitted correctly");
    CHECK(mock_xfb[6] == 0xAAAAAAAAu, "row 1, pixel pair 4-5 untouched");
    CHECK(mock_xfb[3*4+0] == 0xAAAAAAAAu, "row 2 (outside blit height) fully untouched");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
