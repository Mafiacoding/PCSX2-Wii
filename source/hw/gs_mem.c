/*
 * gs_mem.c - simplified linear GS local memory model. See
 * include/core/hw/gs_mem.h for the important simplification notes
 * (real hardware uses swizzled block addressing; this doesn't).
 */

#include "core/hw/gs_mem.h"
#include <string.h>

static uint8_t g_gs_mem[GS_MEM_SIZE];

void gs_mem_init(void) { memset(g_gs_mem, 0, sizeof(g_gs_mem)); }
uint8_t *gs_mem_get(void) { return g_gs_mem; }

static inline uint32_t pixel_offset(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    /* Our own simplified linear convention: bp is a word offset into
     * GS memory, bw is the scanline width in pixels (PSMCT32 = 4
     * bytes/pixel). Deliberately not the real hardware's block/FBW
     * encoding - see header. */
    return bp * 4u + (y * bw + x) * 4u;
}

uint32_t gs_mem_read_psmct32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t off = pixel_offset(bp, bw, x, y);
    if (off + 4 > GS_MEM_SIZE) return 0;
    uint32_t v;
    memcpy(&v, g_gs_mem + off, 4); /* host-native order is fine: this
                                     * buffer is purely internal, never
                                     * read/written as raw guest bytes
                                     * the way EE/IOP RAM is. */
    return v;
}

void gs_mem_write_psmct32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t rgba)
{
    uint32_t off = pixel_offset(bp, bw, x, y);
    if (off + 4 > GS_MEM_SIZE) return;
    memcpy(g_gs_mem + off, &rgba, 4);
}

/* Round 25: real PSMCT32 page/block-swizzled addressing - see
 * gs_mem.h's extended comment above these declarations for the full
 * scope (real at page/block granularity, simplified within-block).
 *
 * PAGE_W/PAGE_H: a PSMCT32 "page" is 64x32 pixels (8192 bytes = one
 * real BP unit). BLOCK_W/BLOCK_H: each page is divided into 32
 * blocks of 8x8 pixels (256 bytes each). The block grid below (8
 * blocks wide x 4 blocks tall = 32) is the real, well-established
 * PSMCT32 block-swizzle order - each entry is that (bx,by) grid
 * cell's real block INDEX within the page (0-31), not a claim of
 * fresh primary-source verification this round (this round's live
 * research pass hit a session limit - see docs/STATUS.md's "GS Round
 * 25" section for the full citation-honesty note; this specific
 * table is nonetheless very widely published/re-derived across PS2
 * homebrew/texture tooling, giving it higher confidence than, say, a
 * from-scratch guess). */
#define GS_SWZ_PAGE_W 64u
#define GS_SWZ_PAGE_H 32u
#define GS_SWZ_BLOCK_W 8u
#define GS_SWZ_BLOCK_H 8u
#define GS_SWZ_BLOCKS_PER_ROW 8u  /* PAGE_W / BLOCK_W */
#define GS_SWZ_BLOCKS_PER_COL 4u  /* PAGE_H / BLOCK_H */
#define GS_SWZ_BLOCK_BYTES (GS_SWZ_BLOCK_W * GS_SWZ_BLOCK_H * 4u) /* 256 */
#define GS_SWZ_PAGE_BYTES (GS_SWZ_BLOCK_BYTES * GS_SWZ_BLOCKS_PER_ROW * GS_SWZ_BLOCKS_PER_COL) /* 8192 */

static const uint8_t gs_psmct32_block_table[GS_SWZ_BLOCKS_PER_COL][GS_SWZ_BLOCKS_PER_ROW] = {
    {  0,  1,  4,  5, 16, 17, 20, 21 },
    {  2,  3,  6,  7, 18, 19, 22, 23 },
    {  8,  9, 12, 13, 24, 25, 28, 29 },
    { 10, 11, 14, 15, 26, 27, 30, 31 },
};

uint32_t gs_mem_swizzle_addr32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    /* bw here is already in pixels (this project's established
     * convention - see FRAME_1/TEX0_1's *64u expansion in gif.c),
     * and real hardware requires it to be a multiple of the page
     * width (64px) - guarded the same way FBW/TBW already are
     * elsewhere in this codebase. */
    uint32_t pages_per_row = bw / GS_SWZ_PAGE_W;
    if (pages_per_row == 0) pages_per_row = 1;

    uint32_t page_x = x / GS_SWZ_PAGE_W;
    uint32_t page_y = y / GS_SWZ_PAGE_H;
    uint32_t lx = x % GS_SWZ_PAGE_W;
    uint32_t ly = y % GS_SWZ_PAGE_H;

    uint32_t block_x = lx / GS_SWZ_BLOCK_W;
    uint32_t block_y = ly / GS_SWZ_BLOCK_H;
    uint32_t block_index = gs_psmct32_block_table[block_y][block_x];

    /* Simplified (documented gap - see header comment): row-major
     * within the 8x8 block, not real hardware's finer column
     * interleave. */
    uint32_t px = lx % GS_SWZ_BLOCK_W;
    uint32_t py = ly % GS_SWZ_BLOCK_H;
    uint32_t within_block_offset = (py * GS_SWZ_BLOCK_W + px) * 4u;

    uint32_t page_index_in_buffer = page_y * pages_per_row + page_x;
    uint32_t base = bp * GS_SWZ_PAGE_BYTES + page_index_in_buffer * GS_SWZ_PAGE_BYTES;
    return base + block_index * GS_SWZ_BLOCK_BYTES + within_block_offset;
}

uint32_t gs_mem_read_psmct32_swizzled(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t off = gs_mem_swizzle_addr32(bp, bw, x, y);
    if (off + 4 > GS_MEM_SIZE) return 0;
    uint32_t v;
    memcpy(&v, g_gs_mem + off, 4);
    return v;
}

void gs_mem_write_psmct32_swizzled(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t rgba)
{
    uint32_t off = gs_mem_swizzle_addr32(bp, bw, x, y);
    if (off + 4 > GS_MEM_SIZE) return;
    memcpy(g_gs_mem + off, &rgba, 4);
}
