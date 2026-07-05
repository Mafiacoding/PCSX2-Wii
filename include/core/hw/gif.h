#ifndef PCSX2WII_GIF_H
#define PCSX2WII_GIF_H

#include <stdint.h>
#include "core/hw/dma.h"

/*
 * GIF (Graphics Interface) packet parser - the first component that
 * actually consumes DMA-delivered data and turns it into GS memory
 * writes, rather than just moving bytes around. This is what makes
 * the DMA chain engine (source/hw/dma.c) and GS memory model
 * (source/hw/gs_mem.c) actually connect to each other.
 *
 * Scope (deliberately limited - see docs/ROADMAP.md):
 *   - PACKED transfer mode only (FLG=0). REGLIST and IMAGE modes are
 *     not implemented.
 *   - Registers handled: PRIM, RGBAQ, XYZ2, and A+D (address+data)
 *     writes to PRIM/RGBAQ/XYZ2/FRAME_1/XYOFFSET_1. Everything else
 *     (textures, alpha blending, Z-buffer, CLAMP/TEST/scissor beyond
 *     basic XY offset, contexts 2, ...) is ignored.
 *   - Primitive types: SPRITE (a filled, untextured, flat-color
 *     axis-aligned rectangle from 2 vertices) and, as of this GS
 *     round, TRIANGLE/TRIANGLE_STRIP/TRIANGLE_FAN (types 3/4/5) - also
 *     flat-shaded (single color per triangle, taken from whichever
 *     RGBAQ was active when the triangle's last vertex arrived - real
 *     per-vertex Gouraud shading is NOT modeled, a deliberate
 *     simplification kept honest here rather than silently guessed).
 *     POINT/LINE still just update vertex/PRIM state without drawing.
 *   - Register field bit positions (GIFTag, PRIM, RGBAQ PACKED
 *     layout, XYZ2 PACKED layout, FRAME, XYOFFSET) are cross-checked
 *     against PCSX2's GS/GSRegs.h and Gif.h, not guessed. The triangle
 *     rasterizer itself (edge-function-based scanline fill) is plain,
 *     well-known 2D geometry, not real-hardware-specific behavior, so
 *     it doesn't need the same kind of external verification the
 *     register layouts do.
 *
 * This is genuinely enough to draw simple flat-colored rectangles and
 * flat-shaded triangles (a BIOS/OSD background bar or a simple 3D
 * wireframe-filled shape, for instance) if real EE code drives it
 * through the DMA chain - it is NOT anywhere near real GS
 * rasterization (no textures, no blending, no Z test, no per-vertex
 * color/perspective).
 */

void gif_init(void);

/* Matches dma_sink_fn - register with dma_set_sink(DMA_CHANNEL_GIF, gif_process_quadwords). */
void gif_process_quadwords(int channel, const uint8_t *data, uint32_t qwc);

/* Exposed for tests: current parser state. */
typedef struct {
    uint32_t prim;         /* current PRIM register value (bits 0-2 = primitive type) */
    uint32_t rgba;         /* current color, packed as 0xAABBGGRR (matches gs_mem convention) */
    uint32_t fbp, fbw;      /* current draw target, in OUR gs_mem bp/bw convention (see gs_mem.h) */
    uint32_t xyoffset_x, xyoffset_y; /* raw 12.4 fixed-point offset (real units) */

    int has_vertex0;
    int32_t v0x, v0y;

    /* Triangle vertex accumulation (TRIANGLE/TRIANGLE_STRIP/
     * TRIANGLE_FAN, prim types 3/4/5). tri_vseq counts vertices
     * received since the primitive type last changed (reset on every
     * PRIM write, matching real hardware starting a fresh vertex
     * sequence). tri_x/tri_y[0..2] is a rolling buffer of the most
     * recent up to 3 vertices - for TRIANGLE_FAN, slot 0 is instead a
     * fixed anchor (never overwritten once set) rather than rolling. */
    int tri_vseq;
    int32_t tri_x[3], tri_y[3];

    uint64_t quadwords_seen;
    uint64_t sprites_drawn;
    uint64_t triangles_drawn;
    uint64_t unsupported_prims_seen;
} gif_state_t;

gif_state_t *gif_get_state(void);

#endif
