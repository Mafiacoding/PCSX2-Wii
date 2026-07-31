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

    /* Round 119 (task #172/#274): gs_decode_dispfb() - real GS
     * DISPFB1/DISPFB2 register field decode (FBP bits 0-8, units of
     * 2048 words; FBW bits 9-14, units of 64 pixels). This is the
     * exact function main.c's real production display path
     * (run_real_boot_flow(), task #126) calls the moment PMODE
     * indicates an active display - previously untested since it
     * lived in main.c (which depends on <gccore.h>, unavailable
     * host-natively). */
    {
        uint32_t bp, bw;

        gs_decode_dispfb(0, &bp, &bw);
        CHECK(bp == 0 && bw == 0, "dispfb=0: FBP=0 words, FBW=0 pixels");

        gs_decode_dispfb(1, &bp, &bw); /* FBP field = 1 */
        CHECK(bp == 2048 && bw == 0, "dispfb FBP=1: bp_words=2048 (1*2048)");

        gs_decode_dispfb(1u << 9, &bp, &bw); /* FBW field = 1, bit 9 exactly */
        CHECK(bp == 0 && bw == 64, "dispfb FBW=1 (bit 9): bw_pixels=64 (1*64)");

        gs_decode_dispfb(1u << 8, &bp, &bw); /* top bit of FBP field (bit 8) */
        CHECK(bp == 256u * 2048u && bw == 0, "dispfb bit 8 (top of FBP field): bp_words=524288, FBW untouched");

        /* Realistic real-hardware-typical case: a 640-pixel-wide
         * framebuffer (FBW field = 640/64 = 10) at word offset
         * 5*2048 = 10240 (FBP field = 5). */
        uint64_t realistic = (uint64_t)5u | ((uint64_t)10u << 9);
        gs_decode_dispfb(realistic, &bp, &bw);
        CHECK(bp == 10240 && bw == 640, "dispfb realistic 640px-wide case: bp_words=10240, bw_pixels=640");

        /* Bits above 14 (outside both real fields) must be ignored -
         * real hardware defines DISPFB1/2 with other fields (DBX/DBY/
         * PSM) above bit 14 that this function doesn't decode. */
        gs_decode_dispfb((uint64_t)1u << 20, &bp, &bw);
        CHECK(bp == 0 && bw == 0, "dispfb bit 20 (outside FBP/FBW fields): both outputs 0");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
