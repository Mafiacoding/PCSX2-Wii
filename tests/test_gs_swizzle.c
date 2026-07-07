/* test_gs_swizzle.c - host-native test for Round 25's real PSMCT32
 * page/block-swizzled addressing (gs_mem_swizzle_addr32() and its
 * read/write wrappers), added as a separate, additional API
 * alongside the pre-existing simplified-linear gs_mem functions -
 * see include/core/hw/gs_mem.h's extended comment on
 * gs_mem_swizzle_addr32() for why this isn't a drop-in replacement,
 * and source/hw/gs_mem.c's own comment for the real page/block
 * table and this round's citation-honesty note (live source-fetch
 * research hit a session limit again this round).
 */
#include <stdio.h>
#include <string.h>
#include "core/hw/gs_mem.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void)
{
    { /* Known-value checks: hand-derived from the documented 8x4
       * block-index table and the page/block geometry (page=64x32px,
       * block=8x8px=256 bytes, page=8192 bytes). Single-page buffer
       * (bw=64), bp=0. */
        gs_mem_init();
        uint32_t bw = 64;

        CHECK(gs_mem_swizzle_addr32(0, bw, 0, 0) == 0,
              "swizzle: pixel (0,0) -> block 0, offset 0 (byte 0)");

        /* (8,0): block_x=1,block_y=0 -> table[0][1]=1 -> block byte
         * offset 1*256=256, within-block offset 0 (top-left of block). */
        CHECK(gs_mem_swizzle_addr32(0, bw, 8, 0) == 256u,
              "swizzle: pixel (8,0) -> block 1 (table[0][1]), byte offset 256");

        /* (0,8): block_x=0,block_y=1 -> table[1][0]=2 -> byte offset 512. */
        CHECK(gs_mem_swizzle_addr32(0, bw, 0, 8) == 512u,
              "swizzle: pixel (0,8) -> block 2 (table[1][0]), byte offset 512");

        /* (63,31): bottom-right pixel of the page. block_x=7,block_y=3
         * -> table[3][7]=31 -> block byte offset 31*256=7936; within-
         * block px=7,py=7 -> (7*8+7)*4=252; total 8188 (last dword of
         * the 8192-byte page). */
        CHECK(gs_mem_swizzle_addr32(0, bw, 63, 31) == 8188u,
              "swizzle: pixel (63,31) -> last dword of the page (byte 8188)");
    }

    { /* bp selects a whole page (8192 bytes) - proven by an exact
       * offset delta between bp=0 and bp=1 at the same (x,y). */
        gs_mem_init();
        uint32_t bw = 64;
        uint32_t a0 = gs_mem_swizzle_addr32(0, bw, 5, 5);
        uint32_t a1 = gs_mem_swizzle_addr32(1, bw, 5, 5);
        CHECK(a1 == a0 + 8192u, "swizzle: bp is a real page unit - bp=1 is exactly +8192 bytes from bp=0 at the same (x,y)");
    }

    { /* A 2-page-wide buffer (bw=128): x>=64 must land in a SECOND
       * page (page_x=1), which is a separate 8192-byte region from
       * page_x=0 at the same y, proving pages_per_row is honored. */
        gs_mem_init();
        uint32_t bw = 128;
        uint32_t a_page0 = gs_mem_swizzle_addr32(0, bw, 0, 0);
        uint32_t a_page1 = gs_mem_swizzle_addr32(0, bw, 64, 0);
        CHECK(a_page1 == a_page0 + 8192u, "swizzle: a 2-page-wide buffer's second page (x=64) starts exactly 8192 bytes after the first");

        /* A pixel in row y=32 (page_y=1) of a 2-page-wide buffer must
         * land in the THIRD page slot (page_index = 1*2+0 = 2). */
        uint32_t a_row1 = gs_mem_swizzle_addr32(0, bw, 0, 32);
        CHECK(a_row1 == a_page0 + 2u * 8192u, "swizzle: row y=32 in a 2-page-wide buffer starts at page index 2 (page_y=1 * pages_per_row=2)");
    }

    { /* No-collision property: every (x,y) pair within one full page
       * must map to a UNIQUE byte offset (a real permutation, not an
       * accidental many-to-one mapping - the strongest evidence the
       * block table + within-block math is actually internally
       * consistent, independent of whether the exact real-hardware
       * table was reproduced correctly). */
        gs_mem_init();
        uint32_t bw = 64;
        static uint8_t seen[GS_MEM_SIZE / 4];
        memset(seen, 0, sizeof(seen));
        int collision = 0;
        for (uint32_t y = 0; y < 32 && !collision; y++) {
            for (uint32_t x = 0; x < 64; x++) {
                uint32_t off = gs_mem_swizzle_addr32(0, bw, x, y);
                uint32_t word_idx = off / 4;
                if (seen[word_idx]) { collision = 1; break; }
                seen[word_idx] = 1;
            }
        }
        CHECK(!collision, "swizzle: all 2048 pixels of a full page map to distinct byte offsets (no aliasing)");
    }

    { /* Round-trip write/read through the real swizzle functions -
       * proves gs_mem_write_psmct32_swizzled/gs_mem_read_psmct32_swizzled
       * agree with each other (the property the GS pipeline would
       * actually rely on if/when it's switched over). */
        gs_mem_init();
        uint32_t bw = 64;
        int ok = 1;
        for (uint32_t y = 0; y < 32 && ok; y++) {
            for (uint32_t x = 0; x < 64; x++) {
                uint32_t color = 0xFF000000u | (y << 8) | x;
                gs_mem_write_psmct32_swizzled(3, bw, x, y, color);
            }
        }
        for (uint32_t y = 0; y < 32 && ok; y++) {
            for (uint32_t x = 0; x < 64; x++) {
                uint32_t expect = 0xFF000000u | (y << 8) | x;
                uint32_t got = gs_mem_read_psmct32_swizzled(3, bw, x, y);
                if (got != expect) { ok = 0; break; }
            }
        }
        CHECK(ok, "swizzle: full-page write/read round-trip via the real swizzle functions matches exactly for all 2048 pixels");
    }

    { /* Independence from the pre-existing simplified-linear API:
       * writing through the SWIZZLED function and reading through the
       * pre-existing LINEAR function at the same (bp,bw,x,y) should
       * NOT generally agree (they use different addressing schemes) -
       * this is a sanity check that the new function is genuinely
       * doing something different, not accidentally aliased to the
       * old one. (0,0) is a degenerate case where both schemes agree
       * (both map to the region's very first byte) - checked via
       * (5,3) instead, which differs under the two schemes. */
        gs_mem_init();
        uint32_t bw = 64;
        gs_mem_write_psmct32_swizzled(0, bw, 5, 3, 0xAABBCCDDu);
        uint32_t via_linear = gs_mem_read_psmct32(0, bw, 5, 3);
        CHECK(via_linear != 0xAABBCCDDu,
              "swizzle: a swizzled write at (5,3) does NOT show up at the same (bp,bw,x,y) under the pre-existing linear API - genuinely different addressing, not aliased");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
