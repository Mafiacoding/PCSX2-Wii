/*
 * gs_wii_output.c - see include/core/hw/gs_wii_output.h.
 */

#include "core/hw/gs_wii_output.h"
#include "core/hw/gs_mem.h"

uint32_t gs_rgb8_pair_to_ycbcr(uint8_t r1, uint8_t g1, uint8_t b1,
                                uint8_t r2, uint8_t g2, uint8_t b2)
{
    /* Ported verbatim from libogc's console.c RGB8_to_YCbCr (BSD-style
     * license). The 16/240 clamping matches broadcast-safe YCbCr
     * range, not full 0-255 range - this is intentional and matches
     * what the Wii's video hardware/libogc itself does. */
    if (r1 < 16) r1 = 16;
    if (g1 < 16) g1 = 16;
    if (b1 < 16) b1 = 16;
    if (r2 < 16) r2 = 16;
    if (g2 < 16) g2 = 16;
    if (b2 < 16) b2 = 16;

    if (r1 > 240) r1 = 240;
    if (g1 > 240) g1 = 240;
    if (b1 > 240) b1 = 240;
    if (r2 > 240) r2 = 240;
    if (g2 > 240) g2 = 240;
    if (b2 > 240) b2 = 240;

    uint8_t Y1 = (uint8_t)((77 * r1 + 150 * g1 + 29 * b1) / 256);
    uint8_t Y2 = (uint8_t)((77 * r2 + 150 * g2 + 29 * b2) / 256);
    uint8_t Cb = (uint8_t)((112 * (b1 + b2) -  74 * (g1 + g2) - 38 * (r1 + r2)) / 512 + 128);
    uint8_t Cr = (uint8_t)((112 * (r1 + r2) - 94  * (g1 + g2) - 18 * (b1 + b2)) / 512 + 128);

    return ((uint32_t)Y1 << 24) | ((uint32_t)Cb << 16) | ((uint32_t)Y2 << 8) | (uint32_t)Cr;
}

static inline void rgba32_to_rgb8(uint32_t rgba, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* PSMCT32 channel order as stored by gs_mem_write_psmct32's caller
     * convention: 0xAABBGGRR (matches how we pack test/demo colors
     * elsewhere in this project - see tests). */
    *r = (uint8_t)(rgba & 0xFF);
    *g = (uint8_t)((rgba >> 8) & 0xFF);
    *b = (uint8_t)((rgba >> 16) & 0xFF);
}

void gs_blit_psmct32_to_xfb(void *xfb, uint32_t xfb_width_px,
                             uint32_t dst_x, uint32_t dst_y,
                             uint32_t gs_bp, uint32_t gs_bw,
                             uint32_t src_x, uint32_t src_y,
                             uint32_t width, uint32_t height)
{
    uint32_t *xfb32 = (uint32_t *)xfb;
    uint32_t xfb_words_per_row = xfb_width_px / 2;

    for (uint32_t row = 0; row < height; row++) {
        for (uint32_t col = 0; col < width; col += 2) {
            uint32_t p1 = gs_mem_read_psmct32(gs_bp, gs_bw, src_x + col, src_y + row);
            uint32_t p2 = (col + 1 < width)
                ? gs_mem_read_psmct32(gs_bp, gs_bw, src_x + col + 1, src_y + row)
                : p1; /* odd trailing pixel: duplicate so the pair conversion still makes sense */

            uint8_t r1, g1, b1, r2, g2, b2;
            rgba32_to_rgb8(p1, &r1, &g1, &b1);
            rgba32_to_rgb8(p2, &r2, &g2, &b2);

            uint32_t yuv = gs_rgb8_pair_to_ycbcr(r1, g1, b1, r2, g2, b2);

            uint32_t dst_word_x = (dst_x + col) / 2;
            uint32_t dst_row = dst_y + row;
            xfb32[dst_row * xfb_words_per_row + dst_word_x] = yuv;
        }
    }
}
