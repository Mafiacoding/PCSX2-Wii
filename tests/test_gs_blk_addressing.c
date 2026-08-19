#include <stdio.h>
#include <string.h>
#include "hw/gs_mem.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

/* Round 640: regression test for the GS VRAM-aliasing bug documented in
 * docs/STATUS.md Round 639/640. Real PS2 GS BITBLTBUF.DBP/SBP and
 * TEX0.TBP0/CBP use "Address/64 words" (256 bytes/unit) real hardware
 * addressing, a 64x finer granularity than FRAME.FBP/ZBUF.ZBP's
 * "Address/2048 words" (8192 bytes/unit). The plain gs_mem_read/write_
 * psmct32() functions (bp*4 bytes/unit) undercount block-scale bp
 * values by exactly this 64x factor, which is why a legitimate texture
 * upload at dbp=13440 (real byte offset 13440*256=3,440,640) was
 * landing at byte offset 13440*4=53,760 - inside a framebuffer's own
 * occupied range - instead of ~3.4MB away where it really belongs. */

int main(void) {
    gs_mem_init();

    /* --- 1. _blk wrapper basic round-trip --- */
    gs_mem_write_psmct32_blk(100, 64, 5, 5, 0xAABBCCDDu);
    CHECK(gs_mem_read_psmct32_blk(100, 64, 5, 5) == 0xAABBCCDDu, "blk pixel roundtrip at bp=100 (5,5)");
    CHECK(gs_mem_read_psmct32_blk(100, 64, 6, 5) == 0, "blk adjacent pixel (6,5) untouched");

    /* --- 2. _blk address math matches the real "Address/64 words"
     * convention: bp unit is 64x finer than the plain functions' bp
     * unit, so gs_mem_write_psmct32_blk(bp, ...) must land at the same
     * byte offset as gs_mem_write_psmct32(bp*64, ...). --- */
    gs_mem_init();
    gs_mem_write_psmct32_blk(50, 32, 0, 0, 0x11112222u);
    CHECK(gs_mem_read_psmct32(50 * 64, 32, 0, 0) == 0x11112222u,
          "blk write at bp=50 lands at same offset as plain write at bp=3200 (50*64)");

    /* --- 3. The exact real-world reproduction: this project's own
     * survey data (Round 639/640) observed a framebuffer at fbp=0 and
     * a real texture-upload dbp=13440, both driven through what used
     * to be the SAME under-scaled bp*4 addressing. Confirm they no
     * longer alias once dbp is routed through the _blk wrapper while
     * fbp stays on the plain function (matching gif.c's actual,
     * differentiated fix). --- */
    gs_mem_init();
    uint32_t fbw = 640;
    /* Framebuffer at fbp=0, write a recognizable pixel. */
    gs_mem_write_psmct32(0, fbw, 100, 50, 0xFF00FF00u);
    /* Texture upload at dbp=13440 (real observed value), write a
     * different recognizable pixel using the fixed _blk path. */
    uint32_t dbw = 64;
    gs_mem_write_psmct32_blk(13440, dbw, 0, 0, 0xDEADBEEFu);
    CHECK(gs_mem_read_psmct32(0, fbw, 100, 50) == 0xFF00FF00u,
          "framebuffer pixel at fbp=0 survives the dbp=13440 texture upload (no more aliasing)");
    CHECK(gs_mem_read_psmct32_blk(13440, dbw, 0, 0) == 0xDEADBEEFu,
          "dbp=13440 texture upload readable back via the same _blk addressing");

    /* --- 4. Bounds safety: a large block-scale bp*64 that would
     * overflow GS_MEM_SIZE must safely no-op (matching the existing
     * plain functions' bounds-check behavior), not read/write OOB. --- */
    gs_mem_init();
    gs_mem_write_psmct32_blk(70000, 64, 0, 0, 0x99999999u); /* 70000*64*4 >> 4MB */
    CHECK(gs_mem_read_psmct32_blk(70000, 64, 0, 0) == 0, "out-of-range blk bp safely no-ops (reads back 0)");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
