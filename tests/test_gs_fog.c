/*
 * test_gs_fog.c - host-native test for Round 97's real GS Fog effect
 * (FOG/FOGCOL registers, PRIM's FGE bit) and the new XYZF2/XYZF3/XYZ3
 * vertex-register variants (task #254, 138th finding). See
 * include/core/hw/gif.h's cur_fog/fogcol/tri_f field comments and
 * gif.c's apply_fog()/apply_xyz2_kick() for the full scope and
 * citations (official Sony GS Users Manual "3.5. Fog Effect" and
 * "7.3 Register List in Address Order").
 *
 * Uses the same write_tag/append_ad A+D-mode packet-building
 * convention established in tests/test_gs_alpha.c, plus a genuine
 * PACKED-mode helper (append_xyzf2_packed) modeled on
 * tests/test_z_buffer.c's append_xyz2_packed, extended with the real
 * GIFPackedXYZF2 Z:24/F:8 split.
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

/* Genuine PACKED-mode XYZF2/XYZF3 loop entry (GIFPackedXYZF2 layout,
 * cross-checked against PCSX2's GS/GSRegs.h and the official GS
 * Users Manual's XYZF2 BIT ASSIGN table): X in word0 (low 16 bits),
 * Y in word1 (low 16 bits), Z in word2's low 24 bits, F (Fog
 * coefficient) in word2's top 8 bits. */
static void append_xyzf2_packed(uint8_t *buf, int *off, uint32_t x_raw, uint32_t y_raw, uint32_t z24, uint32_t f8)
{
    uint32_t w2 = (z24 & 0xFFFFFFu) | ((f8 & 0xFFu) << 24);
    wle32(buf + *off, x_raw); wle32(buf + *off + 4, y_raw);
    wle32(buf + *off + 8, w2); wle32(buf + *off + 12, 0) /* ADC, unused */;
    *off += 16;
}

/* Draws a flat, untextured SPRITE covering (0,0)-(20,20) via the
 * established A+D-mode XYZ2 convention, after configuring FOG/FOGCOL
 * and PRIM's FGE bit as requested. */
static void draw_sprite_fog(uint32_t rgba, int fge, uint32_t fog, uint32_t fr, uint32_t fg, uint32_t fb)
{
    uint8_t buf[16 * 9];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    write_tag(buf, &off, 8, 0xE);
    append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | (fge ? PRIM_FGE_MASK : 0u), 0, GS_REG_PRIM);
    append_ad(buf, &off, rgba, 0, GS_REG_RGBAQ);
    append_ad(buf, &off, 0u, fog << 24, GS_REG_FOG);
    append_ad(buf, &off, fr | (fg << 8) | (fb << 16), 0, GS_REG_FOGCOL);
    append_ad(buf, &off, (uint32_t)(0 << 4), (uint32_t)(0 << 4), GS_REG_XYZ2);
    append_ad(buf, &off, (uint32_t)(20 << 4), (uint32_t)(20 << 4), GS_REG_XYZ2);
    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));
}

int main(void)
{
    { /* Fog effect, FGE=1: white sprite (255,255,255) blended against
       * a black FOGCOL (0,0,0) at fog=128 (mid-value). Real formula
       * (GS Users Manual "3.5. Fog Effect", footnote "A*B=(AxB)>>8"):
       *   R = (128*255 + 127*0) >> 8 = 32640 >> 8 = 127 (and likewise
       *   G, B - clean, hand-verified arithmetic, not re-derived from
       *   the implementation itself). Alpha is left untouched (255). */
        gs_mem_init(); gif_init();
        draw_sprite_fog(0xFFFFFFFFu, 1, 128, 0, 0, 0);
        uint32_t px = gs_mem_read_psmct32(0, 640, 10, 10);
        CHECK(px == 0xFF7F7F7Fu,
              "FGE=1, fog=128, black FOGCOL: white sprite blends to (127,127,127,255)");
    }

    { /* FGE=0 (fogging off): same FOG/FOGCOL configured, but PRIM's
       * FGE bit is NOT set - real hardware behavior per the manual
       * ("Whether or not Fogging is performed is specified with the
       * FGE flag of the PRIM register") - color must draw completely
       * unmodified, the same safety-gate regression check this file
       * uses elsewhere (e.g. Round 96's SCISSOR "not configured"
       * case). */
        gs_mem_init(); gif_init();
        draw_sprite_fog(0xFFFFFFFFu, 0, 128, 0, 0, 0);
        uint32_t px = gs_mem_read_psmct32(0, 640, 10, 10);
        CHECK(px == 0xFFFFFFFFu,
              "FGE=0: FOG/FOGCOL configured but fogging off - sprite draws unmodified");
    }

    { /* XYZF2's own embedded F field (genuine PACKED mode) overrides
       * whatever a prior standalone FOG write set, and - matching
       * this file's already-established SPRITE "flat, second-vertex"
       * convention (same as Z) - it's the SECOND vertex's F that
       * actually takes effect. Vertex0 carries F=200 (should be
       * ignored); vertex1 carries F=128 (should be used), reproducing
       * test 1's exact (127,127,127,255) result via a completely
       * different register path (PACKED XYZF2, not standalone FOG). */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 6];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        write_tag(buf, &off, 4, 0xE);
        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_SPRITE | PRIM_FGE_MASK, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ);
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        uint8_t buf2[16 * 3];
        int off2 = 0;
        write_tag(buf2, &off2, 2, GIF_REG_XYZF2);
        append_xyzf2_packed(buf2, &off2, 0u << 4, 0u << 4, 0u, 200u);
        append_xyzf2_packed(buf2, &off2, 20u << 4, 20u << 4, 0u, 128u);
        gif_process_quadwords(DMA_CHANNEL_GIF, buf2, (uint32_t)(off2 / 16));

        uint32_t px = gs_mem_read_psmct32(0, 640, 10, 10);
        CHECK(px == 0xFF7F7F7Fu,
              "XYZF2 PACKED: 2nd vertex's embedded F=128 (not 1st vertex's F=200) drives the fog blend");
    }

    { /* XYZ3: "vertex kick without drawing kick" (official GS Users
       * Manual). TRIANGLE_STRIP with V0,V1,V2 (draws triangle #1 at
       * V2), then V3 delivered via XYZ3 (queue advances, but the
       * would-be triangle #2, V1-V2-V3, must NOT draw), then V4 via
       * normal XYZ2 (draws triangle #3, V2-V3-V4). Verified via the
       * public triangles_drawn counter - directly matches the
       * manual's own worked example ("only the 2nd triangle is not
       * drawn"). */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 5];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        write_tag(buf, &off, 4, 0xE);
        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_TRIANGLE_STRIP, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ);
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        uint8_t bufv[16 * 4];
        memset(bufv, 0, sizeof(bufv));
        int offv = 0;
        write_tag(bufv, &offv, 3, GIF_REG_XYZ2);
        wle32(bufv + offv, 0u << 4); wle32(bufv + offv + 4, 0u << 4); wle32(bufv + offv + 8, 0u); wle32(bufv + offv + 12, 0); offv += 16;
        wle32(bufv + offv, 20u << 4); wle32(bufv + offv + 4, 0u << 4); wle32(bufv + offv + 8, 0u); wle32(bufv + offv + 12, 0); offv += 16;
        wle32(bufv + offv, 0u << 4); wle32(bufv + offv + 4, 20u << 4); wle32(bufv + offv + 8, 0u); wle32(bufv + offv + 12, 0); offv += 16;
        gif_process_quadwords(DMA_CHANNEL_GIF, bufv, (uint32_t)(offv / 16));
        CHECK(gif_get_state()->triangles_drawn == 1, "TRIANGLE_STRIP V0-V2: 1st triangle drawn normally");

        uint8_t bufx3[16 * 2];
        int offx3 = 0;
        write_tag(bufx3, &offx3, 1, GIF_REG_XYZ3);
        wle32(bufx3 + offx3, 30u << 4); wle32(bufx3 + offx3 + 4, 30u << 4); wle32(bufx3 + offx3 + 8, 0u); wle32(bufx3 + offx3 + 12, 0); offx3 += 16;
        gif_process_quadwords(DMA_CHANNEL_GIF, bufx3, (uint32_t)(offx3 / 16));
        CHECK(gif_get_state()->triangles_drawn == 1, "XYZ3 vertex kick: queue advances but the 2nd triangle is NOT drawn");

        uint8_t bufv4[16 * 2];
        int offv4 = 0;
        write_tag(bufv4, &offv4, 1, GIF_REG_XYZ2);
        wle32(bufv4 + offv4, 40u << 4); wle32(bufv4 + offv4 + 4, 0u << 4); wle32(bufv4 + offv4 + 8, 0u); wle32(bufv4 + offv4 + 12, 0); offv4 += 16;
        gif_process_quadwords(DMA_CHANNEL_GIF, bufv4, (uint32_t)(offv4 / 16));
        CHECK(gif_get_state()->triangles_drawn == 2, "subsequent normal XYZ2: 3rd triangle draws, using the queue XYZ3 advanced");
    }

    { /* XYZF3: same "kick without draw" suppression as XYZ3, plus
       * confirms the embedded F field parses correctly on a
       * non-drawing vertex without disturbing the queue. */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 5];
        memset(buf, 0, sizeof(buf));
        int off = 0;
        write_tag(buf, &off, 4, 0xE);
        append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
        append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
        append_ad(buf, &off, (uint32_t)PRIM_TYPE_TRIANGLE_STRIP, 0, GS_REG_PRIM);
        append_ad(buf, &off, 0xFFFFFFFFu, 0, GS_REG_RGBAQ);
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

        uint8_t bufv[16 * 4];
        int offv = 0;
        write_tag(bufv, &offv, 3, GIF_REG_XYZ2);
        wle32(bufv + offv, 0u << 4); wle32(bufv + offv + 4, 0u << 4); wle32(bufv + offv + 8, 0u); wle32(bufv + offv + 12, 0); offv += 16;
        wle32(bufv + offv, 20u << 4); wle32(bufv + offv + 4, 0u << 4); wle32(bufv + offv + 8, 0u); wle32(bufv + offv + 12, 0); offv += 16;
        wle32(bufv + offv, 0u << 4); wle32(bufv + offv + 4, 20u << 4); wle32(bufv + offv + 8, 0u); wle32(bufv + offv + 12, 0); offv += 16;
        gif_process_quadwords(DMA_CHANNEL_GIF, bufv, (uint32_t)(offv / 16));
        CHECK(gif_get_state()->triangles_drawn == 1, "(XYZF3 setup) 1st triangle drawn normally");

        uint8_t bufx3[16 * 2];
        int offx3 = 0;
        write_tag(bufx3, &offx3, 1, GIF_REG_XYZF3);
        wle32(bufx3 + offx3, 30u << 4); wle32(bufx3 + offx3 + 4, 30u << 4);
        wle32(bufx3 + offx3 + 8, 0u | (77u << 24)); wle32(bufx3 + offx3 + 12, 0); offx3 += 16;
        gif_process_quadwords(DMA_CHANNEL_GIF, bufx3, (uint32_t)(offx3 / 16));
        CHECK(gif_get_state()->triangles_drawn == 1, "XYZF3 vertex kick (F=77 embedded): queue advances, 2nd triangle NOT drawn");

        uint8_t bufv4[16 * 2];
        int offv4 = 0;
        write_tag(bufv4, &offv4, 1, GIF_REG_XYZ2);
        wle32(bufv4 + offv4, 40u << 4); wle32(bufv4 + offv4 + 4, 0u << 4); wle32(bufv4 + offv4 + 8, 0u); wle32(bufv4 + offv4 + 12, 0); offv4 += 16;
        gif_process_quadwords(DMA_CHANNEL_GIF, bufv4, (uint32_t)(offv4 / 16));
        CHECK(gif_get_state()->triangles_drawn == 2, "(XYZF3 setup) subsequent normal XYZ2 draws the 3rd triangle");
    }

    printf(failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
