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
 *     XYOFFSET_1/ZBUF_1/TEST_1 (ZBUF_1/TEST_1 added task #89 - real
 *     Z-buffer storage + depth test for triangles/SPRITE, see
 *     rasterize_triangle()/rasterize_sprite()). Everything else
 *     (alpha blending/test, CLAMP/scissor beyond basic XY offset,
 *     contexts 2, CLUT/paletted textures, mipmaps, ...) is ignored.
 *   - Primitive types: SPRITE (a filled, flat-color or - as of task
 *     #88 - texture-mapped axis-aligned rectangle from 2 vertices,
 *     always flat-shaded like real hardware; see gif.c's
 *     rasterize_sprite() for its simplified, explicitly-noted texture-
 *     coordinate approximation, distinct from triangles' full
 *     per-pixel perspective correction) and TRIANGLE/TRIANGLE_STRIP/
 *     TRIANGLE_FAN (types 3/4/5), which respect PRIM's real IIP bit
 *     (bit 3, flat vs. Gouraud shading), TME bit (bit 4, flat/Gouraud
 *     color vs. texture-mapped), and - as of task #88 - FST bit (bit
 *     8: 1 = UV fixed-point texel coordinates, 0 = ST+Q floating-point
 *     perspective-correct coordinates) - see gif.c's
 *     rasterize_triangle(). Color interpolation (Gouraud) always uses
 *     plain affine (screen-space) barycentric weighting. Texture-
 *     coordinate interpolation (when TME=1) uses plain affine
 *     interpolation when FST=1 (UV mode, matching real hardware) but
 *     genuine perspective-correct (1/Q) interpolation when FST=0
 *     (ST+Q mode, added this round - see rasterize_triangle()'s S/Q,
 *     T/Q, 1/Q barycentric interpolation, matching the standard
 *     perspective-texture-mapping algorithm real GS hardware uses).
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
/* Z-buffer/depth-test registers (task #89) - cross-checked
 * against a live fetch of PCSX2's GS/GSRegs.h GIF_A_D_REG enum. */
#define GS_REG_TEST_1     0x47
#define GS_REG_ZBUF_1     0x4E

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
/* PRIM bit 8 (FST): 1 = UV fixed-point texel coordinates (this
 * project's original, still-supported mode), 0 = ST+Q floating-point
 * perspective-correct coordinates (task #88) - cross-checked against
 * PCSX2's own GS/GSRegs.h GIFRegPRIM bitfield (PRIM:3,IIP:1,TME:1,
 * FGE:1,ABE:1,AA1:1,FST:1,... - FST is the 7th field, bit offset 8). */
#define PRIM_FST_MASK 0x100u

/* GIFRegTEST bitfield (task #89) - cross-checked against PCSX2's
 * GS/GSRegs.h GIFRegTEST: ATE:1,ATST:3,AREF:8,AFAIL:2,DATE:1,DATM:1,
 * ZTE:1,ZTST:2 (all in word0; word1 unused). Only ZTE (bit 16) and
 * ZTST (bits 17-18) are modeled - alpha test (ATE/ATST/AREF/AFAIL)
 * and the destination-alpha bits (DATE/DATM) are not implemented
 * (no alpha blending exists in this project yet - see gif.h's
 * top-of-file scope comment). */
#define TEST_ZTE_MASK   0x10000u
#define TEST_ZTST_SHIFT 17u
#define TEST_ZTST_MASK  0x3u

/* GS_ZTST enum - cross-checked against PCSX2's GS/GSRegs.h GS_ZTST
 * (real, literal hardware semantics: NEVER = every fragment fails,
 * ALWAYS = every fragment passes regardless of Z, GEQUAL/GREATER =
 * compare the new fragment's Z against the value already stored in
 * the Z buffer). */
#define GS_ZTST_NEVER   0u
#define GS_ZTST_ALWAYS  1u
#define GS_ZTST_GEQUAL  2u
#define GS_ZTST_GREATER 3u

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
    /* TEX0's TW/TH fields (log2 texture width/height, real hardware
     * bitfield - TW is 4 bits within word0 alone (bits 26-29); TH is
     * a 4-bit field that straddles the 64-bit register's word
     * boundary (2 bits from word0's top, bits 30-31, plus 2 bits from
     * word1's bottom, bits 0-1) - cross-checked against PCSX2's own
     * GS/GSRegs.h GIFRegTEX0's union of two overlapping bitfield
     * layouts. Needed (added task #88) to scale normalized ST+Q
     * texture coordinates (0.0-1.0 range) into texel space - UV mode
     * (FST=1) doesn't need these, since UV is already in texel units. */
    uint32_t tex_tw, tex_th;

    /* Z-buffer / depth-test state (task #89). zbp is the Z buffer's
     * base pointer, in OUR gs_mem bp convention (see gs_mem.h) - real
     * hardware's ZBUF register (GIFRegZBUF: ZBP:9, PSM:6, ZMSK:1) has
     * NO separate width field, matching this model, which reuses fbw
     * for Z-buffer addressing too. zbuf_configured is this project's
     * OWN safety gate (not a real hardware concept): it stays 0 until
     * an explicit ZBUF_1 A+D write happens, so that pre-existing tests/
     * demos that never configure a Z buffer keep drawing exactly as
     * before (zbp defaults to 0, same as fbp's own default - without
     * this gate, an unconfigured Z buffer would silently alias and
     * corrupt the color buffer). zte/ztst mirror TEST_1's real ZTE/
     * ZTST fields (see TEST_ZTE_MASK/TEST_ZTST_* above). PSM/CLUT-
     * equivalent Z formats (PSMZ32/24/16) are not modeled - Z is
     * always stored as a plain 32-bit word via gs_mem's existing
     * PSMCT32-shaped helpers, matching gs_mem.h's own documented
     * linear-addressing simplification. */
    uint32_t zbp;
    int zmsk;
    int zbuf_configured;
    int zte;
    int ztst;

    /* Current UV register value (real hardware's 12.4 fixed-point
     * texel coordinate "FST=1" mode - see gif.h's scope comment),
     * already converted to integer texels exactly like XYZ2's own
     * >>4 conversion. */
    int32_t cur_u, cur_v;

    /* Current ST+Q state (real hardware's "FST=0" perspective-correct
     * mode, task #88): S/T are real IEEE-754 floats (normalized
     * texture-space coordinates, 0.0-1.0 typically), latched by either
     * an A+D ST write (GS_REG_ST - S,T only, no Q) or a PACKED STQ tag
     * (S,T,Q together - not yet implemented as a distinct PACKED
     * register path, see below). Q is a real float too, latched
     * together with RGBAQ on real hardware (GIFRegRGBAQ's layout:
     * R/G/B/A bytes in word0, Q as a float in word1) - cross-checked
     * against PCSX2's own GS/GSRegs.h. */
    float cur_s, cur_t, cur_q;

    int has_vertex0;
    int32_t v0x, v0y;
    /* SPRITE's first-vertex texture coordinates (task #88 - SPRITE
     * texturing), captured alongside v0x/v0y so rasterize_sprite()
     * can interpolate between the two corners. */
    int32_t v0u, v0v;
    float v0s, v0t, v0q;
    /* SPRITE's first-vertex Z (task #89) - see tri_z's comment
     * below for the same A+D-vs-PACKED scope caveat. */
    uint32_t v0z;

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
    /* Per-vertex ST+Q (task #88) - captured alongside tri_u/tri_v at
     * every vertex kick regardless of FST (harmless when unused);
     * rasterize_triangle() picks UV (affine) or ST+Q (perspective-
     * correct) interpolation based on PRIM's real FST bit. */
    float tri_s[3], tri_t[3], tri_q[3];
    /* Per-vertex Z (task #89), from XYZ2's real Z word. IMPORTANT
     * scope caveat: this project's existing A+D-mode XYZ2 handling
     * (apply_ad_write's GS_REG_XYZ2 case - the ONLY path every
     * existing test/demo in this codebase uses) packs X into the
     * ENTIRE first 32-bit word and Y into the ENTIRE second 32-bit
     * word - a convention already baked into every existing test
     * file before this round, which leaves no room for Z (real
     * hardware's actual GIFRegXYZ A+D layout is X:16,Y:16 packed
     * together in ONE word, Z:32 alone in the other - cross-checked
     * against PCSX2's GS/GSRegs.h - but changing this project's
     * established A+D convention now would require touching every
     * existing test file and main.c's demo, well outside this task's
     * scope). Z therefore only flows through for the genuine PACKED-
     * mode XYZ2 path (GIFPackedXYZ2: X in word0, Y in word1, Z in
     * word2 - a real, correctly-cross-checked, and previously-
     * completely-unused-by-any-test layout), which is what the new
     * tests/test_z_buffer.c uses. A+D-mode XYZ2 draws get Z=0
     * (harmless: Z-buffer reads/writes stay fully gated behind
     * zbuf_configured, so pre-existing A+D-only tests are completely
     * unaffected either way). */
    uint32_t tri_z[3];

    uint64_t quadwords_seen;
    uint64_t sprites_drawn;
    uint64_t triangles_drawn;
    uint64_t unsupported_prims_seen;
    uint64_t pixels_ztest_failed; /* task #89 - counts fragments rejected by the Z test, for test visibility */
} gif_state_t;

gif_state_t *gif_get_state(void);

#endif
