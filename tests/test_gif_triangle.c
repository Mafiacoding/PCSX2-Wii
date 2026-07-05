/* test_gif_triangle.c - host-native test for gif.c's TRIANGLE/
 * TRIANGLE_STRIP/TRIANGLE_FAN support (GS round 1: first flat-shaded
 * triangle primitive, added alongside the existing SPRITE rasterizer -
 * see include/core/hw/gif.h's scope comment).
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
 * XYOFFSET_1(0,0) + PRIM(prim_type) + RGBAQ(color) + one XYZ2 per
 * vertex in `verts` (n_verts pairs of pixel x,y). */
static void run_packet(uint32_t prim_type, uint32_t rgba_lo, const int32_t *verts, int n_verts)
{
    uint8_t buf[16 * (4 + 8)];
    memset(buf, 0, sizeof(buf));
    int off = 0;
    int nloop = 4 + n_verts;

    wle32(buf + off,     (uint32_t)nloop | (1u << 15));
    wle32(buf + off + 4, (0u << 26) | (1u << 28));
    wle32(buf + off + 8, GIF_REG_AD);
    wle32(buf + off + 12, 0);
    off += 16;

    append_ad(buf, &off, (10u << 9), 0, GS_REG_FRAME_1);
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    append_ad(buf, &off, prim_type, 0, GS_REG_PRIM);
    append_ad(buf, &off, rgba_lo, 0, GS_REG_RGBAQ);
    for (int i = 0; i < n_verts; i++) {
        int32_t px = verts[i*2], py = verts[i*2+1];
        append_ad(buf, &off, (uint32_t)(px << 4), (uint32_t)(py << 4), GS_REG_XYZ2);
    }

    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));
}

int main(void) {
    uint32_t green = 0u | (255u << 8) | (0u << 16) | (255u << 24); /* opaque green, 0xAABBGGRR = 0xFF00FF00 */

    /* --- Plain TRIANGLE (type 3): right triangle (10,10)-(30,10)-(10,30) --- */
    gs_mem_init();
    gif_init();
    {
        int32_t verts[] = { 10,10, 30,10, 10,30 };
        run_packet(PRIM_TYPE_TRIANGLE, green, verts, 3);
    }
    gif_state_t *st = gif_get_state();
    CHECK((st->prim & 0x7u) == PRIM_TYPE_TRIANGLE, "PRIM set to TRIANGLE");
    CHECK(st->triangles_drawn == 1, "exactly one triangle drawn for a plain TRIANGLE");
    CHECK(gs_mem_read_psmct32(0, 640, 13, 13) == green, "interior point of the triangle is green");
    CHECK(gs_mem_read_psmct32(0, 640, 25, 25) == 0, "point outside the hypotenuse (x+y > 40) is untouched");
    CHECK(gs_mem_read_psmct32(0, 640, 5, 5) == 0, "point outside the triangle entirely (below/left of it) is untouched");

    /* --- TRIANGLE_STRIP (type 4): quad (0,0)-(20,0)-(0,20)-(20,20),
     * split by the v1-v2 shared edge into 2 triangles covering the
     * whole square. --- */
    gs_mem_init();
    gif_init();
    {
        int32_t verts[] = { 0,0, 20,0, 0,20, 20,20 };
        run_packet(PRIM_TYPE_TRIANGLE_STRIP, green, verts, 4);
    }
    st = gif_get_state();
    CHECK(st->triangles_drawn == 2, "TRIANGLE_STRIP with 4 vertices draws exactly 2 triangles");
    CHECK(gs_mem_read_psmct32(0, 640, 3, 3) == green, "upper-left region (first strip triangle) is green");
    CHECK(gs_mem_read_psmct32(0, 640, 16, 16) == green, "lower-right region (second strip triangle) is green");

    /* --- TRIANGLE_FAN (type 5): square (10,10)-(30,10)-(30,30)-(10,30)
     * as a fan anchored at the first vertex - 2 triangles covering the
     * whole square. --- */
    gs_mem_init();
    gif_init();
    {
        int32_t verts[] = { 10,10, 30,10, 30,30, 10,30 };
        run_packet(PRIM_TYPE_TRIANGLE_FAN, green, verts, 4);
    }
    st = gif_get_state();
    CHECK(st->triangles_drawn == 2, "TRIANGLE_FAN with 4 vertices draws exactly 2 triangles");
    CHECK(gs_mem_read_psmct32(0, 640, 25, 13) == green, "region covered by the fan's first triangle (near anchor+v1+v2) is green");
    CHECK(gs_mem_read_psmct32(0, 640, 13, 25) == green, "region covered by the fan's second triangle (near anchor+v2+v3) is green");
    CHECK(gs_mem_read_psmct32(0, 640, 5, 5) == 0, "point outside the fan's square entirely is untouched");

    /* --- A PRIM change mid-stream resets the vertex sequence: after
     * a TRIANGLE_STRIP draws, switching PRIM to SPRITE and back should
     * not let stale strip vertices leak into a new triangle. --- */
    gs_mem_init();
    gif_init();
    {
        int32_t verts[] = { 0,0, 20,0, 0,20 }; /* one triangle, uses up the strip's first 3 slots */
        run_packet(PRIM_TYPE_TRIANGLE_STRIP, green, verts, 3);
    }
    st = gif_get_state();
    uint64_t before = st->triangles_drawn;
    {
        /* Switch to TRIANGLE_FAN and supply only 2 vertices - must NOT
         * draw anything yet (a fresh sequence needs its own 3rd vertex,
         * not leftovers from the strip above). */
        int32_t verts[] = { 100,100, 120,100 };
        run_packet(PRIM_TYPE_TRIANGLE_FAN, green, verts, 2);
    }
    CHECK(st->triangles_drawn == before, "changing PRIM mid-stream resets the vertex sequence (no stale-vertex triangle draws)");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
