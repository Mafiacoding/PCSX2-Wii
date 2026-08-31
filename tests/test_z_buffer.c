/*
 * test_z_buffer.c - host-native test for the Z-buffer / depth-test
 * implementation added to source/hw/gif.c (task #89, task 6: "Add
 * ZBUF register, Z value per vertex, Z-buffer storage, and depth
 * test for triangle rasterization"). See include/core/hw/gif.h's
 * ZBUF_1/TEST_1/GS_ZTST_* comments and the tri_z/v0z field comments
 * for the full scope and citations (PCSX2's GS/GSRegs.h GIFRegZBUF/
 * GIFRegTEST/GS_ZTST).
 *
 * IMPORTANT: unlike every other existing GIF test in this project,
 * the XYZ2 vertices here are sent via genuine PACKED-mode GIF
 * packets (GIFPackedXYZ2: X in word0, Y in word1, Z in word2 - a
 * real, cross-checked layout), NOT the A+D convention every other
 * test/demo uses. This project's existing A+D XYZ2 convention
 * (word0=X-only, word1=Y-only, established before this round across
 * every other test file and main.c) has no room left for a real Z
 * value - see gif.h's tri_z field comment for the full explanation.
 * PACKED mode was, before this round, a completely unused code path
 * for XYZ2 in this codebase, so building tests around it here is
 * purely additive and carries zero regression risk to the other 51
 * tests (confirmed by the full regression run alongside this file).
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

/* Builds one real GIFTag (matches process_one_packet()'s own parsing:
 * NLOOP bits0-14, PRE bit14, PRIM bits15-25, FLG bits26-27, NREG
 * bits28-31 of word1; REGS in words 2-3). Every packet in this test
 * uses nreg=1 (so only the REGS field's nibble 0, i.e. all of word2's
 * low nibble, matters) and flg=0 (PACKED). */
static void write_tag(uint8_t *buf, int *off, uint32_t nloop, uint32_t regs_nibble0)
{
    uint32_t w0 = nloop & 0x7FFFu;
    uint32_t w1 = (0u << 26) /* FLG = PACKED */ | (1u << 28) /* NREG = 1 */;
    uint32_t w2 = regs_nibble0 & 0xFu;
    uint32_t w3 = 0u;
    wle32(buf + *off, w0); wle32(buf + *off + 4, w1);
    wle32(buf + *off + 8, w2); wle32(buf + *off + 12, w3);
    *off += 16;
}

/* One A+D (address+data) loop entry - DATA in words 0-1, ADDR in
 * word2's low byte (matches every existing test's append_ad helper). */
static void append_ad(uint8_t *buf, int *off, uint32_t data_lo, uint32_t data_hi, uint32_t addr)
{
    wle32(buf + *off, data_lo); wle32(buf + *off + 4, data_hi);
    wle32(buf + *off + 8, addr); wle32(buf + *off + 12, 0);
    *off += 16;
}

/* One genuine PACKED-mode XYZ2 loop entry: X in word0 (low 16 bits),
 * Y in word1 (low 16 bits), Z as the ENTIRE word2 (real, full 32-bit
 * value) - cross-checked against PCSX2's GS/GSRegs.h GIFPackedXYZ2. */
static void append_xyz2_packed(uint8_t *buf, int *off, uint32_t x_raw, uint32_t y_raw, uint32_t z_raw)
{
    wle32(buf + *off, x_raw); wle32(buf + *off + 4, y_raw);
    wle32(buf + *off + 8, z_raw); wle32(buf + *off + 12, 0) /* ADC, unused */;
    *off += 16;
}

/* Sends a "setup" packet (one A+D-mode tag covering PRIM/RGBAQ/FRAME_1/
 * XYOFFSET_1, plus optionally ZBUF_1/TEST_1) followed by a separate
 * PACKED-mode XYZ2 tag carrying 3 vertices (with real, distinct Z
 * values) - i.e. one flat or Gouraud TRIANGLE draw. */
static void draw_triangle(uint32_t prim, uint32_t rgba,
                           int32_t x0, int32_t y0, uint32_t z0,
                           int32_t x1, int32_t y1, uint32_t z1,
                           int32_t x2, int32_t y2, uint32_t z2)
{
    uint8_t buf[16 * 8];
    memset(buf, 0, sizeof(buf));
    int off = 0;

    write_tag(buf, &off, /*nloop=*/2, GIF_REG_AD);
    append_ad(buf, &off, prim, 0, GS_REG_PRIM);
    append_ad(buf, &off, rgba, 0, GS_REG_RGBAQ);

    write_tag(buf, &off, /*nloop=*/3, GIF_REG_XYZ2);
    append_xyz2_packed(buf, &off, (uint32_t)(x0 << 4), (uint32_t)(y0 << 4), z0);
    append_xyz2_packed(buf, &off, (uint32_t)(x1 << 4), (uint32_t)(y1 << 4), z1);
    append_xyz2_packed(buf, &off, (uint32_t)(x2 << 4), (uint32_t)(y2 << 4), z2);

    gif_process_quadwords(0, buf, (uint32_t)(off / 16));
}

/* One-time setup packet: FRAME_1 (bp=0, bw=64), XYOFFSET_1 (0,0). */
static void setup_frame(void)
{
    uint8_t buf[16 * 4];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    write_tag(buf, &off, /*nloop=*/2, GIF_REG_AD);
    append_ad(buf, &off, (1u << 9), 0, GS_REG_FRAME_1); /* FBW field=1 -> fbw=64 */
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    gif_process_quadwords(0, buf, (uint32_t)(off / 16));
}

static void setup_zbuf(uint32_t zbp, uint32_t zmsk)
{
    uint8_t buf[16 * 2];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    write_tag(buf, &off, /*nloop=*/1, GIF_REG_AD);
    append_ad(buf, &off, (zbp & 0x1FFu), (zmsk & 0x1u), GS_REG_ZBUF_1);
    gif_process_quadwords(0, buf, (uint32_t)(off / 16));
}

static void setup_test(uint32_t zte, uint32_t ztst)
{
    uint8_t buf[16 * 2];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    write_tag(buf, &off, /*nloop=*/1, GIF_REG_AD);
    uint32_t data_lo = ((zte & 0x1u) << 16) | ((ztst & 0x3u) << 17);
    append_ad(buf, &off, data_lo, 0, GS_REG_TEST_1);
    gif_process_quadwords(0, buf, (uint32_t)(off / 16));
}

/* SPRITE draw with distinct per-vertex Z (to test the flat/"second
 * vertex" Z convention - see rasterize_sprite()'s comment). */
static void draw_sprite(uint32_t rgba, int32_t x0, int32_t y0, uint32_t z0, int32_t x1, int32_t y1, uint32_t z1)
{
    uint8_t buf[16 * 8];
    memset(buf, 0, sizeof(buf));
    int off = 0;

    write_tag(buf, &off, /*nloop=*/2, GIF_REG_AD);
    append_ad(buf, &off, PRIM_TYPE_SPRITE, 0, GS_REG_PRIM);
    append_ad(buf, &off, rgba, 0, GS_REG_RGBAQ);

    write_tag(buf, &off, /*nloop=*/2, GIF_REG_XYZ2);
    append_xyz2_packed(buf, &off, (uint32_t)(x0 << 4), (uint32_t)(y0 << 4), z0);
    append_xyz2_packed(buf, &off, (uint32_t)(x1 << 4), (uint32_t)(y1 << 4), z1);

    gif_process_quadwords(0, buf, (uint32_t)(off / 16));
}

int main(void)
{
    /* --- ZBUF_1 / TEST_1 register parsing --- */
    {
        gs_mem_init();
        gif_init();
        setup_frame();
        setup_zbuf(32u, 0u);
        gif_state_t *st = gif_get_state();
        /* Round 748: ZBUF.ZBP is real-hardware page-granularity (Address/
         * 2048 words) - gif.c's ZBUF_1 handler now scales the raw field
         * by *2048 at decode time (matching gs_decode_dispfb()'s existing
         * DISPFB convention), so the stored word offset is 32*2048, not
         * the raw field value 32 itself. See docs/STATUS.md Round 748. */
        CHECK(st->zbp == 32u * 2048u, "ZBUF_1: ZBP field parsed correctly (scaled *2048, real page-granularity)");
        CHECK(st->zmsk == 0, "ZBUF_1: ZMSK field parsed correctly (0)");
        CHECK(st->zbuf_configured == 1, "ZBUF_1: zbuf_configured gate set after an explicit write");

        setup_test(1u, GS_ZTST_GEQUAL);
        CHECK(st->zte == 1, "TEST_1: ZTE field parsed correctly");
        CHECK(st->ztst == (int)GS_ZTST_GEQUAL, "TEST_1: ZTST field parsed correctly (GEQUAL)");
    }

    /* --- Genuine per-vertex Z interpolation (centroid proof, task #88's
     * technique reapplied to Z): triangle (0,0)/(9,0)/(0,9) has centroid
     * (3,3) with EXACT barycentric weights (1/3,1/3,1/3) for ANY
     * triangle. Distinct Z0=0, Z1=300, Z2=600 average to exactly 300 -
     * an integer, so no rounding ambiguity. ZTST_ALWAYS so every pixel
     * is written unconditionally, isolating interpolation from the
     * depth-test pass/fail logic tested separately below. */
    {
        gs_mem_init();
        gif_init();
        setup_frame();
        setup_zbuf(32u, 0u);
        setup_test(1u, GS_ZTST_ALWAYS);

        draw_triangle(PRIM_TYPE_TRIANGLE, 0xFF0000FFu,
                       0, 0, 0u, 9, 0, 300u, 0, 9, 600u);

        uint32_t stored_z = gs_mem_read_psmct32(32u * 2048u, 64u, 3u, 3u);
        CHECK(stored_z == 300u, "Z buffer: centroid Z is the true barycentric average (0+300+600)/3 = 300, not a guess");
    }

    /* --- Depth test rejects a fragment that fails GEQUAL, keeps
     * accepting ones that pass --- */
    {
        gs_mem_init();
        gif_init();
        setup_frame();
        setup_zbuf(32u, 0u);
        setup_test(1u, GS_ZTST_GEQUAL);

        /* Triangle A: Z=100, red, covers (5,5). */
        draw_triangle(PRIM_TYPE_TRIANGLE, 0x000000FFu, 0, 0, 100u, 20, 0, 100u, 0, 20, 100u);
        uint32_t after_a = gs_mem_read_psmct32(0u, 64u, 5u, 5u);
        CHECK(after_a == 0x000000FFu, "Depth test: first triangle (no prior Z) draws normally");

        uint64_t failed_before = gif_get_state()->pixels_ztest_failed;
        /* Triangle B: Z=50 (< stored 100) - GEQUAL must reject every pixel. */
        draw_triangle(PRIM_TYPE_TRIANGLE, 0x0000FF00u, 0, 0, 50u, 20, 0, 50u, 0, 20, 50u);
        uint32_t after_b = gs_mem_read_psmct32(0u, 64u, 5u, 5u);
        CHECK(after_b == after_a, "Depth test: a farther fragment (Z=50 < stored 100) under GEQUAL does NOT overwrite color");
        CHECK(gif_get_state()->pixels_ztest_failed > failed_before, "Depth test: rejected fragments are counted (pixels_ztest_failed increased)");

        /* Triangle C: Z=150 (>= stored 100) - GEQUAL must accept. */
        draw_triangle(PRIM_TYPE_TRIANGLE, 0x00FF0000u, 0, 0, 150u, 20, 0, 150u, 0, 20, 150u);
        uint32_t after_c = gs_mem_read_psmct32(0u, 64u, 5u, 5u);
        CHECK(after_c == 0x00FF0000u, "Depth test: a nearer-or-equal fragment (Z=150 >= stored 100) under GEQUAL DOES overwrite color");
        CHECK(gs_mem_read_psmct32(32u * 2048u, 64u, 5u, 5u) == 150u, "Depth test: Z buffer updated to the passing fragment's Z (150)");
    }

    /* --- ZTST_NEVER: every fragment rejected regardless of Z --- */
    {
        gs_mem_init();
        gif_init();
        setup_frame();
        setup_zbuf(32u, 0u);
        setup_test(1u, GS_ZTST_ALWAYS);
        draw_triangle(PRIM_TYPE_TRIANGLE, 0x11111111u, 0, 0, 5u, 20, 0, 5u, 0, 20, 5u);
        uint32_t baseline = gs_mem_read_psmct32(0u, 64u, 5u, 5u);

        setup_test(1u, GS_ZTST_NEVER);
        draw_triangle(PRIM_TYPE_TRIANGLE, 0x22222222u, 0, 0, 9999u, 20, 0, 9999u, 0, 20, 9999u);
        CHECK(gs_mem_read_psmct32(0u, 64u, 5u, 5u) == baseline, "ZTST_NEVER: fragment rejected even with an overwhelmingly larger Z");
    }

    /* --- ZMSK: color is written, but the Z buffer itself is NOT
     * updated when ZMSK=1 --- */
    {
        gs_mem_init();
        gif_init();
        setup_frame();
        setup_zbuf(32u, 0u);
        setup_test(1u, GS_ZTST_ALWAYS);
        draw_triangle(PRIM_TYPE_TRIANGLE, 0x000000FFu, 0, 0, 10u, 20, 0, 10u, 0, 20, 10u);
        CHECK(gs_mem_read_psmct32(32u * 2048u, 64u, 5u, 5u) == 10u, "ZMSK=0: Z buffer written normally");

        setup_zbuf(32u, 1u); /* now ZMSK=1 */
        draw_triangle(PRIM_TYPE_TRIANGLE, 0x0000FF00u, 0, 0, 999u, 20, 0, 999u, 0, 20, 999u);
        CHECK(gs_mem_read_psmct32(0u, 64u, 5u, 5u) == 0x0000FF00u, "ZMSK=1: color is still written normally (ALWAYS test)");
        CHECK(gs_mem_read_psmct32(32u * 2048u, 64u, 5u, 5u) == 10u, "ZMSK=1: Z buffer was NOT updated (still the old value, 10, not 999)");

        /* Prove the stale Z is really what's being compared against:
         * switch back to GEQUAL with ZMSK=0, draw at Z=15 (>= stale
         * 10, so it should pass; it would fail against 999). */
        setup_zbuf(32u, 0u);
        setup_test(1u, GS_ZTST_GEQUAL);
        draw_triangle(PRIM_TYPE_TRIANGLE, 0x00FF0000u, 0, 0, 15u, 20, 0, 15u, 0, 20, 15u);
        CHECK(gs_mem_read_psmct32(0u, 64u, 5u, 5u) == 0x00FF0000u, "ZMSK proof: Z=15 passes GEQUAL against the stale stored Z=10 (confirms 999 was never actually stored)");
    }

    /* --- No ZBUF_1 ever configured: depth test is fully bypassed,
     * matching this project's pre-task-#89 behavior exactly (safety
     * gate - see gif.h's zbuf_configured comment) --- */
    {
        gs_mem_init();
        gif_init();
        setup_frame();
        /* Deliberately no setup_zbuf()/setup_test() call. */
        draw_triangle(PRIM_TYPE_TRIANGLE, 0x000000FFu, 0, 0, 1000u, 20, 0, 1000u, 0, 20, 1000u);
        draw_triangle(PRIM_TYPE_TRIANGLE, 0x0000FF00u, 0, 0, 0u, 20, 0, 0u, 0, 20, 0u);
        CHECK(gs_mem_read_psmct32(0u, 64u, 5u, 5u) == 0x0000FF00u,
              "No ZBUF configured: second draw (Z=0) overwrites first (Z=1000) unconditionally - zero regression vs. pre-#89 behavior");
    }

    /* --- SPRITE: flat Z uses the second (completing) vertex, and the
     * depth test applies to SPRITE too --- */
    {
        gs_mem_init();
        gif_init();
        setup_frame();
        setup_zbuf(32u, 0u);
        setup_test(1u, GS_ZTST_GREATER);

        /* v0 Z=5 (should be ignored), v1(completing) Z=200 (flat Z for the whole sprite). */
        draw_sprite(0x000000FFu, 0, 0, 5u, 10, 10, 200u);
        CHECK(gs_mem_read_psmct32(0u, 64u, 5u, 5u) == 0x000000FFu, "SPRITE: first draw establishes color/Z=200 (flat, from the completing vertex)");
        CHECK(gs_mem_read_psmct32(32u * 2048u, 64u, 5u, 5u) == 200u, "SPRITE: Z buffer holds the completing vertex's Z (200), not the first vertex's (5)");

        /* Completing Z=100 (not > stored 200) - GREATER must reject. */
        draw_sprite(0x0000FF00u, 0, 0, 999u, 10, 10, 100u);
        CHECK(gs_mem_read_psmct32(0u, 64u, 5u, 5u) == 0x000000FFu, "SPRITE depth test: Z=100 does not beat stored 200 under GREATER - color unchanged");

        /* Completing Z=300 (> stored 200) - GREATER must accept. */
        draw_sprite(0x00FF0000u, 0, 0, 1u, 10, 10, 300u);
        CHECK(gs_mem_read_psmct32(0u, 64u, 5u, 5u) == 0x00FF0000u, "SPRITE depth test: Z=300 beats stored 200 under GREATER - color updated");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
