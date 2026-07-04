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

/* Appends one 16-byte A+D "loop" qword: DATA in words 0-1, ADDR in
 * word 2's low byte (matches gif.c's apply_ad_write expectations). */
static void append_ad(uint8_t *buf, int *off, uint32_t data_lo, uint32_t data_hi, uint32_t addr)
{
    wle32(buf + *off, data_lo);
    wle32(buf + *off + 4, data_hi);
    wle32(buf + *off + 8, addr);
    wle32(buf + *off + 12, 0);
    *off += 16;
}

int main(void) {
    gs_mem_init();
    gif_init();

    uint8_t buf[16 * 8];
    memset(buf, 0, sizeof(buf));
    int off = 0;

    const int NLOOP = 6;
    /* GIFtag: NLOOP=6, EOP=1, PRE=0, FLG=0 (PACKED), NREG=1, REGS
     * nibble0 = 0xE (A+D) - every one of the 6 loops is a single A+D
     * register write. */
    wle32(buf + off,     (uint32_t)NLOOP | (1u << 15));
    wle32(buf + off + 4, (0u << 26) | (1u << 28));
    wle32(buf + off + 8, GIF_REG_AD);
    wle32(buf + off + 12, 0);
    off += 16;

    /* 1: FRAME_1 -> fbp=0, fbw field=10 (=640px) */
    append_ad(buf, &off, /*data_lo=*/(10u << 9), /*data_hi=*/0, GS_REG_FRAME_1);
    /* 2: XYOFFSET_1 -> (0,0) */
    append_ad(buf, &off, 0, 0, GS_REG_XYOFFSET_1);
    /* 3: PRIM -> type 6 (SPRITE) */
    append_ad(buf, &off, PRIM_TYPE_SPRITE, 0, GS_REG_PRIM);
    /* 4: RGBAQ -> opaque green (R=0,G=255,B=0,A=255) */
    append_ad(buf, &off, 0u | (255u << 8) | (0u << 16) | (255u << 24), 0, GS_REG_RGBAQ);
    /* 5: XYZ2 vertex0 = pixel (10,10) -> raw = pixel<<4 (12.4 fixed point) */
    append_ad(buf, &off, (uint32_t)(10 << 4), (uint32_t)(10 << 4), GS_REG_XYZ2);
    /* 6: XYZ2 vertex1 = pixel (30,25) -> sprite kicks here */
    append_ad(buf, &off, (uint32_t)(30 << 4), (uint32_t)(25 << 4), GS_REG_XYZ2);

    CHECK(off == 16 + NLOOP * 16, "test packet built to the expected size (tag + 6 loop qwords)");

    gif_process_quadwords(DMA_CHANNEL_GIF, buf, (uint32_t)(off / 16));

    gif_state_t *st = gif_get_state();
    CHECK(st->fbp == 0, "FRAME_1 A+D write set fbp=0");
    CHECK(st->fbw == 640, "FRAME_1 A+D write set fbw=640 (field 10 * 64)");
    CHECK((st->prim & 0x7u) == PRIM_TYPE_SPRITE, "PRIM A+D write set primitive type to SPRITE");
    CHECK(st->rgba == 0xFF00FF00u, "RGBAQ A+D write set color to opaque green (0xAABBGGRR = 0xFF00FF00)");
    CHECK(st->sprites_drawn == 1, "exactly one sprite was drawn");
    CHECK(st->has_vertex0 == 0, "vertex state reset after the sprite kicked");

    /* Verify the actual pixels: rectangle spans x in [10,30), y in [10,25) */
    CHECK(gs_mem_read_psmct32(0, 640, 15, 15) == 0xFF00FF00u, "pixel inside the sprite rect is green");
    CHECK(gs_mem_read_psmct32(0, 640, 29, 24) == 0xFF00FF00u, "pixel at the inner edge of the rect is green");
    CHECK(gs_mem_read_psmct32(0, 640, 9, 15) == 0, "pixel just left of the rect is untouched");
    CHECK(gs_mem_read_psmct32(0, 640, 30, 15) == 0, "pixel just right of the rect (exclusive bound) is untouched");
    CHECK(gs_mem_read_psmct32(0, 640, 15, 9) == 0, "pixel just above the rect is untouched");
    CHECK(gs_mem_read_psmct32(0, 640, 15, 25) == 0, "pixel just below the rect (exclusive bound) is untouched");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
