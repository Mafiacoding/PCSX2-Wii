/* test_gif_gouraud.c - host-native test for gif.c's Gouraud-shading
 * support on TRIANGLE primitives (GS round: task #78), driven by
 * PRIM's real IIP bit (bit 3, mask 0x8) - confirmed against PCSX2's
 * own GS/GSRegs.h GIFRegPRIM bitfield layout (see docs/STATUS.md).
 *
 * Triangle used throughout: (0,0)-(60,0)-(0,60), a right triangle with
 * area 3600. For any interior point (x,y), the barycentric weights
 * work out to a clean closed form:
 *   b0 = 1 - (x+y)/60   (vertex0 weight, dominant near (0,0))
 *   b1 = x/60           (vertex1 weight, dominant near (60,0))
 *   b2 = y/60           (vertex2 weight, dominant near (0,60))
 * so sample points near each vertex should read back close to that
 * vertex's color, and the centroid should read back an equal blend of
 * all three (since each color channel is nonzero in only one vertex's
 * color below).
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

/* Builds and processes one A+D GIF packet: FRAME_1(fbp=0,fbw=640) +
 * XYOFFSET_1(0,0) + PRIM(prim_type) + per-vertex (RGBAQ(colors[i]) +
 * XYZ2(verts[i])) - i.e., each vertex can carry its own color, unlike
 * test_gif_triangle.c's single shared-color helper. */
static void run_packet_colored(uint32_t prim_type, const uint32_t *colors, const int32_t *verts, int n_verts)
{
    uint8_t buf[16 * (3 + 2 * 8)];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    int nloop = 3 + 2 * n_verts;

    wle32(buf + off,     (uint32_t)nloop | (1u << 15));
    wle32(buf + off + 4, (0u << 26) | (1u << 28));
    wle32(buf + off + 8, GIF_REG_AD);
    wle32(buf + off + 12, 0);
    off += 16;

    append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    append_ad(buf, &off, prim_type, 0, GS_REG_PRIM);
    for (int i = 0; i < n_verts; i++) {
        uint32_t rgba = colors[i];
        uint32_t rgbaq_lo = (rgba & 0xFFu) | (((rgba >> 8) & 0xFFu) << 8) | (((rgba >> 16) & 0xFFu) << 16) | (((rgba >> 24) & 0xFFu) << 24);
        append_ad(buf, &off, rgbaq_lo, 0, GS_REG_RGBAQ);
        int32_t px = verts[i*2], py = verts[i*2+1];
        append_ad(buf, &off, (uint32_t)(px << 4), (uint32_t)(py << 4), GS_REG_XYZ2);
    }

    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));
}

static uint8_t chan(uint32_t rgba, int shift) { return (uint8_t)((rgba >> shift) & 0xFFu); }

int main(void) {
    /* rgba packing throughout matches g_gif.rgba: (a<<24)|(b<<16)|(g<<8)|r */
    uint32_t red   = 0xFF0000FFu; /* r=255,g=0,  b=0,  a=255 */
    uint32_t green = 0xFF00FF00u; /* r=0,  g=255,b=0,  a=255 */
    uint32_t blue  = 0xFFFF0000u; /* r=0,  g=0,  b=255,a=255 */

    int32_t verts[] = { 0,0, 60,0, 0,60 };
    uint32_t colors[] = { red, green, blue };

    /* --- Gouraud (IIP=1): distinct per-vertex colors interpolate --- */
    gs_mem_init();
    gif_init();
    run_packet_colored(PRIM_TYPE_TRIANGLE | PRIM_IIP_MASK, colors, verts, 3);
    {
        gif_state_t *st = gif_get_state();
        CHECK(st->triangles_drawn == 1, "Gouraud: exactly one triangle drawn");

        uint32_t near_v0 = gs_mem_read_psmct32(0, 640, 5, 5);
        CHECK(chan(near_v0, 0) > 200 && chan(near_v0, 8) < 60 && chan(near_v0, 16) < 60,
              "Gouraud: sample near vertex0 (0,0) is dominated by red");

        uint32_t near_v1 = gs_mem_read_psmct32(0, 640, 50, 5);
        CHECK(chan(near_v1, 8) > 200 && chan(near_v1, 0) < 60 && chan(near_v1, 16) < 60,
              "Gouraud: sample near vertex1 (60,0) is dominated by green");

        uint32_t near_v2 = gs_mem_read_psmct32(0, 640, 5, 50);
        CHECK(chan(near_v2, 16) > 200 && chan(near_v2, 0) < 60 && chan(near_v2, 8) < 60,
              "Gouraud: sample near vertex2 (0,60) is dominated by blue");

        uint32_t center = gs_mem_read_psmct32(0, 640, 20, 20);
        CHECK(chan(center, 0) > 70 && chan(center, 0) < 100 &&
              chan(center, 8) > 70 && chan(center, 8) < 100 &&
              chan(center, 16) > 70 && chan(center, 16) < 100,
              "Gouraud: centroid is a roughly equal blend of all 3 vertex colors (~85,85,85), not any single pure color");
        CHECK(chan(center, 24) > 240, "Gouraud: interpolated alpha also blends correctly (all 3 vertices used a=255)");
    }

    /* --- Flat (IIP=0): same distinct per-vertex colors, but the whole
     * triangle must use ONLY the last vertex's color (blue) - proves
     * per-vertex color capture didn't silently turn on Gouraud
     * shading for the existing flat path, and matches
     * test_gif_triangle.c's pre-existing flat-shading contract. --- */
    gs_mem_init();
    gif_init();
    run_packet_colored(PRIM_TYPE_TRIANGLE, colors, verts, 3);
    {
        gif_state_t *st = gif_get_state();
        CHECK(st->triangles_drawn == 1, "Flat: exactly one triangle drawn");

        uint32_t near_v0 = gs_mem_read_psmct32(0, 640, 5, 5);
        uint32_t near_v1 = gs_mem_read_psmct32(0, 640, 50, 5);
        uint32_t near_v2 = gs_mem_read_psmct32(0, 640, 5, 50);
        uint32_t center  = gs_mem_read_psmct32(0, 640, 20, 20);

        CHECK(near_v0 == blue && near_v1 == blue && near_v2 == blue && center == blue,
              "Flat (IIP=0): every sampled point uses only the last vertex's color (blue), no interpolation");
    }

    /* --- Flat SPRITE regression: unrelated primitive type must still
     * work unchanged (SPRITE has no IIP concept - always flat). --- */
    gs_mem_init();
    gif_init();
    {
        uint32_t sprite_colors[] = { red, red };
        int32_t sprite_verts[] = { 10,10, 30,30 };
        run_packet_colored(PRIM_TYPE_SPRITE, sprite_colors, sprite_verts, 2);
    }
    CHECK(gs_mem_read_psmct32(0, 640, 15, 15) == red, "SPRITE regression: still flat-fills correctly after Gouraud changes");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
