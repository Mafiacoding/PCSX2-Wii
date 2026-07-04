#ifndef PCSX2WII_GS_MEM_H
#define PCSX2WII_GS_MEM_H

#include <stdint.h>

/*
 * GS local memory (the real hardware's 4MB eDRAM) - simplified model.
 *
 * IMPORTANT SIMPLIFICATION: real GS memory uses block/column-swizzled
 * addressing (see PCSX2's GS/GSLocalMemory.h - GSLocalMemory::swizzle32
 * and friends) so that the rasterizer's 2D access patterns map to
 * cache-friendly runs in the actual 1D eDRAM. This model uses plain
 * LINEAR addressing instead: pixel (x,y) at a given base pointer (bp)
 * and buffer width (bw) maps straightforwardly to
 * bp*256 + y*bw*64*4 + x*4 (word-aligned, PSMCT32 only). This is
 * good enough to get pixels into a scratch buffer and out to a
 * display for a first "something shows up" milestone, but it is NOT
 * bit-compatible with real GS memory layout - anything that reads GS
 * memory expecting the real swizzle pattern (texture sampling with
 * wraparound, some blit tricks games rely on) will not work correctly
 * against this model. Real swizzle addressing is future work - see
 * docs/ROADMAP.md section 6.
 *
 * Only PSMCT32 (32-bit RGBA8888-ish, the format an ARGB/opaque splash
 * screen would most plausibly use) is modeled. PSMCT24/16, Z-buffer
 * formats, and CLUT/paletted formats are not implemented.
 */

#define GS_MEM_SIZE (4 * 1024 * 1024)

void gs_mem_init(void);
uint8_t *gs_mem_get(void); /* raw 4MB buffer, for tests/inspection */

/* bp: base pointer, bw: buffer width in pixels, x/y: pixel
 * coordinates. NOTE: real GS registers (FBP/FBW) encode these in
 * hardware-specific units (blocks-of-32, pixels/64, etc. - see
 * GS/GSRegs.h Block()/FBW fields) - converting real register values
 * into plain word offsets and pixel counts before calling these
 * functions is the caller's job, not handled here. */
uint32_t gs_mem_read_psmct32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);
void     gs_mem_write_psmct32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t rgba);

#endif
