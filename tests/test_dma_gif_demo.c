/* test_dma_gif_demo.c - host-native test mirroring main.c's "real
 * GIF-packet demo" (added this round): builds the exact same A+D GIF
 * packet (FRAME_1 + XYOFFSET_1 + PRIM(TRIANGLE|IIP) + 3x(RGBAQ+XYZ2)
 * for a red/green/blue Gouraud triangle), but instead of writing it
 * into real Wii-target EE RAM and calling dma_channel_kick() on real
 * hardware, drives the exact same dma.c/gif.c code host-natively so
 * the packet layout (byte counts, nloop, register nibbles) can be
 * verified BEFORE trusting a devkitPPC-only compile to prove it's
 * correct - a clean Wii rebuild proves the code compiles, not that
 * the packet bytes are well-formed.
 */
#include <stdio.h>
#include <string.h>
#include "hw/gs_mem.c"
#include "hw/gif.c"
#include "hw/dma.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint8_t chan_byte(uint32_t rgba, int shift) { return (uint8_t)((rgba >> shift) & 0xFFu); }

int main(void)
{
    static uint8_t fake_ram[2 * 1024 * 1024];
    memset(fake_ram, 0, sizeof(fake_ram));

    gs_mem_init();
    gif_init();
    dma_init();
    dma_bind_ee_ram(fake_ram, sizeof(fake_ram));
    dma_set_sink(DMA_CHANNEL_GIF, gif_process_quadwords);

    /* --- Exact packet-building logic mirrored from main.c --- */
    static uint8_t pkt[16 * (1 + 3 + 3 * 2)];
    memset(pkt, 0, sizeof(pkt));
    int off = 0;

#define WLE32(p, v) do { \
    uint32_t _v = (uint32_t)(v); \
    (p)[0] = (uint8_t)(_v);       (p)[1] = (uint8_t)(_v >> 8); \
    (p)[2] = (uint8_t)(_v >> 16); (p)[3] = (uint8_t)(_v >> 24); \
} while (0)
#define APPEND_AD(data_lo, data_hi, addr) do { \
    WLE32(pkt + off,      (data_lo)); \
    WLE32(pkt + off + 4,  (data_hi)); \
    WLE32(pkt + off + 8,  (addr));    \
    WLE32(pkt + off + 12, 0);         \
    off += 16; \
} while (0)

    const int n_verts = 3;
    const int nloop = 3 + 2 * n_verts;
    const uint32_t fbWidth = 640;

    WLE32(pkt + off,     (uint32_t)nloop | (1u << 15));
    WLE32(pkt + off + 4, (0u << 26) | (1u << 28));
    WLE32(pkt + off + 8, GIF_REG_AD);
    WLE32(pkt + off + 12, 0);
    off += 16;

    uint32_t fbw_field = fbWidth / 64u;
    APPEND_AD((fbw_field << 9), 0, GS_REG_FRAME_1);
    APPEND_AD(0, 0, GS_REG_XYOFFSET_1);
    APPEND_AD((uint32_t)PRIM_TYPE_TRIANGLE | PRIM_IIP_MASK, 0, GS_REG_PRIM);

    static const uint32_t vcolor[3] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u };
    static const int32_t vpos[3][2] = { { 40, 60 }, { 220, 60 }, { 40, 200 } };
    for (int i = 0; i < n_verts; i++) {
        uint32_t rgba = vcolor[i];
        uint32_t rgbaq_lo = (rgba & 0xFFu) | (((rgba >> 8) & 0xFFu) << 8) |
                            (((rgba >> 16) & 0xFFu) << 16) | (((rgba >> 24) & 0xFFu) << 24);
        APPEND_AD(rgbaq_lo, 0, GS_REG_RGBAQ);
        APPEND_AD((uint32_t)(vpos[i][0] << 4), (uint32_t)(vpos[i][1] << 4), GS_REG_XYZ2);
    }
#undef APPEND_AD
#undef WLE32

    CHECK(off == (int)sizeof(pkt), "packet builder fills the buffer exactly (no size mismatch, no overflow)");

    const uint32_t pkt_ram_addr = 0x00100000u;
    memcpy(fake_ram + pkt_ram_addr, pkt, (size_t)off);

    dma_state_t *dma = dma_get_state();
    dma->chan[DMA_CHANNEL_GIF].chcr = 0;
    dma->chan[DMA_CHANNEL_GIF].madr = pkt_ram_addr;
    dma->chan[DMA_CHANNEL_GIF].qwc  = (uint32_t)(off / 16);
    dma_channel_kick(DMA_CHANNEL_GIF);

    CHECK(dma->chan[DMA_CHANNEL_GIF].last_error == DMA_ERR_NONE, "DMA kick reports no error");
    CHECK(dma->chan[DMA_CHANNEL_GIF].qwc == 0, "NORMAL-mode DMA consumed all QWC (0 remaining)");
    CHECK(dma->chan[DMA_CHANNEL_GIF].madr == pkt_ram_addr + (uint32_t)off,
          "MADR advanced by the full packet length after the transfer");

    gif_state_t *st = gif_get_state();
    CHECK(st->triangles_drawn == 1, "exactly one triangle was drawn by the real DMA->GIF pipeline");
    CHECK((st->prim & 0x7u) == PRIM_TYPE_TRIANGLE, "PRIM decoded as TRIANGLE");
    CHECK((st->prim & PRIM_IIP_MASK) != 0, "PRIM's IIP bit is set (Gouraud requested)");

    /* Same barycentric sample points as tests/test_gif_gouraud.c,
     * translated for this triangle's actual vertex coordinates
     * (40,60)-(220,60)-(40,200): near vertex0 -> red, near vertex1 ->
     * green, near vertex2 -> blue, centroid -> even blend. */
    uint32_t near_v0 = gs_mem_read_psmct32(0, fbWidth, 45, 65);
    CHECK(chan_byte(near_v0, 0) > 200 && chan_byte(near_v0, 8) < 60 && chan_byte(near_v0, 16) < 60,
          "sample near vertex0 (40,60) is dominated by red");

    uint32_t near_v1 = gs_mem_read_psmct32(0, fbWidth, 210, 65);
    CHECK(chan_byte(near_v1, 8) > 200 && chan_byte(near_v1, 0) < 60 && chan_byte(near_v1, 16) < 60,
          "sample near vertex1 (220,60) is dominated by green");

    uint32_t near_v2 = gs_mem_read_psmct32(0, fbWidth, 45, 195);
    CHECK(chan_byte(near_v2, 16) > 200 && chan_byte(near_v2, 0) < 60 && chan_byte(near_v2, 8) < 60,
          "sample near vertex2 (40,200) is dominated by blue");

    uint32_t centroid = gs_mem_read_psmct32(0, fbWidth, 100, 106); /* ~(40+220+40)/3, (60+60+200)/3 */
    CHECK(chan_byte(centroid, 0) > 60 && chan_byte(centroid, 0) < 130 &&
          chan_byte(centroid, 8) > 60 && chan_byte(centroid, 8) < 130 &&
          chan_byte(centroid, 16) > 60 && chan_byte(centroid, 16) < 130,
          "centroid is a blend of all 3 vertex colors, not any single pure color");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
