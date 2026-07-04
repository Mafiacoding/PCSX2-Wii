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
