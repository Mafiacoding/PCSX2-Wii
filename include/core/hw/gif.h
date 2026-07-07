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
 *   - Registers handled: PRIM, RGBAQ, XYZ2, UV, TEX0_1, and A+D
 *     (address+data) writes to PRIM/RGBAQ/XYZ2/UV/TEX0_1/FRAME_1/
 *     XYOFFSET_1. Everything else (alpha blending, Z-buffer, CLAMP/
 *     TEST/scissor beyond basic XY offset, contexts 2, CLUT/paletted
 *     textures, mipmaps, ...) is ignored.
 *   - Primitive types: SPRITE (a filled, untextured, flat-color
 *     axis-aligned rectangle from 2 vertices, always flat-shaded like
 *     real hardware - texturing is NOT modeled for SPRITE, only for
 *     triangles) and TRIANGLE/TRIANGLE_STRIP/TRIANGLE_FAN (types
 *     3/4/5), which respect PRIM's real IIP bit (bit 3, flat vs.
 *     Gouraud shading) and TME bit (bit 4, flat/Gouraud color vs.
 *     texture-mapped) - see gif.c's rasterize_triangle(). Color
 *     interpolation (Gouraud) and texture-coordinate interpolation
 *     (when TME=1) both use plain affine (screen-space) barycentric
 *     weighting, NOT the real GS's perspective-corrected (1/Q)
 *     interpolation - an honest, noted simplification, not a silently
 *     guessed one. Texture coordinates come from the UV register only
 *     (real hardware's "FST=1" mode) - the ST+Q floating-point/
 *     perspective-correct coordinate path (FST=0) is NOT supported.
 *     Texture sampling is nearest-neighbor from GS memory (PSMCT32
 *     only, matching gs_mem's existing format limitation) with no
 *     CLAMP/wrap modeling (negative interpolated coordinates are
 *     clamped to 0 as a defensive simplification, not real repeat/
 *     clamp semantics). TFX DECAL replaces color with the texture
 *     sample; MODULATE (and, simplified, HIGHLIGHT/HIGHLIGHT2 too)
 *     blends texture and shaded color as (tex*color)/128 per channel,
 *     the standard GS modulate formula. POINT/LINE still just update
 *     vertex/PRIM state without drawing.
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

/* GIF register codes (PACKED mode descriptor nibbles / A+D addresses)
 * and PRIM primitive-type/IIP values - cross-checked against PCSX2's
 * GS/GSRegs.h GIF_A_D_REG enum and GIFRegPRIM bitfield layout. Public
 * (not just gif.c-internal) so any real GIF-packet producer - e.g.
 * main.c's demo, or a future VIF passthrough - can build a well-formed
 * packet without duplicating these constants. */
#define GIF_REG_PRIM      0x00
#define GIF_REG_RGBAQ     0x01
#define GIF_REG_XYZ2      0x05
#define GIF_REG_AD        0x0E
#define GIF_REG_NOP       0x0F

#define GS_REG_PRIM       0x00
#define GS_REG_RGBAQ      0x01
#define GS_REG_ST         0x02
#define GS_REG_UV         0x03
#define GS_REG_XYZ2       0x05
#define GS_REG_TEX0_1     0x06
#define GS_REG_FRAME_1    0x4C
#define GS_REG_XYOFFSET_1 0x18

#define PRIM_TYPE_TRIANGLE       3
#define PRIM_TYPE_TRIANGLE_STRIP 4
#define PRIM_TYPE_TRIANGLE_FAN   5
#define PRIM_TYPE_SPRITE         6

/* PRIM bit 3 (IIP): 0 = flat shading, 1 = Gouraud (per-vertex color
 * interpolation). PRIM bit 4 (TME): 0 = untextured (flat/Gouraud
 * color only), 1 = texture-mapped - see gif.c's rasterize_triangle().
 * Both cross-checked against PCSX2's own GS/GSRegs.h GIFRegPRIM
 * bitfield layout. */
#define PRIM_IIP_MASK 0x8u
#define PRIM_TME_MASK 0x10u

/* TEX0's TFX field (2 bits) - cross-checked against PCSX2's own
 * GS/GSRegs.h GS_TFX enum. HIGHLIGHT/HIGHLIGHT2 are simplified to
 * behave like MODULATE (an honest, noted simplification - real
 * hardware's highlight modes involve a second, specular-like term
 * this project does not model). */
#define TEX_TFX_MODULATE  0u
#define TEX_TFX_DECAL     1u

void gif_init(void);

/* Matches dma_sink_fn - register with dma_set_sink(DMA_CHANNEL_GIF, gif_process_quadwords). */
void gif_process_quadwords(int channel, const uint8_t *data, uint32_t qwc);

/* Exposed for tests: current parser state. */
typedef struct {
    uint32_t prim;         /* current PRIM register value (bits 0-2 = primitive type) */
    uint32_t rgba;         /* current color, packed as 0xAABBGGRR (matches gs_mem convention) */
    uint32_t fbp, fbw;      /* current draw target, in OUR gs_mem bp/bw convention (see gs_mem.h) */
    uint32_t xyoffset_x, xyoffset_y; /* raw 12.4 fixed-point offset (real units) */

    /* TEX0_1 (current texture source) - TBP0/TBW decoded into OUR
     * gs_mem bp/bw convention exactly like FRAME_1's FBP/FBW (see
     * gs_mem.h - this project uses simplified linear addressing, not
     * real hardware's block-swizzled layout, for textures too). tfx
     * is TEX0's 2-bit TFX field (see TEX_TFX_* above). PSM/TW/TH/CLUT
     * fields are ignored (PSMCT32 always assumed, matching gs_mem). */
    uint32_t tex_tbp0, tex_tbw;
    uint32_t tex_tfx;

    /* Current UV register value (real hardware's 12.4 fixed-point
     * texel coordinate "FST=1" mode - see gif.h's scope comment),
     * already converted to integer texels exactly like XYZ2's own
     * >>4 conversion. */
    int32_t cur_u, cur_v;

    int has_vertex0;
    int32_t v0x, v0y;

    /* Triangle vertex accumulation (TRIANGLE/TRIANGLE_STRIP/
     * TRIANGLE_FAN, prim types 3/4/5). tri_vseq counts vertices
     * received since the primitive type last changed (reset on every
     * PRIM write, matching real hardware starting a fresh vertex
     * sequence). tri_x/tri_y/tri_rgba/tri_u/tri_v[0..2] is a rolling
     * buffer of the most recent up to 3 vertices (position + the
     * RGBAQ color + the UV texture coordinate active when that vertex
     * was kicked) - for TRIANGLE_FAN, slot 0 is instead a fixed anchor
     * (never overwritten once set) rather than rolling. tri_rgba
     * enables real per-vertex Gouraud shading (PRIM register bit 3,
     * IIP); tri_u/tri_v enable real per-vertex texture-coordinate
     * interpolation (PRIM register bit 4, TME) - both confirmed
     * against PCSX2's own GS/GSRegs.h GIFRegPRIM layout. */
    int tri_vseq;
    int32_t tri_x[3], tri_y[3];
    uint32_t tri_rgba[3];
    int32_t tri_u[3], tri_v[3];

    uint64_t quadwords_seen;
    uint64_t sprites_drawn;
    uint64_t triangles_drawn;
    uint64_t unsupported_prims_seen;
} gif_state_t;

gif_state_t *gif_get_state(void);

#endif
