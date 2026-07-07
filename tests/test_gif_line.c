/*
 * test_gif_line.c - host-native test for POINT/LINE/LINE_STRIP
 * rasterization added to source/hw/gif.c (task: "GS coverage
 * breadth", item 5). See include/core/hw/gif.h's PRIM_TYPE_POINT/
 * PRIM_TYPE_LINE/PRIM_TYPE_LINE_STRIP comments and gif.c's
 * rasterize_point()/rasterize_line() for the full scope and
 * citations (PCSX2's GS/GSRegs.h `enum GS_PRIM`, GSRasterizer.cpp's
 * DrawPoint/DrawEdgeLine, GSDrawScanline.cpp's CSetupPrim).
 *
 * Like test_z_buffer.c, XYZ2 vertices here use genuine PACKED-mode GIF
 * packets (X in word0, Y in word1, Z in word2) so real per-vertex Z
 * values can be exercised - this project's A+D XYZ2 convention has no
 * room for Z (see gif.h's tri_z field comment).
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

static void append_xyz2_packed(uint8_t *buf, int *off, uint32_t x_raw, uint32_t y_raw, uint32_t z_raw)
{
    wle32(buf + *off, x_raw); wle32(buf + *off + 4, y_raw);
    wle32(buf + *off + 8, z_raw); wle32(buf + *off + 12, 0);
    *off += 16;
}

int main(void)
{
    gs_mem_init();
    gif_init();

    { /* POINT: a single flat-color pixel, no interpolation */
        uint8_t buf[16 * 16];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        write_tag(buf, &off, 4, 0xE);
        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, PRIM_TYPE_POINT, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0u | (200u << 8) | (50u << 16) | (255u << 24), 0, GS_REG_RGBAQ);

        write_tag(buf, &off, 1, 0x5);
        append_xyz2_packed(buf, &off, (uint32_t)(15 << 4), (uint32_t)(20 << 4), 0);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        gif_state_t *st = gif_get_state();
        CHECK(st->points_drawn == 1, "POINT: exactly 1 point drawn");
        CHECK(gs_mem_read_psmct32(0, 640, 15, 20) == (0xFFu << 24 | 50u << 16 | 200u << 8 | 0u),
              "POINT: pixel written with the exact RGBAQ color, no interpolation");
    }

    { /* LINE, flat shading: color must be the LAST vertex's (real
       * hardware convention, cross-checked against PCSX2's
       * CSetupPrim last=1 for GS_LINE_CLASS). A horizontal line so
       * every pixel on it is trivially predictable. */
        gs_mem_init();
        gif_init();
        uint8_t buf[16 * 16];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        write_tag(buf, &off, 4, 0xE);
        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, PRIM_TYPE_LINE, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0u | (0u << 8) | (0u << 16) | (255u << 24), 0, GS_REG_RGBAQ);

        write_tag(buf, &off, 2, 0x55);
        append_xyz2_packed(buf, &off, (uint32_t)(10 << 4), (uint32_t)(50 << 4), 0);
        append_xyz2_packed(buf, &off, (uint32_t)(20 << 4), (uint32_t)(50 << 4), 0);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        gif_state_t *st = gif_get_state();
        CHECK(st->lines_drawn == 1, "LINE (flat): exactly 1 segment drawn");
        uint32_t red = (255u << 24) | (0u << 16) | (0u << 8) | 0u;
        CHECK(gs_mem_read_psmct32(0, 640, 15, 50) == red, "LINE (flat): a mid-segment pixel has the flat (last-vertex) color");
        CHECK(gs_mem_read_psmct32(0, 640, 10, 50) == red, "LINE (flat): the start endpoint has the flat color too");
        CHECK(gs_mem_read_psmct32(0, 640, 20, 50) == red, "LINE (flat): the end endpoint has the flat color too");
    }

    { /* LINE, Gouraud shading: distinct per-vertex colors, verify the
       * midpoint blends roughly halfway (real per-pixel linear DDA
       * interpolation, not a flat fill). */
        gs_mem_init();
        gif_init();
        uint8_t buf[16 * 16];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        write_tag(buf, &off, 3, 0xE);
        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, (uint32_t)(PRIM_TYPE_LINE | PRIM_IIP_MASK), 0, GS_REG_PRIM);

        write_tag(buf, &off, 1, 0xE);
        append_ad(buf, &off, 255u | (0u << 8) | (0u << 16) | (255u << 24), 0, GS_REG_RGBAQ);
        write_tag(buf, &off, 1, 0x5);
        append_xyz2_packed(buf, &off, (uint32_t)(0 << 4), (uint32_t)(10 << 4), 0);

        write_tag(buf, &off, 1, 0xE);
        append_ad(buf, &off, 0u | (0u << 8) | (255u << 16) | (255u << 24), 0, GS_REG_RGBAQ);
        write_tag(buf, &off, 1, 0x5);
        append_xyz2_packed(buf, &off, (uint32_t)(100 << 4), (uint32_t)(10 << 4), 0);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        gif_state_t *st = gif_get_state();
        CHECK(st->lines_drawn == 1, "LINE (Gouraud): exactly 1 segment drawn");
        uint32_t start_px = gs_mem_read_psmct32(0, 640, 0, 10);
        uint32_t end_px = gs_mem_read_psmct32(0, 640, 100, 10);
        uint32_t mid_px = gs_mem_read_psmct32(0, 640, 50, 10);
        CHECK((start_px & 0xFFu) == 255u, "LINE (Gouraud): start endpoint is fully red");
        CHECK(((end_px >> 16) & 0xFFu) == 255u, "LINE (Gouraud): end endpoint is fully blue");
        uint32_t mid_r = mid_px & 0xFFu, mid_b = (mid_px >> 16) & 0xFFu;
        CHECK(mid_r > 100u && mid_r < 155u, "LINE (Gouraud): midpoint red channel is roughly half-way (real per-pixel interpolation, not flat)");
        CHECK(mid_b > 100u && mid_b < 155u, "LINE (Gouraud): midpoint blue channel is roughly half-way too");
    }

    { /* LINE_STRIP: 3 vertices -> 2 connected segments, real hardware's
       * rolling-window continuation (same shape as TRIANGLE_STRIP). */
        gs_mem_init();
        gif_init();
        uint8_t buf[16 * 16];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        write_tag(buf, &off, 4, 0xE);
        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, PRIM_TYPE_LINE_STRIP, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0u | (255u << 8) | (0u << 16) | (255u << 24), 0, GS_REG_RGBAQ);

        write_tag(buf, &off, 3, 0x555);
        append_xyz2_packed(buf, &off, (uint32_t)(5 << 4),  (uint32_t)(5 << 4),  0);
        append_xyz2_packed(buf, &off, (uint32_t)(5 << 4),  (uint32_t)(15 << 4), 0);
        append_xyz2_packed(buf, &off, (uint32_t)(15 << 4), (uint32_t)(15 << 4), 0);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        gif_state_t *st = gif_get_state();
        CHECK(st->lines_drawn == 2, "LINE_STRIP: 3 vertices produce exactly 2 connected segments");
        uint32_t green = (255u << 24) | (0u << 16) | (255u << 8) | 0u;
        CHECK(gs_mem_read_psmct32(0, 640, 5, 10) == green, "LINE_STRIP: a pixel on the 1st segment (vertical) is drawn");
        CHECK(gs_mem_read_psmct32(0, 640, 10, 15) == green, "LINE_STRIP: a pixel on the 2nd segment (horizontal, sharing vertex 1) is drawn");
    }

    { /* Z-buffer interaction: a LINE whose Z fails the depth test over
       * a pre-populated Z-buffer must not touch color/Z at all. */
        gs_mem_init();
        gif_init();
        uint8_t buf[16 * 16];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        write_tag(buf, &off, 6, 0xE);
        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        /* ZBP is a real 9-bit hardware field (0-511, masked in
         * gif.c's GS_REG_ZBUF_1 handler) - must be far enough from 0
         * that gs_mem's simplified flat addressing (bp*4 +
         * (y*bw+x)*4 - see gs_mem.h, where zbp directly becomes an
         * X-pixel offset in that formula) doesn't alias the Z
         * buffer's test pixel with any COLOR pixel this line actually
         * draws (x=30..40) - 200 is comfortably outside that span and
         * within the real 0-511 field range. */
        append_ad(buf, &off, 200u, 0, GS_REG_ZBUF_1);
        append_ad(buf, &off, (1u << 16) | (GS_ZTST_GEQUAL << 17), 0, GS_REG_TEST_1);
        append_ad(buf, &off, PRIM_TYPE_LINE, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0u | (0u << 8) | (0u << 16) | (255u << 24), 0, GS_REG_RGBAQ);

        write_tag(buf, &off, 2, 0x55);
        append_xyz2_packed(buf, &off, (uint32_t)(30 << 4), (uint32_t)(30 << 4), 100u);
        append_xyz2_packed(buf, &off, (uint32_t)(40 << 4), (uint32_t)(30 << 4), 100u);

        gs_mem_write_psmct32(200, 640, 35, 30, 200u);
        uint32_t before = gs_mem_read_psmct32(0, 640, 35, 30);

        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        gif_state_t *st = gif_get_state();
        CHECK(st->pixels_ztest_failed >= 1, "LINE + Z-test: at least one fragment counted as Z-test-failed");
        CHECK(gs_mem_read_psmct32(0, 640, 35, 30) == before, "LINE + Z-test: color buffer at the failing pixel is untouched");
        CHECK(gs_mem_read_psmct32(200, 640, 35, 30) == 200u, "LINE + Z-test: Z-buffer at the failing pixel keeps its old value");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
