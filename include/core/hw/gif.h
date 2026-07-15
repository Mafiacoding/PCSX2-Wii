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
 *     the standard GS modulate formula. POINT (type 0) and LINE/
 *     LINE_STRIP (types 1/2, task: "GS coverage breadth") are now
 *     implemented too - see gif.c's rasterize_point()/
 *     rasterize_line(). POINT is a single flat-color, non-
 *     interpolated pixel write (real hardware: no interpolation of
 *     any kind for a single vertex). LINE/LINE_STRIP support real
 *     IIP-bit Gouraud shading (flat shading uses the LAST vertex's
 *     color, same convention as triangles/sprites - cross-checked
 *     against PCSX2's `GSDrawScanline::CSetupPrim`, which selects
 *     `last=1` for `GS_LINE_CLASS`) and real linear (not
 *     perspective-corrected - Z has already been through the
 *     perspective projection by the time it reaches the rasterizer,
 *     same as triangles) Z interpolation along the segment, ported
 *     from PCSX2's `GSRasterizer::DrawEdgeLine` DDA algorithm (walk
 *     the major axis one pixel at a time, step every interpolated
 *     attribute - color, Z - linearly per major-axis pixel). No
 *     texture mapping (real GS hardware does not texture-map POINT/
 *     LINE - only SPRITE/TRIANGLE support TME).
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
/* Round 97 (138th finding, task #254 - vertex-register parity):
 * XYZF2 (X/Y/Z + Fog coefficient, WITH drawing kick), XYZF3/XYZ3
 * (vertex-queue-advance-only variants, no drawing kick), and FOG (a
 * standalone "current fog value" register, same "latched, then read
 * at vertex kick" pattern as ST/UV - see gif.h's cur_fog field
 * comment below). Real PACKED-mode tag nibbles, cross-checked against
 * the official Sony GS Users Manual's "7.3 Register List in Address
 * Order" (0x04=XYZF2, 0x0a=FOG, 0x0c=XYZF3, 0x0d=XYZ3) - the same
 * legitimately public, citable source as Round 96's SCISSOR_1/2. */
#define GIF_REG_XYZF2     0x04
#define GIF_REG_FOG       0x0A
#define GIF_REG_XYZF3     0x0C
#define GIF_REG_XYZ3      0x0D

#define GS_REG_PRIM       0x00
#define GS_REG_RGBAQ      0x01
#define GS_REG_ST         0x02
#define GS_REG_UV         0x03
#define GS_REG_XYZ2       0x05
#define GS_REG_XYZF2      0x04
#define GS_REG_XYZF3      0x0C
#define GS_REG_XYZ3       0x0D
#define GS_REG_FOG        0x0A
#define GS_REG_TEX0_1     0x06
#define GS_REG_FRAME_1    0x4C
#define GS_REG_XYOFFSET_1 0x18
/* Z-buffer/depth-test registers (task #89) - cross-checked
 * against a live fetch of PCSX2's GS/GSRegs.h GIF_A_D_REG enum. */
#define GS_REG_TEST_1     0x47
#define GS_REG_ZBUF_1     0x4E
/* Round 96: SCISSOR_1/2 (clipping) - real, well-known GS register
 * addresses, cross-checked this round against the official Sony
 * GS Users Manual ("7.3 Register List in Address Order": 0x40=
 * SCISSOR_1, 0x41=SCISSOR_2), a genuine primary-source citation
 * (not the session-limited-research caveat noted on Rounds 24-28
 * below - this round had the real manual available). Previously
 * explicitly flagged as unmodeled (see the Round 28 comment
 * above: "CLAMP/TEX2/SCISSOR/FBA remain entirely unmodeled"). */
#define GS_REG_SCISSOR_1  0x40
#define GS_REG_SCISSOR_2  0x41
/* ALPHA_1 (Round 23: alpha blending) - cross-checked against a live
 * research pass over PCSX2's GS/GSRegs.h GIF_A_D_REG enum. */
#define GS_REG_ALPHA_1    0x42

/* Round 26: BITBLTBUF/TRXPOS/TRXREG/TRXDIR - the real GS registers
 * that configure and trigger a host<->local (or local<->local)
 * memory transfer, whose actual pixel payload arrives as an IMAGE-
 * mode GIF packet (or is read out that way, for local-to-host). Real
 * addresses (well-known, standard GS register set - this round's
 * research pass again hit a session limit, see docs/STATUS.md's "GS
 * Round 26" section for the citation-honesty note, same caveat as
 * Rounds 24-25). Only host-to-local (XDIR=0) transfers into a
 * PSMCT32 destination are actually implemented - see gif.c's IMAGE-
 * mode handling for the full scope; local-to-host/local-to-local
 * (XDIR=1/2) are parsed but not acted on (documented gap, consistent
 * with this project not having a local-to-local blit engine at all
 * and no host-readback path either). */
/* Round 27: GS Context 2 (dual-context support). Real hardware has
 * TWO independent rendering contexts, each with its own full set of
 * FRAME/ZBUF/XYOFFSET/TEX0/TEST/ALPHA (and more this project doesn't
 * model - CLAMP/TEX1/TEX2/SCISSOR/FBA/MIPTBP - see the scope note on
 * gif_state_t's ctx1_xxx/ctx2_xxx fields below). PRIM's CTXT bit
 * (bit 9) selects which context's already-configured state applies
 * to the NEXT primitive - context 2's registers are all at address
 * (context-1 address + 1), a clean, well-established real GS register
 * table pattern (already independently confirmed self-consistent by
 * this project's own prior-round additions: TEX0_1=0x06/TEX0_2=0x07,
 * XYOFFSET_1=0x18/XYOFFSET_2=0x19, TEST_1=0x47/TEST_2=0x48,
 * ALPHA_1=0x42/ALPHA_2=0x43, FRAME_1=0x4C/FRAME_2=0x4D,
 * ZBUF_1=0x4E/ZBUF_2=0x4F). Same session-limited-research caveat as
 * Rounds 24-26 applies to this round's specific field/address
 * details, mitigated here by that internal +1 consistency check. */
#define GS_REG_FRAME_2    0x4D
#define GS_REG_XYOFFSET_2 0x19
#define GS_REG_TEX0_2     0x07
#define GS_REG_TEST_2     0x48
#define GS_REG_ALPHA_2    0x43
#define GS_REG_ZBUF_2     0x4F
#define PRIM_CTXT_MASK    0x200u /* PRIM bit 9: 0=context1, 1=context2 */

/* Round 28: mipmaps - TEX1 (LOD calculation params) and MIPTBP1/
 * MIPTBP2 (per-level texture base pointers, levels 1-6). Real
 * addresses (well-known GS register set, same session-limited-
 * research caveat as Rounds 24-27 - see docs/STATUS.md's "GS Round
 * 28" section). Round 29 continued (15th change) closes the
 * context-2 half of the "CLAMP/TEX1/TEX2/SCISSOR/FBA/MIPTBP
 * unmodeled for either context" gap Round 27 explicitly left open:
 * TEX1/MIPTBP1/MIPTBP2 are now genuinely per-context, using the same
 * real _2 register addresses (base+1, the same convention every
 * other _1/_2 pair in this file already follows) and the same
 * ctx1_xxx/ctx2_xxx permanent-storage + gs_activate_context()
 * pattern Round 27 established for FRAME/XYOFFSET/TEX0/ZBUF/TEST/
 * ALPHA. CLAMP/TEX2/SCISSOR/FBA remain entirely unmodeled (for either
 * context) - a separate, larger gap (these registers don't exist in
 * this codebase at all yet, unlike TEX1/MIPTBP which already had a
 * context-1 implementation to extend). */
#define GS_REG_TEX1_1     0x14
#define GS_REG_MIPTBP1_1  0x34
#define GS_REG_MIPTBP2_1  0x36
#define GS_REG_TEX1_2     0x15
#define GS_REG_MIPTBP1_2  0x35
#define GS_REG_MIPTBP2_2  0x37

/* Round 98 (139th finding, task #254, GS gap follow-up 2/N): CLAMP_1/2
 * (real texture wrap-mode registers) - addresses cross-checked against
 * the official GS Users Manual's "7.3 Register List in Address Order"
 * (0x08=CLAMP_1, 0x09=CLAMP_2). Previously entirely unmodeled (flagged
 * since Round 28: "CLAMP/TEX2/SCISSOR/FBA remain entirely unmodeled",
 * confirmed again in Round 96's manual audit). */
#define GS_REG_CLAMP_1    0x08
#define GS_REG_CLAMP_2    0x09

/* CLAMP's WMS/WMT wrap-mode field values (real hardware, manual's own
 * "FIELD" table) - REPEAT wraps via bitmask (real hardware requires
 * power-of-2 texture sizes, matching this project's existing TW/TH
 * log2-size fields); CLAMP clamps to [0, size-1]; REGION_CLAMP clamps
 * to an explicit [MINU,MAXU]/[MINV,MAXV] sub-rectangle; REGION_REPEAT
 * masks with MINU/MINV (UMSK/VMSK) then ORs in MAXU/MAXV (UFIX/VFIX). */
#define GS_CLAMP_REPEAT        0u
#define GS_CLAMP_CLAMP         1u
#define GS_CLAMP_REGION_CLAMP  2u
#define GS_CLAMP_REGION_REPEAT 3u

/* TEX1's MMIN field (3 bits) - real hardware distinguishes several
 * NEAREST/LINEAR x MIPMAP_NEAREST/MIPMAP_LINEAR combinations (values
 * 0-5); this project does not implement trilinear/bilinear filtering
 * at all (nearest-neighbor sampling only, an existing, established
 * limitation - see gif.h's top-of-file scope comment), so the only
 * distinction this round actually needs is "mipmapping engaged or
 * not": MMIN 0-1 = no mipmap (real values NEAREST/LINEAR), MMIN 2-5 =
 * mipmap engaged (some NEAREST_MIPMAP_x/LINEAR_MIPMAP_x combination -
 * this round does not distinguish between them further, since single-
 * level nearest selection is used regardless of which one is set, a
 * deliberate simplification consistent with not modeling trilinear
 * blending). */
#define GS_MMIN_MIPMAP_THRESHOLD 2u /* MMIN >= this value means "mipmapping is engaged" */

#define GS_REG_BITBLTBUF  0x50
#define GS_REG_TRXPOS     0x51
#define GS_REG_TRXREG     0x52
#define GS_REG_TRXDIR     0x53

#define TRXDIR_HOST_TO_LOCAL  0u
#define TRXDIR_LOCAL_TO_HOST  1u
#define TRXDIR_LOCAL_TO_LOCAL 2u
#define TRXDIR_OFF            3u

/* POINT/LINE/LINE_STRIP (task: "GS coverage breadth", item 5) -
 * cross-checked against a live fetch of PCSX2's GS/GSRegs.h `enum
 * GS_PRIM` (GS_POINTLIST=0, GS_LINELIST=1, GS_LINESTRIP=2,
 * GS_TRIANGLELIST=3, ...) - these are real 3-bit PRIM.PRIM hardware
 * field values, not emulator convention. */
#define PRIM_TYPE_POINT          0
#define PRIM_TYPE_LINE           1
#define PRIM_TYPE_LINE_STRIP     2
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
/* PRIM bit 6 (ABE) - real GIFRegPRIM bitfield: PRIM:3,IIP:1,TME:1,
 * FGE:1,ABE:1,AA1:1,FST:1,CTXT:1,FIX:1 (cross-checked against
 * PCSX2's GS/GSRegs.h). Gates real alpha blending (Round 23, see
 * ALPHA_1 below) - matches this file's existing bit-position-
 * comment style for IIP/TME/FST above. */
#define PRIM_ABE_MASK 0x40u
/* PRIM bit 5 (FGE) - real GIFRegPRIM bitfield (see PRIM_ABE_MASK's own
 * comment for the full field ordering: PRIM:3,IIP:1,TME:1,FGE:1,
 * ABE:1,AA1:1,FST:1,CTXT:1,FIX:1). Gates the Round 97 Fog effect
 * (task #254, 138th finding) - see gs_finish_pixel()'s fog-blend step
 * in gif.c and the GS_REG_FOGCOL/cur_fog comments below. */
#define PRIM_FGE_MASK 0x20u
/* FOGCOL (Round 97, task #254): the "distant fog color" (Rfc,Gfc,Bfc)
 * blended against per-pixel Fog coefficient F, per the official GS
 * Users Manual's "3.5. Fog Effect" formula:
 *   R = F*Rv + (0xff-F)*Rfc  (and likewise G, B; A is left untouched)
 * Real, well-known GS register address (manual's "7.3 Register List
 * in Address Order": 0x3d=FOGCOL). */
#define GS_REG_FOGCOL 0x3D

/* GIFRegTEST bitfield (task #89, extended Round 23) - cross-checked
 * against PCSX2's GS/GSRegs.h GIFRegTEST: ATE:1,ATST:3,AREF:8,
 * AFAIL:2,DATE:1,DATM:1,ZTE:1,ZTST:2 (all in word0; word1 unused).
 * ATE/ATST/AREF/AFAIL (real alpha test) are now modeled (Round 23) -
 * see gs_finish_pixel() in gif.c. DATE/DATM (destination-alpha test,
 * a real but distinct/rarer GS feature used for certain stencil-like
 * tricks) remain unmodeled - a deliberate, separate, still-open gap. */
#define TEST_ATE_MASK    0x1u
#define TEST_ATST_SHIFT  1u
#define TEST_ATST_MASK   0x7u
#define TEST_AREF_SHIFT  4u
#define TEST_AREF_MASK   0xFFu
#define TEST_AFAIL_SHIFT 12u
#define TEST_AFAIL_MASK  0x3u
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

/* GS_ATST enum (Round 23) - cross-checked against PCSX2's GS/GSRegs.h
 * GS_ATST: real, literal per-fragment alpha-value compare modes
 * against the 8-bit AREF reference value. */
#define GS_ATST_NEVER    0u
#define GS_ATST_ALWAYS   1u
#define GS_ATST_LESS     2u
#define GS_ATST_LEQUAL   3u
#define GS_ATST_EQUAL    4u
#define GS_ATST_GEQUAL   5u
#define GS_ATST_GREATER  6u
#define GS_ATST_NOTEQUAL 7u

/* GS_AFAIL enum (Round 23) - cross-checked against PCSX2's GS/
 * GSRegs.h GS_AFAIL: what happens to a fragment that FAILS the
 * alpha test above. KEEP discards the fragment entirely (no color,
 * no Z write); FB_ONLY still writes color but suppresses the Z
 * write; ZB_ONLY still writes Z but suppresses the color write;
 * RGB_ONLY writes RGB but preserves the framebuffer's OLD alpha byte
 * (real hardware detail: on a genuine 32-bit-alpha target, which is
 * the only format this project's simplified gs_mem models anyway -
 * see gs_mem.h - so the real "downgrade to FB_ONLY for non-32bit
 * formats" special case PCSX2 documents never applies here). */
#define GS_AFAIL_KEEP     0u
#define GS_AFAIL_FB_ONLY  1u
#define GS_AFAIL_ZB_ONLY  2u
#define GS_AFAIL_RGB_ONLY 3u

/* GIFRegALPHA bitfield (Round 23) - cross-checked against PCSX2's
 * GS/GSRegs.h GIFRegALPHA: A:2,B:2,C:2,D:2 (word0 low byte), FIX:8
 * (word0's top byte, bits 32-39 of the 64-bit register - i.e. byte 4
 * of the 8-byte register). A/B/D select a COLOR input for the blend
 * equation (0=Cs source/fragment color, 1=Cd destination/framebuffer
 * color, 2=black/zero); C selects the blend COEFFICIENT (0=As source
 * alpha, 1=Ad destination alpha, 2=Af, the fixed FIX value). Real
 * blend equation (see gs_finish_pixel() in gif.c): Color = ((A-B)*C)
 * >>7 + D - a plain truncating shift, no rounding bias, cross-checked
 * against PCSX2's GSDrawScanline.cpp AlphaBlend path. The written
 * alpha channel is always the fragment's own source alpha (As) -
 * blending only ever affects RGB on real hardware. */
#define ALPHA_A_SHIFT 0u
#define ALPHA_B_SHIFT 2u
#define ALPHA_C_SHIFT 4u
#define ALPHA_D_SHIFT 6u
#define ALPHA_ABCD_MASK 0x3u
/* FIX occupies bits 32-39 of the 64-bit register, i.e. the LOW byte
 * of word1 (this project's "data_hi" 32-bit half in apply_ad_write) -
 * extracted as `data_hi & ALPHA_FIX_MASK` directly, no shift needed
 * (see apply_ad_write's GS_REG_ALPHA_1 case in gif.c). */
#define ALPHA_FIX_MASK  0xFFu
#define GS_ALPHA_CS 0u
#define GS_ALPHA_CD 1u
#define GS_ALPHA_ZERO 2u
#define GS_ALPHA_AS 0u
#define GS_ALPHA_AD 1u
#define GS_ALPHA_AFIX 2u

/* TEX0's TFX field (2 bits) - cross-checked against PCSX2's own
 * GS/GSRegs.h GS_TFX enum. HIGHLIGHT/HIGHLIGHT2 are simplified to
 * behave like MODULATE (an honest, noted simplification - real
 * hardware's highlight modes involve a second, specular-like term
 * this project does not model). */
#define TEX_TFX_MODULATE  0u
#define TEX_TFX_DECAL     1u

/* TEX0's PSM field (6 bits, word0 bits 20-25) - real GS pixel storage
 * mode enum values (well-known, widely-published PS2 GS constants -
 * cross-referenced against this project's prior TEX0/FRAME/ZBUF work
 * and general PS2 GS documentation; this round's live source-fetch
 * research pass hit a session limit before it could run, so these
 * are sourced from established knowledge rather than a fresh citation
 * trail - flagged here per this project's citation-honesty policy).
 * This project's gs_mem is PSMCT32-storage-only (see gs_mem.h), so
 * only PSMCT32/PSMT8/PSMT4 are actually supported by the sampler
 * below; the other real values are listed for documentation and to
 * make an unsupported-PSM texture fail loudly (falls back to
 * PSMCT32 sampling) rather than silently - a deliberate, documented
 * gap, not a claim of full PSM coverage. */
#define TEX_PSM_PSMCT32  0x00u
#define TEX_PSM_PSMCT24  0x01u
#define TEX_PSM_PSMCT16  0x02u
#define TEX_PSM_PSMCT16S 0x0Au
#define TEX_PSM_PSMT8    0x13u
#define TEX_PSM_PSMT4    0x14u
#define TEX_PSM_PSMT8H   0x1Bu
#define TEX_PSM_PSMT4HL  0x24u
#define TEX_PSM_PSMT4HH  0x2Cu

/* CLUT geometry constants (Round 24, CLUT/paletted textures). Real
 * hardware's CSM1 CLUT storage addresses palette entries in units of
 * 16 ("CSA units") regardless of whether the texture is PSMT8 (256
 * entries = 16 CSA units) or PSMT4 (16 entries = 1 CSA unit, letting
 * up to 32 different 4-bit palettes share the same CLUT storage
 * region at different CSA offsets). This project models the CLUT as
 * its own small gs_mem region addressed via the existing bp/bw
 * convention, with a fixed row width of 16 entries/row
 * (CLUT_ROW_WIDTH) - consistent with the real 16-entries-per-CSA-unit
 * granularity. Only CPSM=PSMCT32 (32-bit RGBA CLUT entries) is
 * supported - CPSM=PSMCT16/PSMCT16S (16-bit CLUT entries) is a
 * documented, unsupported gap (this project's gs_mem has no PSMCT16
 * storage format at all, see gs_mem.h). */
#define CLUT_ROW_WIDTH   16u
#define CLUT_CSA_UNIT    16u

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

    /* PSM/CLUT fields (Round 24, CLUT/paletted textures). tex_psm is
     * TEX0's real 6-bit PSM field (see TEX_PSM_* above) - only
     * PSMT8/PSMT4 engage the CLUT path below, anything else
     * (including the PSMCT32 default) uses the pre-existing direct
     * gs_mem sample. tex_cbp/tex_cpsm/tex_csa/tex_cld are TEX0's
     * CLUT fields - cbp is used directly as OUR gs_mem bp convention
     * for the CLUT's storage location (exactly like tex_tbp0 for the
     * texture itself); cpsm is checked but only PSMCT32 (0) is
     * actually supported (see CLUT_ROW_WIDTH's comment); csa selects
     * a 16-entry-unit offset within the CLUT storage; cld (CLUT load
     * control) is parsed but not acted on - this emulator has no
     * CLUT cache to manage, so every texture sample simply re-reads
     * the CLUT fresh from gs_mem each time, which is always correct
     * (just not a claim of matching real hardware's CLUT-cache
     * timing/behavior). */
    uint32_t tex_psm;
    uint32_t tex_cbp, tex_cpsm, tex_csa, tex_cld;

    /* Round 26: host-to-local IMAGE transfer state (BITBLTBUF/
     * TRXPOS/TRXREG/TRXDIR - see their definitions above for the
     * scope). trx_active becomes true only when TRXDIR is written
     * with XDIR=0 (host-to-local) AND the destination PSM is PSMCT32
     * (the only format this project's gs_mem actually stores) -
     * anything else leaves trx_active false, so a subsequent IMAGE-
     * mode packet's data is safely skipped (byte-accounted, not
     * interpreted) rather than misinterpreted. trx_cur_x/trx_cur_y
     * are the transfer's progress cursor, relative to (dsax,dsay),
     * reset to (0,0) each time TRXDIR triggers a new transfer. */
    uint32_t trx_dbp, trx_dbw, trx_dpsm;
    uint32_t trx_dsax, trx_dsay;
    uint32_t trx_rrw, trx_rrh;
    uint32_t trx_xdir;
    int trx_active;
    uint32_t trx_cur_x, trx_cur_y;

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

    /* Real alpha test (Round 23) - TEST_1's ATE/ATST/AREF/AFAIL
     * fields, see TEST_ATE_MASK/etc and GS_ATST_xxx && GS_AFAIL_xxx constants above. */
    int ate;
    int atst;
    uint32_t aref;
    int afail;

    /* Real alpha blending (Round 23) - ALPHA_1's A/B/C/D/FIX fields,
     * see ALPHA_*_SHIFT/GS_ALPHA_* above. Gated at draw time by
     * PRIM's own ABE bit (PRIM_ABE_MASK), matching real hardware
     * (the ALPHA register's values are latched independently of
     * whether blending is currently enabled - only PRIM.ABE decides
     * whether they're actually used for a given draw). */
    uint32_t alpha_a, alpha_b, alpha_c, alpha_d;
    uint32_t alpha_fix;

    /* Round 96: SCISSOR_1/2 - real scissoring/clip rectangle, per
     * GS Users Manual "SCISSOR_1/SCISSOR_2: Setting for Scissoring
     * Area": SCAX0/SCAY0 = upper-left, SCAX1/SCAY1 = lower-right,
     * inclusive, in the same window coordinate system as XYZ2's
     * screen coordinates (i.e. directly comparable to this
     * project's own rasterizer x/y values, no extra scaling).
     * scissor_configured follows the same established "safety
     * gate" pattern as zbuf_configured above: real hardware
     * defaults SCISSOR to (0,0)-(0,0) (which would clip away
     * everything), but this project defaults to "not configured =
     * no clipping applied" so every pre-existing test/demo that
     * never writes SCISSOR keeps drawing exactly as before. */
    uint32_t scissor_x0, scissor_x1, scissor_y0, scissor_y1;
    int scissor_configured;

    /* Round 27: GS Context 2 (dual-context support) - see
     * PRIM_CTXT_MASK/GS_REG_FRAME_2/etc above for the register-
     * address side. The flat fields above (fbp, fbw, xyoffset_x/y,
     * tex_tbp0, tex_tbw, tex_tfx, tex_tw, tex_th, tex_psm, tex_cbp,
     * tex_cpsm, tex_csa, tex_cld, zbp, zmsk, zbuf_configured, zte,
     * ztst, ate, atst, aref, afail, alpha_a/b/c/d, alpha_fix) now
     * serve as the CURRENTLY ACTIVE context's view - refreshed by
     * gs_activate_context() (called at the top of each rasterizer,
     * right before a primitive is actually drawn) from whichever of
     * the two permanent per-context storage banks below PRIM's CTXT
     * bit currently selects. Every A+D write to a _1 register (e.g.
     * FRAME_1) updates BOTH the matching ctx1_xxx permanent field AND
     * the flat field directly (so code that reads the flat fields
     * immediately after a register write, without an intervening
     * primitive draw - as several pre-existing tests do - keeps
     * seeing correct, unchanged behavior); a _2 register write (e.g.
     * FRAME_2) updates ONLY the ctx2_xxx permanent field, since
     * context 2 only becomes "live" in the flat fields once a
     * primitive is actually drawn with PRIM.CTXT=1 selected. This
     * design deliberately avoids touching gs_finish_pixel()/
     * gs_sample_texel()/gs_sample_clut()'s existing internals at all
     * (they keep reading the flat fields exactly as before this
     * round) - only apply_ad_write() (new _2 cases + mirroring the
     * _1 cases) and the 4 rasterizers (one new gs_activate_context()
     * call each, at the very top) change. */
    uint32_t ctx1_fbp, ctx1_fbw;
    uint32_t ctx1_xyoffset_x, ctx1_xyoffset_y;
    uint32_t ctx1_tex_tbp0, ctx1_tex_tbw, ctx1_tex_tfx, ctx1_tex_tw, ctx1_tex_th;
    uint32_t ctx1_tex_psm, ctx1_tex_cbp, ctx1_tex_cpsm, ctx1_tex_csa, ctx1_tex_cld;
    uint32_t ctx1_zbp;
    int ctx1_zmsk, ctx1_zbuf_configured, ctx1_zte, ctx1_ztst;
    int ctx1_ate, ctx1_atst, ctx1_afail;
    uint32_t ctx1_aref;
    uint32_t ctx1_alpha_a, ctx1_alpha_b, ctx1_alpha_c, ctx1_alpha_d, ctx1_alpha_fix;
    uint32_t ctx1_scissor_x0, ctx1_scissor_x1, ctx1_scissor_y0, ctx1_scissor_y1;
    int ctx1_scissor_configured;

    uint32_t ctx2_fbp, ctx2_fbw;
    uint32_t ctx2_xyoffset_x, ctx2_xyoffset_y;
    uint32_t ctx2_tex_tbp0, ctx2_tex_tbw, ctx2_tex_tfx, ctx2_tex_tw, ctx2_tex_th;
    uint32_t ctx2_tex_psm, ctx2_tex_cbp, ctx2_tex_cpsm, ctx2_tex_csa, ctx2_tex_cld;
    uint32_t ctx2_zbp;
    int ctx2_zmsk, ctx2_zbuf_configured, ctx2_zte, ctx2_ztst;
    int ctx2_ate, ctx2_atst, ctx2_afail;
    uint32_t ctx2_aref;
    uint32_t ctx2_alpha_a, ctx2_alpha_b, ctx2_alpha_c, ctx2_alpha_d, ctx2_alpha_fix;
    uint32_t ctx2_scissor_x0, ctx2_scissor_x1, ctx2_scissor_y0, ctx2_scissor_y1;
    int ctx2_scissor_configured;

    /* Round 28: mipmaps (TEX1 + MIPTBP1/MIPTBP2) - see
     * GS_REG_TEX1_1/MIPTBP1_1/MIPTBP2_1's header comment for the
     * field-layout scope note. TEX1's fields: tex1_lcm (LOD
     * Calculation Method: 0=computed from texture/screen size ratio,
     * 1=fixed from K), tex1_mxl (Max LOD level, 0-6), tex1_mmag/
     * tex1_mmin (real filter mode fields - only tex1_mmin is actually
     * used, to decide whether mipmapping is engaged at all, per
     * GS_MMIN_MIPMAP_THRESHOLD's comment), tex1_mtba (Mipmap Texture
     * Base Auto - only MTBA=0, explicit MIPTBP1/2 lookup, is
     * implemented; MTBA=1's automatic per-level address formula is a
     * documented, unimplemented gap - falls back to level 0 always),
     * tex1_l (parsed and stored but NOT applied to the LOD formula
     * this round - a documented, explicit simplification, not a
     * fabricated formula detail), tex1_k (signed 12-bit LOD bias,
     * 1/16 units - applied when tex1_lcm=1). tex_mip_tbp/tex_mip_tbw
     * hold levels 1-6's base pointer/width (index 0 = level 1, index
     * 5 = level 6), parsed from MIPTBP1_1 (levels 1-3) and
     * MIPTBP2_1 (levels 4-6) - level 0 itself continues to use the
     * existing tex_tbp0/tex_tbw fields, unchanged. These are the
     * "flat/active" fields gs_activate_context() refreshes at the top
     * of each rasterizer (Round 29 continued, 15th change) from
     * whichever of ctx1_tex1_xxx/ctx2_tex1_xxx below PRIM's CTXT bit
     * currently selects - same pattern as fbp/tex_tbp0/alpha_a/etc. */
    int tex1_lcm;
    uint32_t tex1_mxl;
    int tex1_mmag;
    uint32_t tex1_mmin;
    int tex1_mtba;
    uint32_t tex1_l;
    int32_t tex1_k;
    uint32_t tex_mip_tbp[6], tex_mip_tbw[6];

    /* Round 29 continued (15th change): per-context permanent storage
     * for the TEX1/MIPTBP fields above - see this file's
     * GS_REG_TEX1_2/MIPTBP1_2/MIPTBP2_2 comment for the scope. */
    int ctx1_tex1_lcm;
    uint32_t ctx1_tex1_mxl;
    int ctx1_tex1_mmag;
    uint32_t ctx1_tex1_mmin;
    int ctx1_tex1_mtba;
    uint32_t ctx1_tex1_l;
    int32_t ctx1_tex1_k;
    uint32_t ctx1_tex_mip_tbp[6], ctx1_tex_mip_tbw[6];

    int ctx2_tex1_lcm;
    uint32_t ctx2_tex1_mxl;
    int ctx2_tex1_mmag;
    uint32_t ctx2_tex1_mmin;
    int ctx2_tex1_mtba;
    uint32_t ctx2_tex1_l;
    int32_t ctx2_tex1_k;
    uint32_t ctx2_tex_mip_tbp[6], ctx2_tex_mip_tbw[6];

    /* Round 98 (139th finding, task #254): CLAMP_1/2 texture wrap mode
     * - wms/wmt hold the real 2-bit wrap-mode field (GS_CLAMP_* above)
     * for S/T independently; minu/maxu/minv/maxv hold the real 10-bit
     * region parameters (meaning depends on wms/wmt - see GS_CLAMP_*
     * comment). Applied in the texture-sampling step of
     * rasterize_triangle()/rasterize_sprite() (gs_apply_clamp_wrap()
     * in gif.c) - previously coordinates were only naively clamped to
     * 0 on the low side with no real wrap/region semantics at all (see
     * this file's top-of-file scope comment, "No CLAMP register
     * modeling"). Same "flat + per-context, refreshed by
     * gs_activate_context()" pattern as tex1_xxx above. */
    uint32_t clamp_wms, clamp_wmt;
    uint32_t clamp_minu, clamp_maxu, clamp_minv, clamp_maxv;
    /* Safety gate (this project's established convention, same as
     * zbuf_configured/scissor_configured): real hardware defaults
     * WMS/WMT to REPEAT, but this project defaults to "not configured
     * = exactly this codebase's pre-existing behavior" (naive clamp-
     * to-0 on the low side, gs_sample_texel()'s own out-of-range-
     * returns-0 on the high side) so every pre-existing texture test
     * keeps sampling exactly as before this round unless it opts in
     * by actually writing CLAMP_1/2. */
    int clamp_configured;
    uint32_t ctx1_clamp_wms, ctx1_clamp_wmt;
    uint32_t ctx1_clamp_minu, ctx1_clamp_maxu, ctx1_clamp_minv, ctx1_clamp_maxv;
    int ctx1_clamp_configured;
    uint32_t ctx2_clamp_wms, ctx2_clamp_wmt;
    uint32_t ctx2_clamp_minu, ctx2_clamp_maxu, ctx2_clamp_minv, ctx2_clamp_maxv;
    int ctx2_clamp_configured;

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

    /* Round 97 (138th finding, task #254): "current fog value" -
     * exactly the same latch-then-read-at-vertex-kick pattern this
     * file already uses for cur_u/cur_v (UV) and cur_s/cur_t (ST):
     * a standalone FOG register write (GS_REG_FOG, either PACKED tag
     * 0x0a or the A+D address) sets this; the combined XYZF2/XYZF3
     * vertex-kick registers ALSO set it directly from their own
     * embedded F field in PACKED mode (see apply_xyz2_kick() in
     * gif.c). Plain XYZ2/XYZ3 (no F field) leave it unchanged, so a
     * fog value set once persists across subsequent XYZ2-only
     * vertices - real hardware behavior for an internal GS register.
     * Default 0xFF (per the GS Users Manual's "3.5. Fog Effect":
     * F=255 is "the fog effect is at the minimum", i.e. the vertex's
     * own color passes through unmodified) - this project's usual
     * safety-gate convention so pre-existing tests/demos that never
     * touch FOG/XYZF2 keep drawing exactly as before this round even
     * if a future round mistakenly left FGE-gating off somewhere. */
    uint32_t cur_fog;
    /* FOGCOL (Round 97) - packed as R in bits 0-7, G in 8-15, B in
     * 16-23 (alpha byte unused/0), matching this file's existing
     * rgba_pack() channel convention for consistency, even though
     * FOGCOL itself has no alpha field on real hardware. */
    uint32_t fogcol;

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
    /* SPRITE's first-vertex Fog coefficient (Round 97, task #254) -
     * latched from cur_fog at the same moment v0z is (see
     * apply_xyz2_kick()'s vertex-kick logic in gif.c). */
    uint32_t v0f;

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
    /* Per-vertex Fog coefficient (Round 97, task #254) - latched from
     * cur_fog at every vertex kick, same shape/rolling-window rules
     * as tri_z above; interpolated the same plain-affine barycentric
     * way as Z in rasterize_triangle() (the GS Users Manual's "3.5.
     * Fog Effect" describes F as linearly interpolated per vertex,
     * with no mention of the 1/Q perspective correction ST/UV need -
     * matching Z's own already-post-projection screen-space-linear
     * behavior, not independently re-derived here). */
    uint32_t tri_f[3];

    /* LINE/LINE_STRIP vertex accumulation (task: "GS coverage
     * breadth", item 5) - a 2-slot rolling window, the same shape as
     * TRIANGLE_STRIP's 3-slot one above. line_vseq counts vertices
     * received since PRIM last changed (reset on every PRIM write,
     * same as tri_vseq). Real hardware: LINE draws one segment per
     * PAIR of vertices (no reuse - matches PCSX2's `NumIndicesForPrim`
     * returning 2 for both LINELIST/LINESTRIP); LINE_STRIP instead
     * keeps the most recent vertex as the next segment's start,
     * giving a connected polyline - same "rolling window" shape as
     * TRIANGLE_STRIP, just 2 slots instead of 3. No texture-
     * coordinate fields: real GS hardware does not texture-map
     * POINT/LINE primitives (only SPRITE/TRIANGLE support TME) - not
     * modeled here since there's nothing to model. */
    int line_vseq;
    int32_t line_x[2], line_y[2];
    uint32_t line_rgba[2];
    uint32_t line_z[2];
    /* Per-vertex Fog coefficient (Round 97, task #254) - same rolling
     * 2-slot shape as line_z above. */
    uint32_t line_f[2];

    uint64_t quadwords_seen;
    uint64_t sprites_drawn;
    uint64_t triangles_drawn;
    uint64_t lines_drawn;   /* task: "GS coverage breadth" */
    uint64_t points_drawn;  /* task: "GS coverage breadth" */
    uint64_t unsupported_prims_seen;
    uint64_t pixels_ztest_failed; /* task #89 - counts fragments rejected by the Z test, for test visibility */
    uint64_t pixels_atest_failed; /* Round 23 - counts fragments rejected by the alpha test, for test visibility */
} gif_state_t;

gif_state_t *gif_get_state(void);

#endif
