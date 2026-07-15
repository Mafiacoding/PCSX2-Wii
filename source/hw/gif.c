/*
 * gif.c - see include/core/hw/gif.h for scope notes and references.
 */

#include "core/hw/gif.h"
#include "core/hw/gs_mem.h"
#include <string.h>
#include <math.h> /* Round 28: log2() for mipmap LOD selection - see rasterize_sprite()'s mip-level logic */

static gif_state_t g_gif;

void gif_init(void)
{
    memset(&g_gif, 0, sizeof(g_gif));
    g_gif.fbw = 640; /* sane default so an A+D FRAME write isn't strictly required for tests/demos */
    /* Round 27: both contexts' permanent storage gets the same guarded
     * default, so a PRIM.CTXT=1 draw before ANY FRAME_2 write behaves
     * exactly as sanely as context 1's pre-existing default (no
     * aliasing/zero-width surprise). */
    g_gif.ctx1_fbw = 640;
    g_gif.ctx2_fbw = 640;
    /* Round 97 (138th finding, task #254): cur_fog defaults to 0xFF
     * ("fog effect at minimum" per the GS Users Manual - i.e. the
     * vertex's own color passes through unmodified) rather than
     * memset's 0 ("fog effect at maximum" - full fog color) - this
     * project's usual safety-gate convention, see cur_fog's own field
     * comment in gif.h. */
    g_gif.cur_fog = 0xFFu;
    g_gif.prmodecont_ac = 1u; /* Round 99: default AC=1 ("use PRIM") - safety gate */
    g_gif.prmode = 0u;
    g_gif.colclamp = 1u; /* Round 101: default CLAMP=1 - safety gate, matches pre-existing hardcoded behavior */
    g_gif.colclamp_configured = 0;
}

gif_state_t *gif_get_state(void) { return &g_gif; }

/* GIF_REG_* / GS_REG_* / PRIM_TYPE_* are now public - see gif.h. */

static inline uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Resets the triangle AND line vertex-accumulation sequences - called
 * whenever PRIM is written (real hardware starts a fresh vertex queue
 * on a new PRIM, so a mid-strip primitive-type change can't
 * accidentally draw a triangle/line from mismatched vertices). Also
 * clears has_vertex0 (SPRITE's own 1-slot accumulator) for the same
 * reason - previously only reset implicitly by SPRITE completing a
 * draw; a PRIM change mid-SPRITE-vertex-pair could otherwise leak a
 * stale first vertex into whatever primitive comes next. */
static void reset_tri_vseq(void) { g_gif.tri_vseq = 0; g_gif.line_vseq = 0; g_gif.has_vertex0 = 0; }

/* Flat-shaded triangle fill via edge functions (standard scanline
 * rasterization - plain 2D geometry, not real-hardware-specific, so
 * it doesn't need external verification the way register layouts do).
 * Single color for the whole triangle (g_gif.rgba at the time the
 * triangle completes) - no per-vertex Gouraud shading, no Z test, no
 * texturing; see gif.h's scope comment. */
static int32_t edge(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t px, int32_t py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* PRIM_IIP_MASK is now public - see gif.h. */

static inline uint8_t rgba_channel(uint32_t rgba, int shift) { return (uint8_t)((rgba >> shift) & 0xFFu); }
static inline uint32_t rgba_pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return (a << 24) | (b << 16) | (g << 8) | r;
}

/* Reinterprets a raw 32-bit GIF/A+D data word as the IEEE-754 float
 * it represents on real hardware (S/T/Q are all real floats - GS/
 * GSRegs.h's GIFRegRGBAQ/GIFRegST/GIFPackedSTQ all use `float`
 * members directly). memcpy avoids strict-aliasing UB from a
 * pointer-cast/union reinterpretation. */
static inline float u32_to_float(uint32_t v)
{
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

/* Round 24: real CLUT/paletted-texture sampling (PSMT8/PSMT4), gated
 * by TEX0's PSM field. See gif.h's TEX_PSM_xxx/CLUT_ROW_WIDTH/
 * CLUT_CSA_UNIT header comments for the full scope, addressing
 * scheme, and citation-honesty note (this round's live source-fetch
 * research pass hit a session limit before it could run, so the CLUT
 * addressing scheme below is sourced from established PS2 GS
 * knowledge rather than a fresh citation trail this round).
 *
 * For PSMT8/PSMT4, this project stores the texture's raw palette
 * INDEX (not a color) at each texel slot in gs_mem, via the exact
 * same `gs_mem_read_psmct32(tbp,tbw,x,y)` convention used for
 * PSMCT32 texture color - masked down to the relevant index range
 * (0-255 for PSMT8, 0-15 for PSMT4). This mirrors the project's
 * already-established, documented limitation that there is no real
 * texture-upload/bit-packing path yet (see test_gif_texture.c's own
 * scope note: textures are pre-existing gs_mem content, filled
 * directly via gs_mem_write_psmct32() before a test packet runs) -
 * real hardware's tightly-packed byte/nibble-per-texel storage is a
 * separate concern (the REGLIST/IMAGE transfer-mode work) from CLUT
 * lookup itself, and is not conflated here.
 *
 * The palette itself lives in gs_mem at tex_cbp (this project's own
 * bp convention, exactly like tex_tbp0), addressed as a small
 * CLUT_ROW_WIDTH-wide (16 entries/row) region: entry N is at pixel
 * (N % CLUT_ROW_WIDTH, N / CLUT_ROW_WIDTH) within that region. CSA
 * (tex_csa) selects a CLUT_CSA_UNIT (16-entry) offset within the
 * palette, matching real hardware's CSA addressing granularity - so
 * PSMT4's 16-entry palette at CSA=csa starts at flat index
 * `csa*CLUT_CSA_UNIT`, and PSMT8's 256-entry palette (spanning 16 CSA
 * units) conventionally starts at CSA=0.
 *
 * PSMT8's real hardware CSM1 storage additionally swizzles the raw
 * index before lookup - bits 3 and 4 of the index are swapped
 * (`(idx & 0xE7) | ((idx & 0x08) << 1) | ((idx & 0x10) >> 1)`), a
 * well-known PS2 GS quirk frequently documented in PS2 homebrew
 * texture-conversion tooling as the "CSM1 8-bit CLUT swizzle" -
 * flagged here as sourced from established community knowledge
 * rather than a primary-source citation this round, consistent with
 * this project's citation-honesty policy. PSMT4 does not need this
 * swizzle (its 16-entry palette has no sub-block structure to
 * rearrange).
 *
 * Only CPSM=PSMCT32 (32-bit RGBA CLUT entries) is supported - see
 * CLUT_ROW_WIDTH's header comment for why CPSM=PSMCT16/16S is a
 * documented, unsupported gap. An unsupported CPSM value falls back
 * to treating the raw index as if it were already a packed RGBA
 * color (better than a crash, honestly wrong rather than silently
 * "correct"). */
static uint32_t gs_sample_clut(uint32_t index)
{
    uint32_t flat = g_gif.tex_csa * CLUT_CSA_UNIT + index;
    uint32_t cx = flat % CLUT_ROW_WIDTH;
    uint32_t cy = flat / CLUT_ROW_WIDTH;
    return gs_mem_read_psmct32(g_gif.tex_cbp, CLUT_ROW_WIDTH, cx, cy);
}

static uint32_t gs_sample_texel(int32_t tex_x, int32_t tex_y)
{
    uint32_t raw = gs_mem_read_psmct32(g_gif.tex_tbp0, g_gif.tex_tbw,
                                        (uint32_t)tex_x, (uint32_t)tex_y);
    if (g_gif.tex_psm == TEX_PSM_PSMT8) {
        uint32_t idx = raw & 0xFFu;
        uint32_t swizzled = (idx & 0xE7u) | ((idx & 0x08u) << 1) | ((idx & 0x10u) >> 1);
        return gs_sample_clut(swizzled);
    } else if (g_gif.tex_psm == TEX_PSM_PSMT4) {
        uint32_t idx = raw & 0x0Fu;
        return gs_sample_clut(idx);
    }
    /* PSMCT32 (default) and any other unsupported PSM: sample
     * directly, no CLUT indirection. */
    return raw;
}

/* Round 23: real alpha test (TEST_1's ATE/ATST/AREF/AFAIL) and real
 * alpha blending (ALPHA_1, gated by PRIM's ABE bit) - see gif.h's
 * TEST_xxx, GS_ATST_xxx, GS_AFAIL_xxx, ALPHA_xxx, and GS_ALPHA_xxx field comments for
 * the full citation trail (PCSX2's GS/GSRegs.h + GSDrawScanline.cpp,
 * cross-checked via a dedicated research pass this round).
 *
 * Centralizes what was previously 4 near-identical inline "write
 * color, maybe write Z" tail blocks - one per rasterizer (triangle/
 * sprite/point/line) - into a single shared helper. This is a
 * deliberate, modest deviation from this file's established pattern
 * of duplicating the (much smaller) Z-test block identically in each
 * rasterizer: alpha test + blending is substantially more logic, and
 * the alpha unit's behavior is identical regardless of which
 * primitive produced the fragment (real hardware doesn't have 4
 * separate alpha units either) - keeping 4 independent copies in
 * sync here would be a real, avoidable maintenance risk.
 *
 * `frag_color` is the fragment's fully shaded/textured color (what
 * every rasterizer previously wrote directly via gs_mem_write_psmct32).
 * `z_write_allowed` is the caller's existing zbuf_configured/zmsk
 * gate (unchanged from before this round) - this function may
 * additionally suppress it (AFAIL) but never re-enables a Z write the
 * caller didn't already allow. */
/* Forward declaration: gs_effective_attr_prim() is defined further down
 * (right before gs_activate_context()) but gs_finish_pixel() below needs
 * it for the ABE check - Round 99, task #254. */
static uint32_t gs_effective_attr_prim(void);

/* Round 101 (142nd finding, task #254 - GS gap follow-up 5/N): COLCLAMP -
 * see gif.h's GS_REG_COLCLAMP/colclamp field comments for the full
 * citation. Applied at every RGB-channel clamp site in this file's
 * render pipeline (alpha blend, fog blend, Gouraud interpolation,
 * texture modulate) - not just the single "final" write, since this
 * codebase's pipeline clamps defensively at each stage rather than
 * carrying unclamped wide intermediates through to one final point;
 * treating every one of those clamp sites as COLCLAMP-governed is the
 * most faithful and consistent way to honor a MASK-mode configuration
 * anywhere overflow can occur (texture modulate's (tex*color)/128 is
 * the most likely real site to actually exceed 255, e.g. 255*255/128
 * = 507) without a larger internal-precision rework. Negative inputs
 * (not expected in practice, since every caller already computes non-
 * negative sums/products) still wrap via the same low-8-bits masking,
 * treating the value's two's-complement bit pattern like a real
 * hardware register would. */
static uint32_t gs_colclamp_channel(int32_t v)
{
    if (!g_gif.colclamp_configured || g_gif.colclamp)
        return (uint32_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    return (uint32_t)v & 0xFFu;
}

static void gs_finish_pixel(int32_t xx, int32_t yy, uint32_t frag_color, uint32_t frag_z, int z_write_allowed)
{
    uint32_t frag_r = rgba_channel(frag_color, 0);
    uint32_t frag_g = rgba_channel(frag_color, 8);
    uint32_t frag_b = rgba_channel(frag_color, 16);
    uint32_t frag_a = rgba_channel(frag_color, 24);

    int color_write = 1;
    int z_write = z_write_allowed;
    int alpha_test_failed = 0;

    if (g_gif.ate) {
        int fail;
        switch (g_gif.atst) {
        case GS_ATST_NEVER:    fail = 1; break;
        case GS_ATST_ALWAYS:   fail = 0; break;
        case GS_ATST_LESS:     fail = (frag_a >= g_gif.aref); break;
        case GS_ATST_LEQUAL:   fail = (frag_a >  g_gif.aref); break;
        case GS_ATST_EQUAL:    fail = (frag_a != g_gif.aref); break;
        case GS_ATST_GEQUAL:   fail = (frag_a <  g_gif.aref); break;
        case GS_ATST_GREATER:  fail = (frag_a <= g_gif.aref); break;
        case GS_ATST_NOTEQUAL: fail = (frag_a == g_gif.aref); break;
        default: fail = 0; break;
        }
        if (fail) {
            alpha_test_failed = 1;
            g_gif.pixels_atest_failed++;
            switch (g_gif.afail) {
            case GS_AFAIL_KEEP:     color_write = 0; z_write = 0; break;
            case GS_AFAIL_FB_ONLY:  z_write = 0; break; /* color still writes, Z suppressed */
            case GS_AFAIL_ZB_ONLY:  color_write = 0; break; /* Z still writes (if allowed), color suppressed */
            case GS_AFAIL_RGB_ONLY: z_write = 0; break; /* RGB still writes, alpha preserved below, Z suppressed */
            default: break;
            }
        }
    }

    if (!color_write && !z_write)
        return;

    if (color_write) {
        uint32_t out_r = frag_r, out_g = frag_g, out_b = frag_b, out_a = frag_a;

        if (gs_effective_attr_prim() & PRIM_ABE_MASK) {
            /* Real alpha blending - RGB channels only; the written
             * alpha is always the fragment's own source alpha (As),
             * matching real GS hardware (see gif.h's ALPHA field
             * comment). */
            uint32_t dst = gs_mem_read_psmct32(g_gif.fbp, g_gif.fbw, (uint32_t)xx, (uint32_t)yy);
            uint32_t dst_r = rgba_channel(dst, 0),  dst_g = rgba_channel(dst, 8);
            uint32_t dst_b = rgba_channel(dst, 16), dst_a = rgba_channel(dst, 24);

            uint32_t coeff;
            switch (g_gif.alpha_c) {
            case GS_ALPHA_AS: coeff = frag_a; break;
            case GS_ALPHA_AD: coeff = dst_a;  break;
            default:          coeff = g_gif.alpha_fix; break; /* Af */
            }

            int32_t r_a = (g_gif.alpha_a == GS_ALPHA_CS) ? (int32_t)out_r : (g_gif.alpha_a == GS_ALPHA_CD) ? (int32_t)dst_r : 0;
            int32_t r_b = (g_gif.alpha_b == GS_ALPHA_CS) ? (int32_t)out_r : (g_gif.alpha_b == GS_ALPHA_CD) ? (int32_t)dst_r : 0;
            int32_t r_d = (g_gif.alpha_d == GS_ALPHA_CS) ? (int32_t)out_r : (g_gif.alpha_d == GS_ALPHA_CD) ? (int32_t)dst_r : 0;
            int32_t g_a = (g_gif.alpha_a == GS_ALPHA_CS) ? (int32_t)out_g : (g_gif.alpha_a == GS_ALPHA_CD) ? (int32_t)dst_g : 0;
            int32_t g_b = (g_gif.alpha_b == GS_ALPHA_CS) ? (int32_t)out_g : (g_gif.alpha_b == GS_ALPHA_CD) ? (int32_t)dst_g : 0;
            int32_t g_d = (g_gif.alpha_d == GS_ALPHA_CS) ? (int32_t)out_g : (g_gif.alpha_d == GS_ALPHA_CD) ? (int32_t)dst_g : 0;
            int32_t b_a = (g_gif.alpha_a == GS_ALPHA_CS) ? (int32_t)out_b : (g_gif.alpha_a == GS_ALPHA_CD) ? (int32_t)dst_b : 0;
            int32_t b_b = (g_gif.alpha_b == GS_ALPHA_CS) ? (int32_t)out_b : (g_gif.alpha_b == GS_ALPHA_CD) ? (int32_t)dst_b : 0;
            int32_t b_d = (g_gif.alpha_d == GS_ALPHA_CS) ? (int32_t)out_b : (g_gif.alpha_d == GS_ALPHA_CD) ? (int32_t)dst_b : 0;

            /* Real GS blend equation: Color = ((A-B)*C)>>7 + D - a
             * plain truncating shift, no rounding bias (cross-checked
             * against PCSX2's GSDrawScanline.cpp AlphaBlend path).
             * The coefficient is deliberately NOT clamped to [0,1] -
             * real hardware allows results >1.0x ("boosted" colors)
             * when coeff > 128, a genuine, documented hardware
             * behavior some games rely on. The FINAL result is passed
             * through gs_colclamp_channel() (Round 101, task #254),
             * which now genuinely honors COLCLAMP=0's real "wrap
             * instead of clamp" mode - previously a known, deliberately
             * un-modeled gap noted right here; closed this round. */
            int32_t r = ((r_a - r_b) * (int32_t)coeff) / 128 + r_d;
            int32_t g = ((g_a - g_b) * (int32_t)coeff) / 128 + g_d;
            int32_t b = ((b_a - b_b) * (int32_t)coeff) / 128 + b_d;
            out_r = gs_colclamp_channel(r);
            out_g = gs_colclamp_channel(g);
            out_b = gs_colclamp_channel(b);
        }

        if (alpha_test_failed && g_gif.afail == GS_AFAIL_RGB_ONLY) {
            /* Real AFAIL=RGB_ONLY-on-fail: RGB channels still write,
             * but the alpha channel keeps its OLD framebuffer value
             * (exact real-hardware behavior for a genuine 32-bit-
             * alpha target - this project's gs_mem is PSMCT32-only,
             * so PCSX2's documented "downgrade to FB_ONLY for non-
             * 32bit formats" special case never applies here). Only
             * reached when the alpha test actually failed - a passing
             * fragment always writes its own real alpha normally. */
            uint32_t existing = gs_mem_read_psmct32(g_gif.fbp, g_gif.fbw, (uint32_t)xx, (uint32_t)yy);
            out_a = rgba_channel(existing, 24);
        }

        gs_mem_write_psmct32(g_gif.fbp, g_gif.fbw, (uint32_t)xx, (uint32_t)yy, rgba_pack(out_r, out_g, out_b, out_a));
    }

    if (z_write) {
        gs_mem_write_psmct32(g_gif.zbp, g_gif.fbw, (uint32_t)xx, (uint32_t)yy, frag_z);
    }
}

/* Round 99 (140th finding, task #254 - GS gap follow-up 3/N): resolves
 * PRIM vs. PRMODE for the 8 mirrored drawing-attribute bits (IIP, TME,
 * FGE, ABE, AA1, FST, CTXT, FIX - bits 3-10), per PRMODECONT.AC (see
 * gif.h's GS_REG_PRMODECONT/GS_REG_PRMODE comment for the full manual
 * citation and bit layout). PRIM's own bits 0-2 (primitive TYPE) are
 * always taken from PRIM itself - PRMODE has no TYPE field at all, per
 * the manual - so this helper always returns PRIM's type bits
 * unchanged regardless of AC. Every attribute-bit read site in this
 * file (gs_activate_context's CTXT check, apply_fog's FGE check,
 * rasterize_*'s IIP/TME/FST/ABE checks) goes through this helper
 * instead of reading g_gif.prim directly, so PRMODECONT genuinely
 * takes effect everywhere real hardware says it should. Defaults to
 * AC=1 ("use PRIM", set in gif_init()) - the established safety-gate
 * convention, so this is a no-op for every pre-existing test/demo
 * unless it actually writes PRMODECONT. */
static uint32_t gs_effective_attr_prim(void)
{
    if (g_gif.prmodecont_ac) return g_gif.prim;
    return (g_gif.prim & 0x7u) | (g_gif.prmode & PRIM_ATTR_MASK);
}

/* Round 27: GS Context 2 (dual-context support) - see gif.h's
 * PRIM_CTXT_MASK/GS_REG_FRAME_2/etc and gif_state_t's ctx1_xxx/
 * ctx2_xxx field comments for the full design. Called once at the
 * very top of each of the 4 rasterizers, right before a primitive is
 * actually drawn - refreshes the flat "active" fields (which
 * gs_finish_pixel()/gs_sample_texel()/gs_sample_clut() and the
 * rasterizers themselves keep reading completely unchanged from
 * before this round) from whichever of ctx1_xxx/ctx2_xxx PRIM's CTXT
 * bit (bit 9) currently selects (Round 99: routed through
 * gs_effective_attr_prim() so PRMODECONT.AC=0 correctly sources CTXT
 * from PRMODE instead, per the manual's own mirrored-bit list). For
 * CTXT=0 this simply re-copies context 1's own already-correct values
 * back (idempotent, matching exactly what apply_ad_write's _1 cases
 * already wrote directly) - so existing single-context callers/tests
 * see zero behavioral change. For CTXT=1, context 2's permanent
 * storage becomes "live" in the active fields for the duration of
 * this primitive's rasterization. */
static void gs_activate_context(void)
{
    if (gs_effective_attr_prim() & PRIM_CTXT_MASK) {
        g_gif.fbp = g_gif.ctx2_fbp;
        g_gif.fbw = g_gif.ctx2_fbw;
        g_gif.xyoffset_x = g_gif.ctx2_xyoffset_x;
        g_gif.xyoffset_y = g_gif.ctx2_xyoffset_y;
        g_gif.tex_tbp0 = g_gif.ctx2_tex_tbp0;
        g_gif.tex_tbw = g_gif.ctx2_tex_tbw;
        g_gif.tex_tfx = g_gif.ctx2_tex_tfx;
        g_gif.tex_tw = g_gif.ctx2_tex_tw;
        g_gif.tex_th = g_gif.ctx2_tex_th;
        g_gif.tex_psm = g_gif.ctx2_tex_psm;
        g_gif.tex_cbp = g_gif.ctx2_tex_cbp;
        g_gif.tex_cpsm = g_gif.ctx2_tex_cpsm;
        g_gif.tex_csa = g_gif.ctx2_tex_csa;
        g_gif.tex_cld = g_gif.ctx2_tex_cld;
        g_gif.zbp = g_gif.ctx2_zbp;
        g_gif.zmsk = g_gif.ctx2_zmsk;
        g_gif.zbuf_configured = g_gif.ctx2_zbuf_configured;
        g_gif.zte = g_gif.ctx2_zte;
        g_gif.ztst = g_gif.ctx2_ztst;
        g_gif.ate = g_gif.ctx2_ate;
        g_gif.atst = g_gif.ctx2_atst;
        g_gif.aref = g_gif.ctx2_aref;
        g_gif.afail = g_gif.ctx2_afail;
        g_gif.alpha_a = g_gif.ctx2_alpha_a;
        g_gif.alpha_b = g_gif.ctx2_alpha_b;
        g_gif.alpha_c = g_gif.ctx2_alpha_c;
        g_gif.alpha_d = g_gif.ctx2_alpha_d;
        g_gif.alpha_fix = g_gif.ctx2_alpha_fix;
        g_gif.scissor_x0 = g_gif.ctx2_scissor_x0;
        g_gif.scissor_x1 = g_gif.ctx2_scissor_x1;
        g_gif.scissor_y0 = g_gif.ctx2_scissor_y0;
        g_gif.scissor_y1 = g_gif.ctx2_scissor_y1;
        g_gif.scissor_configured = g_gif.ctx2_scissor_configured;
        g_gif.tex1_lcm = g_gif.ctx2_tex1_lcm;
        g_gif.tex1_mxl = g_gif.ctx2_tex1_mxl;
        g_gif.tex1_mmag = g_gif.ctx2_tex1_mmag;
        g_gif.tex1_mmin = g_gif.ctx2_tex1_mmin;
        g_gif.tex1_mtba = g_gif.ctx2_tex1_mtba;
        g_gif.tex1_l = g_gif.ctx2_tex1_l;
        g_gif.tex1_k = g_gif.ctx2_tex1_k;
        for (int lvl = 0; lvl < 6; lvl++) {
            g_gif.tex_mip_tbp[lvl] = g_gif.ctx2_tex_mip_tbp[lvl];
            g_gif.tex_mip_tbw[lvl] = g_gif.ctx2_tex_mip_tbw[lvl];
        }
        g_gif.clamp_wms = g_gif.ctx2_clamp_wms;
        g_gif.clamp_wmt = g_gif.ctx2_clamp_wmt;
        g_gif.clamp_minu = g_gif.ctx2_clamp_minu;
        g_gif.clamp_maxu = g_gif.ctx2_clamp_maxu;
        g_gif.clamp_minv = g_gif.ctx2_clamp_minv;
        g_gif.clamp_maxv = g_gif.ctx2_clamp_maxv;
        g_gif.clamp_configured = g_gif.ctx2_clamp_configured;
    } else {
        g_gif.fbp = g_gif.ctx1_fbp;
        g_gif.fbw = g_gif.ctx1_fbw;
        g_gif.xyoffset_x = g_gif.ctx1_xyoffset_x;
        g_gif.xyoffset_y = g_gif.ctx1_xyoffset_y;
        g_gif.tex_tbp0 = g_gif.ctx1_tex_tbp0;
        g_gif.tex_tbw = g_gif.ctx1_tex_tbw;
        g_gif.tex_tfx = g_gif.ctx1_tex_tfx;
        g_gif.tex_tw = g_gif.ctx1_tex_tw;
        g_gif.tex_th = g_gif.ctx1_tex_th;
        g_gif.tex_psm = g_gif.ctx1_tex_psm;
        g_gif.tex_cbp = g_gif.ctx1_tex_cbp;
        g_gif.tex_cpsm = g_gif.ctx1_tex_cpsm;
        g_gif.tex_csa = g_gif.ctx1_tex_csa;
        g_gif.tex_cld = g_gif.ctx1_tex_cld;
        g_gif.zbp = g_gif.ctx1_zbp;
        g_gif.zmsk = g_gif.ctx1_zmsk;
        g_gif.zbuf_configured = g_gif.ctx1_zbuf_configured;
        g_gif.zte = g_gif.ctx1_zte;
        g_gif.ztst = g_gif.ctx1_ztst;
        g_gif.ate = g_gif.ctx1_ate;
        g_gif.atst = g_gif.ctx1_atst;
        g_gif.aref = g_gif.ctx1_aref;
        g_gif.afail = g_gif.ctx1_afail;
        g_gif.alpha_a = g_gif.ctx1_alpha_a;
        g_gif.alpha_b = g_gif.ctx1_alpha_b;
        g_gif.alpha_c = g_gif.ctx1_alpha_c;
        g_gif.alpha_d = g_gif.ctx1_alpha_d;
        g_gif.alpha_fix = g_gif.ctx1_alpha_fix;
        g_gif.scissor_x0 = g_gif.ctx1_scissor_x0;
        g_gif.scissor_x1 = g_gif.ctx1_scissor_x1;
        g_gif.scissor_y0 = g_gif.ctx1_scissor_y0;
        g_gif.scissor_y1 = g_gif.ctx1_scissor_y1;
        g_gif.scissor_configured = g_gif.ctx1_scissor_configured;
        g_gif.tex1_lcm = g_gif.ctx1_tex1_lcm;
        g_gif.tex1_mxl = g_gif.ctx1_tex1_mxl;
        g_gif.tex1_mmag = g_gif.ctx1_tex1_mmag;
        g_gif.tex1_mmin = g_gif.ctx1_tex1_mmin;
        g_gif.tex1_mtba = g_gif.ctx1_tex1_mtba;
        g_gif.tex1_l = g_gif.ctx1_tex1_l;
        g_gif.tex1_k = g_gif.ctx1_tex1_k;
        for (int lvl = 0; lvl < 6; lvl++) {
            g_gif.tex_mip_tbp[lvl] = g_gif.ctx1_tex_mip_tbp[lvl];
            g_gif.tex_mip_tbw[lvl] = g_gif.ctx1_tex_mip_tbw[lvl];
        }
        g_gif.clamp_wms = g_gif.ctx1_clamp_wms;
        g_gif.clamp_wmt = g_gif.ctx1_clamp_wmt;
        g_gif.clamp_minu = g_gif.ctx1_clamp_minu;
        g_gif.clamp_maxu = g_gif.ctx1_clamp_maxu;
        g_gif.clamp_minv = g_gif.ctx1_clamp_minv;
        g_gif.clamp_maxv = g_gif.ctx1_clamp_maxv;
        g_gif.clamp_configured = g_gif.ctx1_clamp_configured;
    }
}

/* Round 96: real SCISSOR clipping (GS Users Manual "SCISSOR_1/2:
 * Setting for Scissoring Area") - previously entirely unmodeled (see
 * the Round 28 comment on GS_REG_TEX1_1 above). Applied uniformly
 * across all 4 rasterizers below: triangle/sprite clamp their
 * bounding box directly (cheaper, and those two already computed a
 * bounding box); point/line test each individual pixel, since they
 * don't have a bounding box to clamp. scissor_configured follows the
 * same "safety gate" pattern as zbuf_configured - if no real SCISSOR
 * write has ever happened, this is a no-op and every pre-existing
 * test/demo keeps drawing exactly as before this round. */
static int scissor_test(int32_t x, int32_t y)
{
    if (!g_gif.scissor_configured) return 1;
    if (x < (int32_t)g_gif.scissor_x0 || x > (int32_t)g_gif.scissor_x1) return 0;
    if (y < (int32_t)g_gif.scissor_y0 || y > (int32_t)g_gif.scissor_y1) return 0;
    return 1;
}

static void clamp_bbox_to_scissor(int32_t *minx, int32_t *maxx, int32_t *miny, int32_t *maxy, int exclusive_max)
{
    if (!g_gif.scissor_configured) return;
    /* SCISSOR's SCAX1/SCAY1 are real, documented INCLUSIVE bounds
     * (GS Users Manual). rasterize_triangle()'s own bbox loop is
     * already inclusive on both ends (`xx <= maxx`), so its maxx/maxy
     * compare directly against SCAX1/SCAY1. rasterize_sprite()'s own
     * loop is EXCLUSIVE on the high edge (`xx < sx1`, matching this
     * project's established SPRITE X1/Y1 convention - see
     * rasterize_sprite()'s own header comment) - exclusive_max=1
     * shifts the comparison by one so an inclusive real SCISSOR bound
     * still allows exactly that last real pixel to be drawn under an
     * exclusive loop. */
    int32_t sx0 = (int32_t)g_gif.scissor_x0, sx1 = (int32_t)g_gif.scissor_x1;
    int32_t sy0 = (int32_t)g_gif.scissor_y0, sy1 = (int32_t)g_gif.scissor_y1;
    if (exclusive_max) { sx1 += 1; sy1 += 1; }
    if (*minx < sx0) *minx = sx0;
    if (*maxx > sx1) *maxx = sx1;
    if (*miny < sy0) *miny = sy0;
    if (*maxy > sy1) *maxy = sy1;
}

/* Round 97 (138th finding, task #254): the Fog effect - blends a
 * fragment's shaded/textured color toward FOGCOL using a per-pixel
 * Fog coefficient F (0-255), per the official GS Users Manual's
 * "3.5. Fog Effect":
 *   R = F*Rv + (0xff-F)*Rfc   (and likewise G, B; alpha is untouched)
 * The manual's own footnote on this formula ("A*B = (AxB)>>8")
 * establishes the real fixed-point convention: divide by 256, not
 * 255 - confirmed self-consistent with the F=255/F=0 boundary
 * descriptions ("255 = fog effect at minimum" implies output should
 * equal the vertex color almost exactly, which >>8 gives - 255/256
 * of the original value - while a plain /255 division would not
 * introduce this same, deliberately-matching, real-hardware
 * rounding). Gated by PRIM's real FGE bit (Round 97's PRIM_FGE_MASK) -
 * "Whether or not Fogging is performed is specified with the FGE flag
 * of the PRIM register", per the manual - so this is a genuine no-op
 * for every pre-existing test/demo, none of which ever set FGE. */
static uint32_t apply_fog(uint32_t color, uint32_t fog)
{
    if (!(gs_effective_attr_prim() & PRIM_FGE_MASK)) return color;
    uint32_t r = rgba_channel(color, 0), g = rgba_channel(color, 8), b = rgba_channel(color, 16), a = rgba_channel(color, 24);
    uint32_t fcr = rgba_channel(g_gif.fogcol, 0), fcg = rgba_channel(g_gif.fogcol, 8), fcb = rgba_channel(g_gif.fogcol, 16);
    uint32_t inv = 255u - fog;
    uint32_t nr = ((fog * r) + (inv * fcr)) >> 8;
    uint32_t ng = ((fog * g) + (inv * fcg)) >> 8;
    uint32_t nb = ((fog * b) + (inv * fcb)) >> 8;
    nr = gs_colclamp_channel((int32_t)nr);
    ng = gs_colclamp_channel((int32_t)ng);
    nb = gs_colclamp_channel((int32_t)nb);
    return rgba_pack(nr, ng, nb, a);
}

/* Round 98 (139th finding, task #254): CLAMP_1/2 real texture wrap
 * mode - per the official GS Users Manual's own FIELD table:
 *   REPEAT (00): coord & (size-1) - real hardware requires power-of-2
 *     texture sizes (matches this project's existing log2 tex_tw/th
 *     fields), so a bitmask is the exact real wrap operation.
 *   CLAMP (01): coord clamped to [0, size-1].
 *   REGION_CLAMP (10): coord clamped to the explicit [minv,maxv]
 *     sub-rectangle (minv/maxv here already resolved to whichever of
 *     MINU/MAXU or MINV/MAXV the caller passed - see call sites).
 *   REGION_REPEAT (11): coord = (coord & minv) | maxv - a masked-
 *     repeat (minv/maxv here are really UMSK/UFIX or VMSK/VFIX in
 *     this mode, per the manual's own field-reuse note).
 * Gated by clamp_configured (this project's established safety-gate
 * convention) - when CLAMP_1/2 was never written, this is a pure
 * pass-through, so every pre-existing texture test/demo keeps
 * sampling exactly as before this round. */
static int32_t gs_apply_clamp_wrap(int32_t coord, uint32_t size_log2, uint32_t wm, uint32_t minv, uint32_t maxv)
{
    if (!g_gif.clamp_configured) return coord;
    int32_t size = (int32_t)(1u << size_log2);
    switch (wm) {
    case GS_CLAMP_REPEAT:
        return coord & (size - 1);
    case GS_CLAMP_CLAMP:
        if (coord < 0) return 0;
        if (coord > size - 1) return size - 1;
        return coord;
    case GS_CLAMP_REGION_CLAMP:
        if (coord < (int32_t)minv) return (int32_t)minv;
        if (coord > (int32_t)maxv) return (int32_t)maxv;
        return coord;
    case GS_CLAMP_REGION_REPEAT:
        return (coord & (int32_t)minv) | (int32_t)maxv;
    default:
        return coord;
    }
}

static void rasterize_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                uint32_t c0, uint32_t c1, uint32_t c2,
                                int32_t u0, int32_t v0, int32_t u1, int32_t v1, int32_t u2, int32_t v2,
                                float s0, float t0, float q0, float s1, float t1, float q1, float s2, float t2, float q2,
                                uint32_t z0, uint32_t z1, uint32_t z2,
                                uint32_t f0, uint32_t f1, uint32_t f2)
{
    gs_activate_context(); /* Round 27: dual-context - see its own comment */
    int32_t minx = x0, maxx = x0, miny = y0, maxy = y0;
    if (x1 < minx) minx = x1;
    if (x1 > maxx) maxx = x1;
    if (x2 < minx) minx = x2;
    if (x2 > maxx) maxx = x2;
    if (y1 < miny) miny = y1;
    if (y1 > maxy) maxy = y1;
    if (y2 < miny) miny = y2;
    if (y2 > maxy) maxy = y2;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    clamp_bbox_to_scissor(&minx, &maxx, &miny, &maxy, 0); /* triangle: inclusive loop */

    /* Degenerate (zero-area) triangle - nothing to draw. Also guards
     * against divide-by-zero-shaped edge cases below. */
    int32_t area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;

    int gouraud = (gs_effective_attr_prim() & PRIM_IIP_MASK) != 0;
    int textured = (gs_effective_attr_prim() & PRIM_TME_MASK) != 0;
    /* Flat shading uses the LAST vertex's color on real hardware
     * (matches this rasterizer's pre-Gouraud behavior, which always
     * used whichever RGBAQ was active when the triangle completed -
     * i.e. exactly c2's value, since c2 is always the most-recently-
     * kicked vertex's color). */
    uint32_t flat_r = rgba_channel(c2, 0), flat_g = rgba_channel(c2, 8);
    uint32_t flat_b = rgba_channel(c2, 16), flat_a = rgba_channel(c2, 24);

    double inv_area = 1.0 / (double)area;

    /* Round 29 continued (14th change): extends Round 28's mipmap
     * support (previously SPRITE-only - see rasterize_sprite()'s own
     * comment for the full scope/citation and the honest limitations
     * this shares: per-PRIMITIVE not per-pixel LOD selection, no
     * trilinear blending, only MTBA=0 explicit-MIPTBP lookup) to
     * TRIANGLE. Uses the triangle's screen-space bounding box
     * (maxx-minx, maxy-miny) as the "screen size" input to the exact
     * same ratio-based LOD formula SPRITE already uses - the natural
     * analog of SPRITE's own (sx1-sx0, sy1-sy0) size, since a
     * triangle has no single well-defined width/height the way an
     * axis-aligned SPRITE rectangle does. Like SPRITE, the override
     * is scoped strictly to this one draw call and restored below. */
    uint32_t saved_mip_tbp0 = g_gif.tex_tbp0;
    uint32_t saved_mip_tbw = g_gif.tex_tbw;
    if (textured && g_gif.tex1_mxl > 0 && g_gif.tex1_mmin >= GS_MMIN_MIPMAP_THRESHOLD && !g_gif.tex1_mtba) {
        uint32_t lod = 0;
        if (g_gif.tex1_lcm) {
            double k = (double)g_gif.tex1_k / 16.0;
            lod = (k <= 0.0) ? 0u : (uint32_t)(k + 0.5);
        } else {
            int32_t screen_w = maxx - minx, screen_h = maxy - miny;
            double tex_w = (double)(1u << g_gif.tex_tw);
            double tex_h = (double)(1u << g_gif.tex_th);
            double ratio_w = (screen_w > 0) ? tex_w / (double)screen_w : 1.0;
            double ratio_h = (screen_h > 0) ? tex_h / (double)screen_h : 1.0;
            double ratio = (ratio_w > ratio_h) ? ratio_w : ratio_h;
            if (ratio > 1.0) {
                double lod_f = log2(ratio);
                lod = (lod_f < 0.0) ? 0u : (uint32_t)lod_f;
            }
        }
        if (lod > g_gif.tex1_mxl) lod = g_gif.tex1_mxl;
        if (lod > 0) {
            g_gif.tex_tbp0 = g_gif.tex_mip_tbp[lod - 1];
            g_gif.tex_tbw = g_gif.tex_mip_tbw[lod - 1];
        }
    }

    for (int32_t yy = miny; yy <= maxy; yy++) {
        for (int32_t xx = minx; xx <= maxx; xx++) {
            int32_t w0 = edge(x1, y1, x2, y2, xx, yy);
            int32_t w1 = edge(x2, y2, x0, y0, xx, yy);
            int32_t w2 = edge(x0, y0, x1, y1, xx, yy);
            /* Inside the triangle if all 3 edge signs match the
             * overall winding (area's sign) - works for either
             * winding direction, since GIF vertex order isn't
             * guaranteed consistent. */
            if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) ||
                (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                /* Barycentric weights (w0 corresponds to the vertex
                 * OPPOSITE it, i.e. weights c0 by w0 etc - standard
                 * edge-function barycentric convention). Used for
                 * BOTH Gouraud color interpolation and (when TME is
                 * set) texture-coordinate interpolation - on real
                 * hardware texture coordinates always interpolate per
                 * pixel when texturing is on, independent of the IIP
                 * (color-shading) bit. Plain affine (screen-space)
                 * interpolation, NOT the real GS's perspective-
                 * corrected (1/Q) one - see gif.h's scope comment. */
                double b0 = (double)w0 * inv_area;
                double b1 = (double)w1 * inv_area;
                double b2 = (double)w2 * inv_area;

                /* Z-buffer / depth test (task #89). Z is genuinely
                 * screen-space-linear on real GS hardware (unlike
                 * S/T - Z has already gone through the perspective
                 * projection by the time it reaches the rasterizer,
                 * so it interpolates the same plain barycentric way
                 * as Gouraud color, no 1/Q correction needed - well-
                 * known real-hardware behavior, not specific to this
                 * project). Gated behind zbuf_configured (this
                 * project's own safety gate - see gif.h) so draws
                 * that never configured a Z buffer behave exactly as
                 * before this round. */
                double zf = b0 * (double)z0 + b1 * (double)z1 + b2 * (double)z2;
                uint32_t frag_z = (zf < 0.0) ? 0u : (uint32_t)(zf + 0.5);
                int z_pass = 1;
                if (g_gif.zbuf_configured && g_gif.zte) {
                    uint32_t stored_z = gs_mem_read_psmct32(g_gif.zbp, g_gif.fbw, (uint32_t)xx, (uint32_t)yy);
                    switch (g_gif.ztst) {
                    case GS_ZTST_NEVER:   z_pass = 0; break;
                    case GS_ZTST_ALWAYS:  z_pass = 1; break;
                    case GS_ZTST_GEQUAL:  z_pass = (frag_z >= stored_z); break;
                    case GS_ZTST_GREATER: z_pass = (frag_z >  stored_z); break;
                    default: z_pass = 1; break;
                    }
                }
                if (!z_pass) {
                    g_gif.pixels_ztest_failed++;
                    continue;
                }

                uint32_t shaded_r, shaded_g, shaded_b, shaded_a;
                if (!gouraud) {
                    shaded_r = flat_r; shaded_g = flat_g; shaded_b = flat_b; shaded_a = flat_a;
                } else {
                    uint32_t r = (uint32_t)(b0 * rgba_channel(c0, 0)  + b1 * rgba_channel(c1, 0)  + b2 * rgba_channel(c2, 0)  + 0.5);
                    uint32_t g = (uint32_t)(b0 * rgba_channel(c0, 8)  + b1 * rgba_channel(c1, 8)  + b2 * rgba_channel(c2, 8)  + 0.5);
                    uint32_t b = (uint32_t)(b0 * rgba_channel(c0, 16) + b1 * rgba_channel(c1, 16) + b2 * rgba_channel(c2, 16) + 0.5);
                    uint32_t a = (uint32_t)(b0 * rgba_channel(c0, 24) + b1 * rgba_channel(c1, 24) + b2 * rgba_channel(c2, 24) + 0.5);
                    r = gs_colclamp_channel((int32_t)r);
                    g = gs_colclamp_channel((int32_t)g);
                    b = gs_colclamp_channel((int32_t)b);
                    a = gs_colclamp_channel((int32_t)a);
                    shaded_r = r; shaded_g = g; shaded_b = b; shaded_a = a;
                }

                uint32_t out;
                if (!textured) {
                    out = rgba_pack(shaded_r, shaded_g, shaded_b, shaded_a);
                } else {
                    double tu, tv;
                    if (gs_effective_attr_prim() & PRIM_FST_MASK) {
                        /* FST=1 (UV mode): plain affine interpolation,
                         * exactly as before task #88 - UV is already
                         * in integer texel units. */
                        tu = b0 * (double)u0 + b1 * (double)u1 + b2 * (double)u2;
                        tv = b0 * (double)v0 + b1 * (double)v1 + b2 * (double)v2;
                    } else {
                        /* FST=0 (ST+Q mode, task #88): genuine
                         * perspective-correct interpolation - the
                         * standard algorithm real GS hardware uses.
                         * 1/Q and S/Q, T/Q (NOT S, T, Q themselves)
                         * are what's affine/linear in screen space;
                         * interpolate those barycentrically, then
                         * recover the true per-pixel S/T by dividing
                         * back out the per-pixel Q. Guard against a
                         * degenerate Q of 0 (real hardware would
                         * produce a divide fault/undefined result
                         * too - clamped to a safe fallback here rather
                         * than crashing or reading garbage memory). */
                        double inv_q0 = (q0 != 0.0f) ? 1.0 / (double)q0 : 0.0;
                        double inv_q1 = (q1 != 0.0f) ? 1.0 / (double)q1 : 0.0;
                        double inv_q2 = (q2 != 0.0f) ? 1.0 / (double)q2 : 0.0;
                        double s_over_q0 = (double)s0 * inv_q0, s_over_q1 = (double)s1 * inv_q1, s_over_q2 = (double)s2 * inv_q2;
                        double t_over_q0 = (double)t0 * inv_q0, t_over_q1 = (double)t1 * inv_q1, t_over_q2 = (double)t2 * inv_q2;

                        double inv_q_interp = b0 * inv_q0 + b1 * inv_q1 + b2 * inv_q2;
                        double s_over_q_interp = b0 * s_over_q0 + b1 * s_over_q1 + b2 * s_over_q2;
                        double t_over_q_interp = b0 * t_over_q0 + b1 * t_over_q1 + b2 * t_over_q2;

                        double q_at_pixel = (inv_q_interp != 0.0) ? 1.0 / inv_q_interp : 0.0;
                        double s_norm = s_over_q_interp * q_at_pixel; /* normalized 0.0-1.0 texture-space S */
                        double t_norm = t_over_q_interp * q_at_pixel;

                        /* Scale normalized ST into texel space using
                         * TEX0's real TW/TH (log2 texture width/
                         * height) fields. */
                        tu = s_norm * (double)(1u << g_gif.tex_tw);
                        tv = t_norm * (double)(1u << g_gif.tex_th);
                    }
                    /* No CLAMP register modeling (wrap/clamp/region) -
                     * negative coordinates are simply clamped to 0, a
                     * defensive simplification, not real repeat/clamp
                     * semantics (see gif.h's scope comment). Out-of-
                     * range coordinates on the high side are left to
                     * gs_mem_read_psmct32()'s own bounds check, which
                     * safely returns 0 rather than reading garbage. */
                    int32_t tex_x = (tu < 0.0) ? 0 : (int32_t)(tu + 0.5);
                    int32_t tex_y = (tv < 0.0) ? 0 : (int32_t)(tv + 0.5);
                    /* Round 98 (139th finding): real CLAMP_1/2 wrap -
                     * see gs_apply_clamp_wrap()'s own comment. No-op
                     * (returns tex_x/tex_y unchanged) unless CLAMP_1/2
                     * was actually configured this draw. */
                    tex_x = gs_apply_clamp_wrap(tex_x, g_gif.tex_tw, g_gif.clamp_wms, g_gif.clamp_minu, g_gif.clamp_maxu);
                    tex_y = gs_apply_clamp_wrap(tex_y, g_gif.tex_th, g_gif.clamp_wmt, g_gif.clamp_minv, g_gif.clamp_maxv);
                    /* Round 24: routes through gs_sample_texel() so
                     * PSMT8/PSMT4 CLUT textures work here too - see
                     * its own comment for the full scope. */
                    uint32_t texel = gs_sample_texel(tex_x, tex_y);
                    if (g_gif.tex_tfx == TEX_TFX_DECAL) {
                        out = texel;
                    } else {
                        /* MODULATE (and, simplified, HIGHLIGHT/
                         * HIGHLIGHT2 too - see gif.h): standard GS
                         * modulate formula, (tex*color)/128 per
                         * channel, clamped to 255. */
                        uint32_t r = (rgba_channel(texel, 0)  * shaded_r) / 128u;
                        uint32_t g = (rgba_channel(texel, 8)  * shaded_g) / 128u;
                        uint32_t b = (rgba_channel(texel, 16) * shaded_b) / 128u;
                        uint32_t a = (rgba_channel(texel, 24) * shaded_a) / 128u;
                        r = gs_colclamp_channel((int32_t)r);
                        g = gs_colclamp_channel((int32_t)g);
                        b = gs_colclamp_channel((int32_t)b);
                        a = gs_colclamp_channel((int32_t)a);
                        out = rgba_pack(r, g, b, a);
                    }
                }
                /* Round 97 (138th finding, task #254): Fog effect -
                 * F interpolates the exact same plain-affine
                 * barycentric way as Z above (see tri_f's field
                 * comment in gif.h), applied here right before the
                 * final composite, same insertion point Round 96's
                 * SCISSOR clamp used for its own gate. */
                double ff = b0 * (double)f0 + b1 * (double)f1 + b2 * (double)f2;
                uint32_t frag_fog = (ff < 0.0) ? 0u : (ff > 255.0 ? 255u : (uint32_t)(ff + 0.5));
                out = apply_fog(out, frag_fog);
                /* ZMSK (real ZBUF register bit): 1 = Z writes
                 * disabled for this draw, matching real hardware
                 * exactly (color can still be written while Z stays
                 * untouched). Round 23: alpha test + blending now
                 * happen inside gs_finish_pixel() - see its comment
                 * above. */
                gs_finish_pixel(xx, yy, out, frag_z, g_gif.zbuf_configured && !g_gif.zmsk);
            }
        }
    }
    g_gif.triangles_drawn++;

    /* Round 29 continued (14th change): restore tex_tbp0/tex_tbw in
     * case a non-base mip level was temporarily selected above - keeps
     * the override strictly local to this single draw call, same as
     * rasterize_sprite()'s own restore. */
    g_gif.tex_tbp0 = saved_mip_tbp0;
    g_gif.tex_tbw = saved_mip_tbw;
}

/* SPRITE rasterizer (task #88 adds texturing here - previously
 * SPRITE was always flat-color only). Real hardware: SPRITE is a
 * filled, axis-aligned rectangle between 2 vertices, always flat-
 * shaded (no Gouraud) - but CAN be textured (PRIM's TME bit) just
 * like triangles.
 *
 * Texture-coordinate interpolation here is a deliberate, simpler
 * approximation than rasterize_triangle()'s full per-pixel
 * perspective correction: since SPRITE is screen-axis-aligned (U
 * varies only with X, V varies only with Y), each corner's texture
 * coordinate is resolved to final texel space FIRST (applying the
 * perspective divide at the corner, for FST=0/ST+Q mode), and then
 * plain linear interpolation is used between the two corners' already-
 * resolved texel coordinates. This is exact when Q is equal at both
 * corners (the overwhelmingly common real case for an axis-aligned
 * 2D sprite) but is a noted simplification - not silently assumed
 * correct - for the rarer case of a genuinely different Q per corner
 * (e.g. a "billboarded" sprite in true 3D perspective), where real
 * hardware would still interpolate more precisely. */
static void rasterize_sprite(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                              int32_t u0, int32_t v0, int32_t u1, int32_t v1,
                              float s0, float t0, float q0, float s1, float t1, float q1,
                              uint32_t z0, uint32_t z1, uint32_t f0, uint32_t f1)
{
    gs_activate_context(); /* Round 27: dual-context - see its own comment */
    int textured = (gs_effective_attr_prim() & PRIM_TME_MASK) != 0;
    /* Z (task #89): real hardware treats SPRITE Z the same way it
     * treats SPRITE color - a single flat value for the whole
     * primitive, taken from the second (completing) vertex,
     * mirroring this file's already-established "flat shading
     * uses the LAST vertex" convention (see rasterize_triangle()'s
     * flat_r/g/b/a). Unlike that color convention, this specific
     * SPRITE-Z-uses-second-vertex detail was not independently
     * re-verified against its own separate citation this round -
     * it is an extension of the same real, already-cited flat-
     * primitive convention, not a fresh guess. z0 is accepted for
     * signature symmetry with the other per-corner parameters but
     * is intentionally unused. */
    (void)z0;
    uint32_t frag_z = z1;
    /* Round 97 (138th finding, task #254): SPRITE Fog, same "flat,
     * second-vertex" convention this file already established for
     * SPRITE Z just above (real hardware does not interpolate either
     * attribute across a SPRITE the way it does for TRIANGLE - both
     * are simple flat per-primitive values here, f0 kept for
     * signature symmetry only, same as z0). */
    (void)f0;
    uint32_t frag_fog = f1;

    /* Resolve each corner's texture coordinate into texel space
     * BEFORE the min/max reordering below, so the X/Y->U/V
     * correspondence direction is preserved regardless of which
     * corner the caller gave first (real hardware allows either
     * vertex order). */
    double tex_u0 = 0.0, tex_v0 = 0.0, tex_u1 = 0.0, tex_v1 = 0.0;
    if (textured) {
        if (gs_effective_attr_prim() & PRIM_FST_MASK) {
            tex_u0 = (double)u0; tex_v0 = (double)v0;
            tex_u1 = (double)u1; tex_v1 = (double)v1;
        } else {
            double inv_q0 = (q0 != 0.0f) ? 1.0 / (double)q0 : 0.0;
            double inv_q1 = (q1 != 0.0f) ? 1.0 / (double)q1 : 0.0;
            tex_u0 = (double)s0 * inv_q0 * (double)(1u << g_gif.tex_tw);
            tex_v0 = (double)t0 * inv_q0 * (double)(1u << g_gif.tex_th);
            tex_u1 = (double)s1 * inv_q1 * (double)(1u << g_gif.tex_tw);
            tex_v1 = (double)t1 * inv_q1 * (double)(1u << g_gif.tex_th);
        }
    }

    int32_t sx0 = x0, sx1 = x1, sy0 = y0, sy1 = y1;
    if (sx1 < sx0) { int32_t t = sx0; sx0 = sx1; sx1 = t; }
    if (sy1 < sy0) { int32_t t = sy0; sy0 = sy1; sy1 = t; }
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    clamp_bbox_to_scissor(&sx0, &sx1, &sy0, &sy1, 1); /* sprite: exclusive loop */

    /* Round 28: mipmaps - SPRITE-only, per-PRIMITIVE (not per-pixel)
     * LOD selection. See gif.h's GS_REG_TEX1_1/MIPTBP1_1/MIPTBP2_1
     * comment and gif_state_t's tex1_xxx/tex_mip_tbp/tex_mip_tbw
     * field comments for the full scope: only MTBA=0 (explicit
     * MIPTBP lookup) is implemented; a single nearest mip level is
     * selected once for the WHOLE sprite (not varied per-pixel, and
     * not trilinear-blended between adjacent levels) - a deliberate,
     * honest simplification consistent with this project's existing
     * nearest-neighbor-only texture sampling (no bilinear/trilinear
     * filtering anywhere in this codebase). If a non-zero level is
     * selected, tex_tbp0/tex_tbw are temporarily overridden for the
     * duration of this draw and restored before returning - the same
     * save/override/restore spirit as gs_activate_context(), just
     * scoped to a single function instead of persisted in gif_state_t. */
    uint32_t saved_mip_tbp0 = g_gif.tex_tbp0;
    uint32_t saved_mip_tbw = g_gif.tex_tbw;
    if (textured && g_gif.tex1_mxl > 0 && g_gif.tex1_mmin >= GS_MMIN_MIPMAP_THRESHOLD && !g_gif.tex1_mtba) {
        uint32_t lod = 0;
        if (g_gif.tex1_lcm) {
            /* Fixed LOD from K (signed, 1/16 units) - L is parsed but
             * deliberately not applied this round (see tex1_l's field
             * comment). */
            double k = (double)g_gif.tex1_k / 16.0;
            lod = (k <= 0.0) ? 0u : (uint32_t)(k + 0.5);
        } else {
            /* Computed from the ratio of texture size to this
             * sprite's own screen-space size - minification only
             * (ratio > 1.0); magnification (ratio <= 1.0) always
             * uses level 0, matching real hardware's own "never
             * upsample past the base level" behavior. */
            int32_t screen_w = sx1 - sx0, screen_h = sy1 - sy0;
            double tex_w = (double)(1u << g_gif.tex_tw);
            double tex_h = (double)(1u << g_gif.tex_th);
            double ratio_w = (screen_w > 0) ? tex_w / (double)screen_w : 1.0;
            double ratio_h = (screen_h > 0) ? tex_h / (double)screen_h : 1.0;
            double ratio = (ratio_w > ratio_h) ? ratio_w : ratio_h;
            if (ratio > 1.0) {
                double lod_f = log2(ratio);
                lod = (lod_f < 0.0) ? 0u : (uint32_t)lod_f; /* floor, since lod_f >= 0 here */
            }
        }
        if (lod > g_gif.tex1_mxl) lod = g_gif.tex1_mxl;
        if (lod > 0) {
            g_gif.tex_tbp0 = g_gif.tex_mip_tbp[lod - 1];
            g_gif.tex_tbw = g_gif.tex_mip_tbw[lod - 1];
        }
    }

    double x_span = (double)(x1 - x0);
    double y_span = (double)(y1 - y0);

    for (int32_t yy = sy0; yy < sy1; yy++) {
        if (yy < 0) continue;
        double frac_y = (y_span != 0.0) ? ((double)yy - (double)y0) / y_span : 0.0;
        for (int32_t xx = sx0; xx < sx1; xx++) {
            if (xx < 0) continue;

            /* Z-buffer / depth test (task #89) - see
             * rasterize_triangle()'s identical logic for the full
             * comment; SPRITE just uses the flat frag_z computed
             * above instead of a per-pixel barycentric one. */
            int z_pass = 1;
            if (g_gif.zbuf_configured && g_gif.zte) {
                uint32_t stored_z = gs_mem_read_psmct32(g_gif.zbp, g_gif.fbw, (uint32_t)xx, (uint32_t)yy);
                switch (g_gif.ztst) {
                case GS_ZTST_NEVER:   z_pass = 0; break;
                case GS_ZTST_ALWAYS:  z_pass = 1; break;
                case GS_ZTST_GEQUAL:  z_pass = (frag_z >= stored_z); break;
                case GS_ZTST_GREATER: z_pass = (frag_z >  stored_z); break;
                default: z_pass = 1; break;
                }
            }
            if (!z_pass) {
                g_gif.pixels_ztest_failed++;
                continue;
            }

            uint32_t out;
            if (!textured) {
                out = g_gif.rgba;
            } else {
                double frac_x = (x_span != 0.0) ? ((double)xx - (double)x0) / x_span : 0.0;
                double tu = tex_u0 + frac_x * (tex_u1 - tex_u0);
                double tv = tex_v0 + frac_y * (tex_v1 - tex_v0);
                int32_t tex_x = (tu < 0.0) ? 0 : (int32_t)(tu + 0.5);
                int32_t tex_y = (tv < 0.0) ? 0 : (int32_t)(tv + 0.5);
                /* Round 98 (139th finding): real CLAMP_1/2 wrap - see
                 * gs_apply_clamp_wrap()'s own comment above. */
                tex_x = gs_apply_clamp_wrap(tex_x, g_gif.tex_tw, g_gif.clamp_wms, g_gif.clamp_minu, g_gif.clamp_maxu);
                tex_y = gs_apply_clamp_wrap(tex_y, g_gif.tex_th, g_gif.clamp_wmt, g_gif.clamp_minv, g_gif.clamp_maxv);
                /* Round 24: routes through gs_sample_texel() so
                 * PSMT8/PSMT4 CLUT textures work here too. */
                uint32_t texel = gs_sample_texel(tex_x, tex_y);
                if (g_gif.tex_tfx == TEX_TFX_DECAL) {
                    out = texel;
                } else {
                    uint32_t r = (rgba_channel(texel, 0)  * rgba_channel(g_gif.rgba, 0))  / 128u;
                    uint32_t g = (rgba_channel(texel, 8)  * rgba_channel(g_gif.rgba, 8))  / 128u;
                    uint32_t b = (rgba_channel(texel, 16) * rgba_channel(g_gif.rgba, 16)) / 128u;
                    uint32_t a = (rgba_channel(texel, 24) * rgba_channel(g_gif.rgba, 24)) / 128u;
                    r = gs_colclamp_channel((int32_t)r);
                    g = gs_colclamp_channel((int32_t)g);
                    b = gs_colclamp_channel((int32_t)b);
                    a = gs_colclamp_channel((int32_t)a);
                    out = rgba_pack(r, g, b, a);
                }
            }
            out = apply_fog(out, frag_fog);
            gs_finish_pixel(xx, yy, out, frag_z, g_gif.zbuf_configured && !g_gif.zmsk);
        }
    }
    g_gif.sprites_drawn++;

    /* Round 28: restore tex_tbp0/tex_tbw in case a mip level other
     * than the base was temporarily selected above - keeps this
     * override strictly local to this single draw call. */
    g_gif.tex_tbp0 = saved_mip_tbp0;
    g_gif.tex_tbw = saved_mip_tbw;
}

/* POINT rasterizer (task: "GS coverage breadth"). Real hardware
 * (PCSX2's `GSRasterizer::DrawPoint`): a single pixel, flat color
 * only (the point's own vertex - `CSetupPrim` selects `last=0` for
 * `GS_POINT_CLASS`, i.e. there's only ever one vertex to begin with,
 * no interpolation of any kind), gated behind the same Z test every
 * other primitive uses. */
static void rasterize_point(int32_t x, int32_t y, uint32_t rgba, uint32_t z, uint32_t fog)
{
    gs_activate_context(); /* Round 27: dual-context - see its own comment */
    if (x < 0 || y < 0) return;
    if (!scissor_test(x, y)) return;

    int z_pass = 1;
    if (g_gif.zbuf_configured && g_gif.zte) {
        uint32_t stored_z = gs_mem_read_psmct32(g_gif.zbp, g_gif.fbw, (uint32_t)x, (uint32_t)y);
        switch (g_gif.ztst) {
        case GS_ZTST_NEVER:   z_pass = 0; break;
        case GS_ZTST_ALWAYS:  z_pass = 1; break;
        case GS_ZTST_GEQUAL:  z_pass = (z >= stored_z); break;
        case GS_ZTST_GREATER: z_pass = (z >  stored_z); break;
        default: z_pass = 1; break;
        }
    }
    if (!z_pass) {
        g_gif.pixels_ztest_failed++;
        return;
    }

    /* Round 97 (138th finding, task #254): POINT Fog - a single
     * vertex, so no interpolation is possible or needed (same "no
     * interpolation of any kind" real-hardware rule this file already
     * documents for POINT's color). */
    gs_finish_pixel(x, y, apply_fog(rgba, fog), z, g_gif.zbuf_configured && !g_gif.zmsk);
    g_gif.points_drawn++;
}

/* LINE/LINE_STRIP rasterizer (task: "GS coverage breadth"). Ported
 * from PCSX2's real `GSRasterizer::DrawEdgeLine` DDA algorithm: walk
 * whichever axis has the larger absolute delta (the "major" axis) one
 * pixel at a time, and linearly step every interpolated attribute
 * (color, Z) by its total delta divided by the major-axis step count -
 * true per-pixel-step DDA, not a Bresenham integer-error accumulator
 * (real hardware's line rule is the same "step the dependent
 * coordinate/attributes linearly per major-axis pixel" shape; this
 * project's own simplification is using a plain float step instead of
 * PCSX2's fixed-point 16.16 subpixel accumulator - a well-known,
 * equivalent-result technique for a software rasterizer, not a
 * real-hardware-specific detail the way the flat/Gouraud-vertex-
 * selection and linear-Z rules are). Flat shading uses the LAST
 * vertex's color (c1) - cross-checked against PCSX2's
 * `GSDrawScanline::CSetupPrim`, which selects `last=1` for
 * `GS_LINE_CLASS` (the same "flat uses the last vertex" convention
 * already established for triangles/sprites in this file). */
static void rasterize_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            uint32_t c0, uint32_t c1, uint32_t z0, uint32_t z1,
                            uint32_t f0, uint32_t f1)
{
    gs_activate_context(); /* Round 27: dual-context - see its own comment */
    int gouraud = (gs_effective_attr_prim() & PRIM_IIP_MASK) != 0;
    uint32_t flat_r = rgba_channel(c1, 0), flat_g = rgba_channel(c1, 8);
    uint32_t flat_b = rgba_channel(c1, 16), flat_a = rgba_channel(c1, 24);

    int32_t dx = x1 - x0, dy = y1 - y0;
    int32_t adx = (dx < 0) ? -dx : dx;
    int32_t ady = (dy < 0) ? -dy : dy;

    if (adx == 0 && ady == 0) {
        /* Degenerate (both vertices coincide) - real hardware still
         * draws the single point (a 0-length line still emits its
         * one pixel); handled the same way DrawPoint would. */
        uint32_t z_avg = z1;
        rasterize_point(x0, y0, gouraud ? c1 : rgba_pack(flat_r, flat_g, flat_b, flat_a), z_avg, f1);
        g_gif.lines_drawn++;
        return;
    }

    int32_t steps = (adx >= ady) ? adx : ady;
    double step_x = (double)dx / (double)steps;
    double step_y = (double)dy / (double)steps;
    double step_z = ((double)z1 - (double)z0) / (double)steps;
    /* Round 97 (138th finding, task #254): Fog steps linearly along
     * the line exactly like Z above (same "already screen-space-
     * linear, no perspective correction needed" real-hardware rule -
     * see gif.h's tri_f field comment). */
    double step_fog = ((double)f1 - (double)f0) / (double)steps;
    double px = (double)x0, py = (double)y0, pz = (double)z0, pf = (double)f0;

    for (int32_t i = 0; i <= steps; i++) {
        int32_t xx = (int32_t)(px + 0.5);
        int32_t yy = (int32_t)(py + 0.5);
        uint32_t frag_z = (pz < 0.0) ? 0u : (uint32_t)(pz + 0.5);

        if (xx >= 0 && yy >= 0 && scissor_test(xx, yy)) {
            int z_pass = 1;
            if (g_gif.zbuf_configured && g_gif.zte) {
                uint32_t stored_z = gs_mem_read_psmct32(g_gif.zbp, g_gif.fbw, (uint32_t)xx, (uint32_t)yy);
                switch (g_gif.ztst) {
                case GS_ZTST_NEVER:   z_pass = 0; break;
                case GS_ZTST_ALWAYS:  z_pass = 1; break;
                case GS_ZTST_GEQUAL:  z_pass = (frag_z >= stored_z); break;
                case GS_ZTST_GREATER: z_pass = (frag_z >  stored_z); break;
                default: z_pass = 1; break;
                }
            }
            if (!z_pass) {
                g_gif.pixels_ztest_failed++;
            } else {
                uint32_t out;
                if (!gouraud) {
                    out = rgba_pack(flat_r, flat_g, flat_b, flat_a);
                } else {
                    double t = (double)i / (double)steps;
                    uint32_t r = (uint32_t)((1.0 - t) * rgba_channel(c0, 0)  + t * rgba_channel(c1, 0)  + 0.5);
                    uint32_t g = (uint32_t)((1.0 - t) * rgba_channel(c0, 8)  + t * rgba_channel(c1, 8)  + 0.5);
                    uint32_t b = (uint32_t)((1.0 - t) * rgba_channel(c0, 16) + t * rgba_channel(c1, 16) + 0.5);
                    uint32_t a = (uint32_t)((1.0 - t) * rgba_channel(c0, 24) + t * rgba_channel(c1, 24) + 0.5);
                    r = gs_colclamp_channel((int32_t)r);
                    g = gs_colclamp_channel((int32_t)g);
                    b = gs_colclamp_channel((int32_t)b);
                    a = gs_colclamp_channel((int32_t)a);
                    out = rgba_pack(r, g, b, a);
                }
                uint32_t frag_fog = (pf < 0.0) ? 0u : (pf > 255.0 ? 255u : (uint32_t)(pf + 0.5));
                out = apply_fog(out, frag_fog);
                gs_finish_pixel(xx, yy, out, frag_z, g_gif.zbuf_configured && !g_gif.zmsk);
            }
        }

        px += step_x; py += step_y; pz += step_z; pf += step_fog;
    }
    g_gif.lines_drawn++;
}

static void apply_xyz2_kick(uint32_t word0, uint32_t word1, uint32_t word2, int do_draw_kick)
{
    /* PACKED XYZ2 layout (GS/GSRegs.h-compatible bit positions): X in
     * bits 0-15 of word0, Y in bits 0-15 of word1 (both 12.4
     * fixed-point, offset-relative). Z (task #89) is the ENTIRE
     * word2 - a real, full 32-bit value, cross-checked against
     * PCSX2's GS/GSRegs.h GIFPackedXYZ2 layout. The ADC/context bit
     * (word3) still isn't used. IMPORTANT: word2 is only ever a real
     * Z value when apply_xyz2_kick() is reached via the genuine
     * PACKED-mode path - the A+D-mode call site passes 0 here instead
     * (see gif.h's tri_z field comment for why).
     *
     * Round 97 (138th finding, task #254): renamed from apply_xyz2()
     * and given a new do_draw_kick parameter so this same shared
     * vertex-kick logic can serve BOTH the original XYZ2/XYZF2 (kick
     * + draw when the vertex queue fills) and the new XYZ3/XYZF3
     * (kick only - "the Drawing Kick is not performed... only the
     * vertex queue is advanced", per the official GS Users Manual's
     * XYZ3 description). do_draw_kick=0 lets every vseq-increment/
     * rolling-window bookkeeping line below run exactly as before -
     * it only gates the terminal rasterize_*() calls. The vertex's
     * Fog coefficient (also new this round) is read from cur_fog,
     * which callers set immediately before invoking this function
     * when the register being processed carries its own F field
     * (XYZF2/XYZF3 in PACKED mode) - see process_one_packet()'s new
     * GIF_REG_XYZF2/XYZF3 cases - or simply left as whatever a prior
     * standalone FOG-register write (or the 0xFF default) already
     * set, for plain XYZ2/XYZ3/XYZF2-via-A+D. */
    int32_t raw_x = (int32_t)(word0 & 0xFFFFu);
    int32_t raw_y = (int32_t)(word1 & 0xFFFFu);
    uint32_t raw_z = word2;

    int32_t x = (raw_x - (int32_t)g_gif.xyoffset_x) >> 4;
    int32_t y = (raw_y - (int32_t)g_gif.xyoffset_y) >> 4;

    uint32_t ptype = g_gif.prim & 0x7u;

    if (ptype == PRIM_TYPE_POINT) {
        /* POINT: draws immediately on every single vertex - no
         * accumulation needed (real hardware: NumIndicesForPrim
         * returns 1 for POINTLIST, each incoming vertex is a
         * complete primitive on its own). XYZ3/XYZF3 (do_draw_kick=0)
         * on a POINT primitive means "advance the queue but don't
         * draw" - since POINT has no queue depth beyond the current
         * vertex, this simply means: don't draw this vertex at all. */
        if (do_draw_kick)
            rasterize_point(x, y, g_gif.rgba, raw_z, g_gif.cur_fog);
        return;
    }

    if (ptype == PRIM_TYPE_LINE || ptype == PRIM_TYPE_LINE_STRIP) {
        g_gif.line_vseq++;

        if (ptype == PRIM_TYPE_LINE) {
            /* Plain LINE: every pair of vertices is an independent
             * segment - no reuse across segments (matches real
             * hardware's NumIndicesForPrim==2 for LINELIST, same "no
             * carry-over" shape as this file's plain TRIANGLE case). */
            int slot = (g_gif.line_vseq - 1) % 2;
            g_gif.line_x[slot] = x;
            g_gif.line_y[slot] = y;
            g_gif.line_rgba[slot] = g_gif.rgba;
            g_gif.line_z[slot] = raw_z;
            g_gif.line_f[slot] = g_gif.cur_fog;
            if (do_draw_kick && g_gif.line_vseq % 2 == 0)
                rasterize_line(g_gif.line_x[0], g_gif.line_y[0], g_gif.line_x[1], g_gif.line_y[1],
                                g_gif.line_rgba[0], g_gif.line_rgba[1],
                                g_gif.line_z[0], g_gif.line_z[1],
                                g_gif.line_f[0], g_gif.line_f[1]);
        } else { /* PRIM_TYPE_LINE_STRIP */
            /* LINE_STRIP: each new vertex (from the 2nd onward) forms
             * a segment with the previous one - a rolling 2-slot
             * window, same shape as this file's TRIANGLE_STRIP
             * handling above, just 2 slots instead of 3. The rolling
             * shift always happens (real hardware: the vertex queue
             * always advances on every vertex kick, draw or not);
             * only the rasterize_line() call itself is gated. */
            g_gif.line_x[0] = g_gif.line_x[1]; g_gif.line_y[0] = g_gif.line_y[1];
            g_gif.line_rgba[0] = g_gif.line_rgba[1]; g_gif.line_z[0] = g_gif.line_z[1];
            g_gif.line_f[0] = g_gif.line_f[1];
            g_gif.line_x[1] = x; g_gif.line_y[1] = y;
            g_gif.line_rgba[1] = g_gif.rgba; g_gif.line_z[1] = raw_z;
            g_gif.line_f[1] = g_gif.cur_fog;
            if (do_draw_kick && g_gif.line_vseq >= 2)
                rasterize_line(g_gif.line_x[0], g_gif.line_y[0], g_gif.line_x[1], g_gif.line_y[1],
                                g_gif.line_rgba[0], g_gif.line_rgba[1],
                                g_gif.line_z[0], g_gif.line_z[1],
                                g_gif.line_f[0], g_gif.line_f[1]);
        }
        return;
    }

    if (ptype == PRIM_TYPE_TRIANGLE || ptype == PRIM_TYPE_TRIANGLE_STRIP || ptype == PRIM_TYPE_TRIANGLE_FAN) {
        g_gif.tri_vseq++;

        if (ptype == PRIM_TYPE_TRIANGLE) {
            /* Plain TRIANGLE: every group of 3 vertices is
             * independent - no reuse across triangles. */
            int slot = (g_gif.tri_vseq - 1) % 3;
            g_gif.tri_x[slot] = x;
            g_gif.tri_y[slot] = y;
            g_gif.tri_rgba[slot] = g_gif.rgba;
            g_gif.tri_u[slot] = g_gif.cur_u;
            g_gif.tri_v[slot] = g_gif.cur_v;
            g_gif.tri_s[slot] = g_gif.cur_s;
            g_gif.tri_t[slot] = g_gif.cur_t;
            g_gif.tri_q[slot] = g_gif.cur_q;
            g_gif.tri_z[slot] = raw_z;
            g_gif.tri_f[slot] = g_gif.cur_fog;
            if (do_draw_kick && g_gif.tri_vseq % 3 == 0)
                rasterize_triangle(g_gif.tri_x[0], g_gif.tri_y[0], g_gif.tri_x[1], g_gif.tri_y[1], g_gif.tri_x[2], g_gif.tri_y[2],
                                    g_gif.tri_rgba[0], g_gif.tri_rgba[1], g_gif.tri_rgba[2],
                                    g_gif.tri_u[0], g_gif.tri_v[0], g_gif.tri_u[1], g_gif.tri_v[1], g_gif.tri_u[2], g_gif.tri_v[2],
                                    g_gif.tri_s[0], g_gif.tri_t[0], g_gif.tri_q[0],
                                    g_gif.tri_s[1], g_gif.tri_t[1], g_gif.tri_q[1],
                                    g_gif.tri_s[2], g_gif.tri_t[2], g_gif.tri_q[2],
                                    g_gif.tri_z[0], g_gif.tri_z[1], g_gif.tri_z[2],
                                    g_gif.tri_f[0], g_gif.tri_f[1], g_gif.tri_f[2]);
        } else if (ptype == PRIM_TYPE_TRIANGLE_STRIP) {
            /* TRIANGLE_STRIP: each new vertex (from the 3rd onward)
             * forms a triangle with the previous 2 - a rolling
             * 3-slot window. The rolling shift always happens; only
             * the rasterize_triangle() call itself is gated by
             * do_draw_kick (same "queue advances regardless" rule as
             * LINE_STRIP above). */
            g_gif.tri_x[0] = g_gif.tri_x[1]; g_gif.tri_y[0] = g_gif.tri_y[1]; g_gif.tri_rgba[0] = g_gif.tri_rgba[1];
            g_gif.tri_u[0] = g_gif.tri_u[1]; g_gif.tri_v[0] = g_gif.tri_v[1];
            g_gif.tri_s[0] = g_gif.tri_s[1]; g_gif.tri_t[0] = g_gif.tri_t[1]; g_gif.tri_q[0] = g_gif.tri_q[1];
            g_gif.tri_z[0] = g_gif.tri_z[1];
            g_gif.tri_f[0] = g_gif.tri_f[1];
            g_gif.tri_x[1] = g_gif.tri_x[2]; g_gif.tri_y[1] = g_gif.tri_y[2]; g_gif.tri_rgba[1] = g_gif.tri_rgba[2];
            g_gif.tri_u[1] = g_gif.tri_u[2]; g_gif.tri_v[1] = g_gif.tri_v[2];
            g_gif.tri_s[1] = g_gif.tri_s[2]; g_gif.tri_t[1] = g_gif.tri_t[2]; g_gif.tri_q[1] = g_gif.tri_q[2];
            g_gif.tri_z[1] = g_gif.tri_z[2];
            g_gif.tri_f[1] = g_gif.tri_f[2];
            g_gif.tri_x[2] = x; g_gif.tri_y[2] = y; g_gif.tri_rgba[2] = g_gif.rgba;
            g_gif.tri_u[2] = g_gif.cur_u; g_gif.tri_v[2] = g_gif.cur_v;
            g_gif.tri_s[2] = g_gif.cur_s; g_gif.tri_t[2] = g_gif.cur_t; g_gif.tri_q[2] = g_gif.cur_q;
            g_gif.tri_z[2] = raw_z;
            g_gif.tri_f[2] = g_gif.cur_fog;
            if (do_draw_kick && g_gif.tri_vseq >= 3)
                rasterize_triangle(g_gif.tri_x[0], g_gif.tri_y[0], g_gif.tri_x[1], g_gif.tri_y[1], g_gif.tri_x[2], g_gif.tri_y[2],
                                    g_gif.tri_rgba[0], g_gif.tri_rgba[1], g_gif.tri_rgba[2],
                                    g_gif.tri_u[0], g_gif.tri_v[0], g_gif.tri_u[1], g_gif.tri_v[1], g_gif.tri_u[2], g_gif.tri_v[2],
                                    g_gif.tri_s[0], g_gif.tri_t[0], g_gif.tri_q[0],
                                    g_gif.tri_s[1], g_gif.tri_t[1], g_gif.tri_q[1],
                                    g_gif.tri_s[2], g_gif.tri_t[2], g_gif.tri_q[2],
                                    g_gif.tri_z[0], g_gif.tri_z[1], g_gif.tri_z[2],
                                    g_gif.tri_f[0], g_gif.tri_f[1], g_gif.tri_f[2]);
        } else { /* PRIM_TYPE_TRIANGLE_FAN */
            /* TRIANGLE_FAN: the first vertex is a fixed anchor
             * (slot 0, never overwritten); each new vertex forms a
             * triangle with the anchor and the previous vertex. */
            if (g_gif.tri_vseq == 1) {
                g_gif.tri_x[0] = x; g_gif.tri_y[0] = y; g_gif.tri_rgba[0] = g_gif.rgba;
                g_gif.tri_u[0] = g_gif.cur_u; g_gif.tri_v[0] = g_gif.cur_v;
                g_gif.tri_s[0] = g_gif.cur_s; g_gif.tri_t[0] = g_gif.cur_t; g_gif.tri_q[0] = g_gif.cur_q;
                g_gif.tri_z[0] = raw_z;
                g_gif.tri_f[0] = g_gif.cur_fog;
                g_gif.tri_x[1] = x; g_gif.tri_y[1] = y; g_gif.tri_rgba[1] = g_gif.rgba; /* also seed "previous" so vseq==2 has something to pair with */
                g_gif.tri_u[1] = g_gif.cur_u; g_gif.tri_v[1] = g_gif.cur_v;
                g_gif.tri_s[1] = g_gif.cur_s; g_gif.tri_t[1] = g_gif.cur_t; g_gif.tri_q[1] = g_gif.cur_q;
                g_gif.tri_z[1] = raw_z;
                g_gif.tri_f[1] = g_gif.cur_fog;
            } else {
                g_gif.tri_x[2] = x; g_gif.tri_y[2] = y; g_gif.tri_rgba[2] = g_gif.rgba;
                g_gif.tri_u[2] = g_gif.cur_u; g_gif.tri_v[2] = g_gif.cur_v;
                g_gif.tri_s[2] = g_gif.cur_s; g_gif.tri_t[2] = g_gif.cur_t; g_gif.tri_q[2] = g_gif.cur_q;
                g_gif.tri_z[2] = raw_z;
                g_gif.tri_f[2] = g_gif.cur_fog;
                if (do_draw_kick && g_gif.tri_vseq >= 3)
                    rasterize_triangle(g_gif.tri_x[0], g_gif.tri_y[0], g_gif.tri_x[1], g_gif.tri_y[1], g_gif.tri_x[2], g_gif.tri_y[2],
                                        g_gif.tri_rgba[0], g_gif.tri_rgba[1], g_gif.tri_rgba[2],
                                        g_gif.tri_u[0], g_gif.tri_v[0], g_gif.tri_u[1], g_gif.tri_v[1], g_gif.tri_u[2], g_gif.tri_v[2],
                                        g_gif.tri_s[0], g_gif.tri_t[0], g_gif.tri_q[0],
                                        g_gif.tri_s[1], g_gif.tri_t[1], g_gif.tri_q[1],
                                        g_gif.tri_s[2], g_gif.tri_t[2], g_gif.tri_q[2],
                                        g_gif.tri_z[0], g_gif.tri_z[1], g_gif.tri_z[2],
                                        g_gif.tri_f[0], g_gif.tri_f[1], g_gif.tri_f[2]);
                g_gif.tri_x[1] = g_gif.tri_x[2]; g_gif.tri_y[1] = g_gif.tri_y[2]; g_gif.tri_rgba[1] = g_gif.tri_rgba[2];
                g_gif.tri_u[1] = g_gif.tri_u[2]; g_gif.tri_v[1] = g_gif.tri_v[2];
                g_gif.tri_s[1] = g_gif.tri_s[2]; g_gif.tri_t[1] = g_gif.tri_t[2]; g_gif.tri_q[1] = g_gif.tri_q[2];
                g_gif.tri_z[1] = g_gif.tri_z[2];
                g_gif.tri_f[1] = g_gif.tri_f[2];
            }
        }
        return;
    }

    if (!g_gif.has_vertex0) {
        g_gif.v0x = x;
        g_gif.v0y = y;
        g_gif.v0u = g_gif.cur_u;
        g_gif.v0v = g_gif.cur_v;
        g_gif.v0s = g_gif.cur_s;
        g_gif.v0t = g_gif.cur_t;
        g_gif.v0q = g_gif.cur_q;
        g_gif.v0z = raw_z;
        g_gif.v0f = g_gif.cur_fog;
        g_gif.has_vertex0 = 1;
        return;
    }

    /* Second vertex: if we're drawing a SPRITE, fill the rectangle
     * between v0 and this vertex now (unless do_draw_kick=0 - the
     * GS Users Manual's XYZ3/XYZF3 "no Drawing Kick" example is a
     * TRIANGLE_STRIP, but the same rule applies here for
     * consistency: the vertex queue is still consumed/advanced,
     * just without triggering the actual fill). */
    if (ptype == PRIM_TYPE_SPRITE) {
        if (do_draw_kick)
            rasterize_sprite(g_gif.v0x, g_gif.v0y, x, y,
                              g_gif.v0u, g_gif.v0v, g_gif.cur_u, g_gif.cur_v,
                              g_gif.v0s, g_gif.v0t, g_gif.v0q, g_gif.cur_s, g_gif.cur_t, g_gif.cur_q,
                              g_gif.v0z, raw_z, g_gif.v0f, g_gif.cur_fog);
    } else {
        g_gif.unsupported_prims_seen++;
    }

    /* SPRITE only ever accumulates 2 vertices at a time before
     * restarting - it has no strip/fan continuation on real hardware
     * either (POINT/LINE, now implemented above this function's
     * TRIANGLE dispatch, follow the same restart-every-N pattern as
     * SPRITE, just with N=1/2). */
    g_gif.has_vertex0 = 0;
}

static void apply_rgbaq(uint32_t word0, uint32_t word1, uint32_t word2)
{
    /* PACKED RGBAQ: R in word0 low byte, G in word1 low byte, B in
     * word2 low byte (word3 holds Q, a texture perspective term we
     * don't use). Alpha defaults to opaque (0xFF) since PACKED RGBAQ's
     * 4th word is Q, not A, in this simplified model - real hardware
     * does have an A field too (word "3" low byte in some
     * descriptions); we treat draws as opaque, which is fine for flat
     * background-style rectangles. */
    uint8_t r = (uint8_t)(word0 & 0xFFu);
    uint8_t g = (uint8_t)(word1 & 0xFFu);
    uint8_t b = (uint8_t)(word2 & 0xFFu);
    g_gif.rgba = ((uint32_t)0xFFu << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static void apply_ad_write(uint32_t addr, uint32_t data_lo, uint32_t data_hi)
{
    switch (addr) {
    case GS_REG_PRIM:
        g_gif.prim = data_lo;
        reset_tri_vseq();
        break;
    case GS_REG_RGBAQ: {
        /* A+D packs RGBAQ's R/G/B/A/Q into one 64-bit data value
         * rather than 4 separate qwords like PACKED mode does: R in
         * bits 0-7, G in 8-15, B in 16-23, A in 24-31 of the low word;
         * Q is the ENTIRE high word, as a real IEEE-754 float - cross-
         * checked against PCSX2's own GS/GSRegs.h GIFRegRGBAQ layout
         * (task #88 - previously data_hi/Q was read but discarded). */
        uint8_t r = (uint8_t)(data_lo & 0xFFu);
        uint8_t g = (uint8_t)((data_lo >> 8) & 0xFFu);
        uint8_t b = (uint8_t)((data_lo >> 16) & 0xFFu);
        uint8_t a = (uint8_t)((data_lo >> 24) & 0xFFu);
        g_gif.rgba = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
        g_gif.cur_q = u32_to_float(data_hi);
    } break;
    case GS_REG_XYZ2:
        /* A+D mode: Z is not available under this project's
         * established A+D XYZ2 convention (word0=X-only,
         * word1=Y-only - see gif.h's tri_z field comment for the
         * full explanation and why it isn't changed here) - pass
         * 0 for Z. */
        apply_xyz2_kick(data_lo, data_hi, 0u, 1);
        break;
    case GS_REG_XYZF2:
        /* Round 97 (138th finding, task #254): A+D XYZF2 follows the
         * same simplified, already-established A+D XYZ2 convention
         * above (X-only/Y-only, no Z) - F is likewise unavailable
         * under that convention, so cur_fog is left as whatever a
         * prior standalone GS_REG_FOG A+D write (or the 0xFF default)
         * already set, exactly like Z is left at 0. This performs a
         * normal drawing-kick vertex push, same as XYZ2. */
        apply_xyz2_kick(data_lo, data_hi, 0u, 1);
        break;
    case GS_REG_XYZ3:
    case GS_REG_XYZF3:
        /* Round 97 (138th finding, task #254): "vertex kick without
         * drawing kick" (official GS Users Manual's XYZ3/XYZF3
         * description) - advances the vertex queue exactly like
         * XYZ2/XYZF2 above but never triggers the terminal
         * rasterize_*() call, see apply_xyz2_kick()'s do_draw_kick
         * parameter. */
        apply_xyz2_kick(data_lo, data_hi, 0u, 0);
        break;
    case GS_REG_FOG:
        /* Round 97 (138th finding, task #254): standalone FOG
         * register - generic A+D 64-bit layout (data_lo=low32,
         * data_hi=high32, matching every OTHER A+D register in this
         * function except XYZ2/XYZF2's own established simplified
         * exception above), F occupying bits 63:56 per the official
         * GS Users Manual's BIT ASSIGN table - i.e. the top byte of
         * data_hi. */
        g_gif.cur_fog = (data_hi >> 24) & 0xFFu;
        break;
    case GS_REG_FOGCOL:
        /* Round 97 (138th finding, task #254): FOGCOL - FCR:bits0-7,
         * FCG:bits8-15, FCB:bits16-23 of data_lo (real GS Users Manual
         * BIT ASSIGN table), packed here via rgba_pack()'s existing
         * channel convention (alpha byte unused/0, matching FOGCOL
         * having no real alpha field). */
        g_gif.fogcol = rgba_pack(data_lo & 0xFFu, (data_lo >> 8) & 0xFFu, (data_lo >> 16) & 0xFFu, 0u);
        break;
    case GS_REG_PRMODECONT:
        /* Round 99 (140th finding, task #254): PRMODECONT.AC (bit 0
         * only, per the manual's BIT ASSIGN table) - see the
         * GS_REG_PRMODECONT/GS_REG_PRMODE comment in gif.h and
         * gs_effective_attr_prim() above for the full citation and
         * how this actually takes effect. */
        g_gif.prmodecont_ac = data_lo & 0x1u;
        break;
    case GS_REG_PRMODE:
        /* Round 99: PRMODE mirrors PRIM's bits 3-10 only (IIP/TME/
         * FGE/ABE/AA1/FST/CTXT/FIX) - bits 0-2 (TYPE) have no
         * counterpart in PRMODE per the manual's own field list, so
         * masked off here defensively (gs_effective_attr_prim()
         * always sources TYPE from PRIM regardless, but keeping
         * g_gif.prmode itself clean avoids any confusion if it's
         * ever read directly elsewhere in the future). Only takes
         * effect while PRMODECONT.AC=0, per the manual. */
        g_gif.prmode = data_lo & PRIM_ATTR_MASK;
        break;
    case GS_REG_FRAME_1: {
        /* FBP: bits 0-8, FBW: bits 9-14 (units of 64px - real hardware
         * convention), PSM: bits 15-20 (ignored, PSMCT32 assumed).
         * FBP here is used directly as our gs_mem "bp" word-offset
         * convention - not a claim it matches real hardware block
         * addressing (see gs_mem.h). Round 27: also mirrors into
         * ctx1_fbp/ctx1_fbw (context 1's permanent storage - see
         * gif.h's dual-context field comment). */
        uint32_t fbp = data_lo & 0x1FFu;
        uint32_t fbw_field = (data_lo >> 9) & 0x3Fu;
        uint32_t fbw = fbw_field * 64u;
        if (fbw == 0) fbw = 640; /* guard against a zero FBW making every pixel alias */
        g_gif.fbp = fbp;
        g_gif.fbw = fbw;
        g_gif.ctx1_fbp = fbp;
        g_gif.ctx1_fbw = fbw;
    } break;
    case GS_REG_FRAME_2: {
        /* Context 2's FRAME - identical bitfield to FRAME_1 above,
         * written ONLY into ctx2_fbp/ctx2_fbw (context 2 only becomes
         * "live" in the flat/active fields once a primitive is
         * actually drawn with PRIM.CTXT=1 - see gs_activate_context()). */
        uint32_t fbp = data_lo & 0x1FFu;
        uint32_t fbw_field = (data_lo >> 9) & 0x3Fu;
        uint32_t fbw = fbw_field * 64u;
        if (fbw == 0) fbw = 640;
        g_gif.ctx2_fbp = fbp;
        g_gif.ctx2_fbw = fbw;
    } break;
    case GS_REG_XYOFFSET_1:
        g_gif.xyoffset_x = data_lo & 0xFFFFu;
        g_gif.xyoffset_y = data_hi & 0xFFFFu;
        g_gif.ctx1_xyoffset_x = g_gif.xyoffset_x;
        g_gif.ctx1_xyoffset_y = g_gif.xyoffset_y;
        break;
    case GS_REG_XYOFFSET_2:
        g_gif.ctx2_xyoffset_x = data_lo & 0xFFFFu;
        g_gif.ctx2_xyoffset_y = data_hi & 0xFFFFu;
        break;
    case GS_REG_UV:
        /* Real hardware: 12.4 fixed-point texel coordinates (this
         * project's "FST=1" assumption - see gif.h's scope comment) -
         * same >>4 conversion as XYZ2's screen coordinates, no
         * XYOFFSET-equivalent subtraction (UV addresses texture
         * memory directly, not screen space). */
        g_gif.cur_u = (int32_t)(data_lo & 0xFFFFu) >> 4;
        g_gif.cur_v = (int32_t)((data_lo >> 16) & 0xFFFFu) >> 4;
        break;
    case GS_REG_ST:
        /* Real hardware (FST=0 mode, task #88): S/T are real IEEE-754
         * floats, one per word - cross-checked against PCSX2's own
         * GS/GSRegs.h GIFRegST layout (`float S; float T;`, no Q -
         * unlike PACKED mode's STQ tag, A+D mode's Q instead arrives
         * together with RGBAQ, see above). */
        g_gif.cur_s = u32_to_float(data_lo);
        g_gif.cur_t = u32_to_float(data_hi);
        break;
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2: {
        /* TEX0 bitfield cross-checked against PCSX2's own
         * GS/GSRegs.h GIFRegTEX0: word0 = TBP0(14):TBW(6):PSM(6):
         * TW(4):pad(2); word1 = pad(2):TCC(1):TFX(2):CBP(14):CPSM(4):
         * CSM(1):CSA(5):CLD(3). TBP0/TBW are used directly as OUR
         * gs_mem bp/bw convention, exactly like FRAME_1's FBP/FBW
         * above - not a claim of matching real hardware block-
         * swizzled addressing (see gs_mem.h). Round 27: TEX0_1 and
         * TEX0_2 share this parsing logic (identical bitfield),
         * writing into ctx1_xxx (+ the flat fields, for immediate
         * visibility) or ctx2_xxx respectively - see gif.h's dual-
         * context field comment. */
        uint32_t tbp0 = data_lo & 0x3FFFu;
        uint32_t tbw_field = (data_lo >> 14) & 0x3Fu;
        uint32_t tfx = (data_hi >> 3) & 0x3u;
        uint32_t tbw = tbw_field * 64u;
        if (tbw == 0) tbw = 640; /* guard against a zero TBW making every texel alias, same as FRAME_1's FBW guard */
        /* TW (bits 26-29 of word0) / TH (task #88: a 4-bit field that
         * straddles the 64-bit register - 2 bits from word0's top,
         * bits 30-31, plus 2 bits from word1's bottom, bits 0-1) -
         * cross-checked against PCSX2's own GS/GSRegs.h GIFRegTEX0's
         * union of two overlapping bitfield layouts. */
        uint32_t tw = (data_lo >> 26) & 0xFu;
        uint32_t th = ((data_lo >> 30) & 0x3u) | ((data_hi & 0x3u) << 2);
        /* Round 24: PSM (word0 bits 20-25) + CLUT fields (word1 bits
         * 5-31: CBP:14, CPSM:4, CSM:1 (ignored - CSM2's separate
         * load-list mode is a documented, unsupported gap; only CSM1
         * is modeled), CSA:5, CLD:3). See TEX_PSM_xxx and
         * CLUT_ROW_WIDTH's header comments for the full scope and
         * citation-honesty note. */
        uint32_t psm = (data_lo >> 20) & 0x3Fu;
        uint32_t cbp = (data_hi >> 5) & 0x3FFFu;
        uint32_t cpsm = (data_hi >> 19) & 0xFu;
        uint32_t csa = (data_hi >> 24) & 0x1Fu;
        uint32_t cld = (data_hi >> 29) & 0x7u;

        if (addr == GS_REG_TEX0_1) {
            g_gif.tex_tbp0 = tbp0; g_gif.tex_tbw = tbw; g_gif.tex_tfx = tfx;
            g_gif.tex_tw = tw; g_gif.tex_th = th;
            g_gif.tex_psm = psm; g_gif.tex_cbp = cbp; g_gif.tex_cpsm = cpsm;
            g_gif.tex_csa = csa; g_gif.tex_cld = cld;
            g_gif.ctx1_tex_tbp0 = tbp0; g_gif.ctx1_tex_tbw = tbw; g_gif.ctx1_tex_tfx = tfx;
            g_gif.ctx1_tex_tw = tw; g_gif.ctx1_tex_th = th;
            g_gif.ctx1_tex_psm = psm; g_gif.ctx1_tex_cbp = cbp; g_gif.ctx1_tex_cpsm = cpsm;
            g_gif.ctx1_tex_csa = csa; g_gif.ctx1_tex_cld = cld;
        } else {
            g_gif.ctx2_tex_tbp0 = tbp0; g_gif.ctx2_tex_tbw = tbw; g_gif.ctx2_tex_tfx = tfx;
            g_gif.ctx2_tex_tw = tw; g_gif.ctx2_tex_th = th;
            g_gif.ctx2_tex_psm = psm; g_gif.ctx2_tex_cbp = cbp; g_gif.ctx2_tex_cpsm = cpsm;
            g_gif.ctx2_tex_csa = csa; g_gif.ctx2_tex_cld = cld;
        }
    } break;
    case GS_REG_COLCLAMP:
        /* Round 101 (142nd finding, task #254): COLCLAMP.CLAMP (bit 0
         * only, per the manual's BIT ASSIGN table) - see gs_colclamp_channel()
         * above for the full citation and how this actually takes effect. */
        g_gif.colclamp = data_lo & 0x1u;
        g_gif.colclamp_configured = 1;
        break;
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2: {
        /* Round 100 (141st finding, task #254): TEX2 - "subset of
         * TEX0" (see gif.h's GS_REG_TEX2_1/2 comment). Reuses TEX0's
         * exact PSM/CBP/CPSM/CSA/CLD bit positions (data_lo bits
         * 20-25 / data_hi bits 5-31) but deliberately does NOT touch
         * tex_tbp0/tex_tbw/tex_tfx/tex_tw/tex_th - those keep
         * whatever the most recent TEX0 write set them to, per the
         * manual's real hardware behavior. */
        uint32_t psm = (data_lo >> 20) & 0x3Fu;
        uint32_t cbp = (data_hi >> 5) & 0x3FFFu;
        uint32_t cpsm = (data_hi >> 19) & 0xFu;
        uint32_t csa = (data_hi >> 24) & 0x1Fu;
        uint32_t cld = (data_hi >> 29) & 0x7u;

        if (addr == GS_REG_TEX2_1) {
            g_gif.tex_psm = psm; g_gif.tex_cbp = cbp; g_gif.tex_cpsm = cpsm;
            g_gif.tex_csa = csa; g_gif.tex_cld = cld;
            g_gif.ctx1_tex_psm = psm; g_gif.ctx1_tex_cbp = cbp; g_gif.ctx1_tex_cpsm = cpsm;
            g_gif.ctx1_tex_csa = csa; g_gif.ctx1_tex_cld = cld;
        } else {
            g_gif.ctx2_tex_psm = psm; g_gif.ctx2_tex_cbp = cbp; g_gif.ctx2_tex_cpsm = cpsm;
            g_gif.ctx2_tex_csa = csa; g_gif.ctx2_tex_cld = cld;
        }
    } break;
    case GS_REG_ZBUF_1: {
        /* GIFRegZBUF bitfield cross-checked against PCSX2's own
         * GS/GSRegs.h: word0 = ZBP(9):pad(15):PSM(6):pad(2);
         * word1 = ZMSK(1):pad(31). PSM is ignored (real Z formats
         * are PSMZ32/24/16 - this project stores Z as a plain
         * 32-bit word via gs_mem's existing PSMCT32-shaped helpers,
         * matching gs_mem.h's documented linear-addressing
         * simplification). ZBP is used directly as OUR gs_mem bp
         * convention, exactly like FRAME_1's FBP above. Real
         * hardware's ZBUF register has NO separate width field - Z
         * buffer addressing reuses the context's FBW, matching
         * this project's choice to pass g_gif.fbw to the Z-buffer
         * gs_mem_read/write_psmct32() calls in rasterize_triangle()/
         * rasterize_sprite(). zbuf_configured is this project's own
         * safety gate - see gif.h's field comment. Round 27: also
         * mirrors into ctx1_zbp/ctx1_zmsk/ctx1_zbuf_configured. */
        g_gif.zbp = data_lo & 0x1FFu;
        g_gif.zmsk = (int)(data_hi & 0x1u);
        g_gif.zbuf_configured = 1;
        g_gif.ctx1_zbp = g_gif.zbp;
        g_gif.ctx1_zmsk = g_gif.zmsk;
        g_gif.ctx1_zbuf_configured = 1;
    } break;
    case GS_REG_ZBUF_2: {
        /* Context 2's ZBUF - identical bitfield to ZBUF_1 above,
         * written ONLY into the ctx2_zxxx permanent fields. */
        g_gif.ctx2_zbp = data_lo & 0x1FFu;
        g_gif.ctx2_zmsk = (int)(data_hi & 0x1u);
        g_gif.ctx2_zbuf_configured = 1;
    } break;
    case GS_REG_TEST_1: {
        /* GIFRegTEST bitfield cross-checked against PCSX2's own
         * GS/GSRegs.h (see TEST_ZTE_MASK, TEST_ZTST_xxx, TEST_ATE_MASK,
         * etc in gif.h). Round 23 adds real ATE/ATST/AREF/AFAIL
         * (alpha test) - see gs_finish_pixel() below. DATE/DATM
         * (destination-alpha test) remain unmodeled, a separate,
         * still-open gap. Round 27: also mirrors into ctx1_zte/
         * ctx1_ztst/ctx1_ate/ctx1_atst/ctx1_aref/ctx1_afail. */
        g_gif.zte = (data_lo & TEST_ZTE_MASK) ? 1 : 0;
        g_gif.ztst = (int)((data_lo >> TEST_ZTST_SHIFT) & TEST_ZTST_MASK);
        g_gif.ate = (data_lo & TEST_ATE_MASK) ? 1 : 0;
        g_gif.atst = (int)((data_lo >> TEST_ATST_SHIFT) & TEST_ATST_MASK);
        g_gif.aref = (data_lo >> TEST_AREF_SHIFT) & TEST_AREF_MASK;
        g_gif.afail = (int)((data_lo >> TEST_AFAIL_SHIFT) & TEST_AFAIL_MASK);
        g_gif.ctx1_zte = g_gif.zte;
        g_gif.ctx1_ztst = g_gif.ztst;
        g_gif.ctx1_ate = g_gif.ate;
        g_gif.ctx1_atst = g_gif.atst;
        g_gif.ctx1_aref = g_gif.aref;
        g_gif.ctx1_afail = g_gif.afail;
    } break;
    case GS_REG_TEST_2: {
        /* Context 2's TEST - identical bitfield to TEST_1 above,
         * written ONLY into the ctx2_xxx permanent fields. */
        g_gif.ctx2_zte = (data_lo & TEST_ZTE_MASK) ? 1 : 0;
        g_gif.ctx2_ztst = (int)((data_lo >> TEST_ZTST_SHIFT) & TEST_ZTST_MASK);
        g_gif.ctx2_ate = (data_lo & TEST_ATE_MASK) ? 1 : 0;
        g_gif.ctx2_atst = (int)((data_lo >> TEST_ATST_SHIFT) & TEST_ATST_MASK);
        g_gif.ctx2_aref = (data_lo >> TEST_AREF_SHIFT) & TEST_AREF_MASK;
        g_gif.ctx2_afail = (int)((data_lo >> TEST_AFAIL_SHIFT) & TEST_AFAIL_MASK);
    } break;
    case GS_REG_ALPHA_1: {
        /* GIFRegALPHA bitfield (Round 23) - see ALPHA_*_SHIFT/
         * GS_ALPHA_* in gif.h. A/B/C/D live in word0 (data_lo)'s low
         * byte. FIX is bits 32-39 of the 64-bit register - i.e. the
         * LOW byte of word1 (data_hi), since data_lo/data_hi split
         * the 64-bit A+D value at the 32-bit word boundary the same
         * way every other A+D register in this file already does
         * (confirmed against this same function's GS_REG_ZBUF_1 case
         * just above: ZBP comes from data_lo, ZMSK - a word1 field -
         * comes from data_hi). Round 27: also mirrors into
         * ctx1_alpha_a/b/c/d/fix. */
        g_gif.alpha_a = (data_lo >> ALPHA_A_SHIFT) & ALPHA_ABCD_MASK;
        g_gif.alpha_b = (data_lo >> ALPHA_B_SHIFT) & ALPHA_ABCD_MASK;
        g_gif.alpha_c = (data_lo >> ALPHA_C_SHIFT) & ALPHA_ABCD_MASK;
        g_gif.alpha_d = (data_lo >> ALPHA_D_SHIFT) & ALPHA_ABCD_MASK;
        g_gif.alpha_fix = data_hi & ALPHA_FIX_MASK;
        g_gif.ctx1_alpha_a = g_gif.alpha_a;
        g_gif.ctx1_alpha_b = g_gif.alpha_b;
        g_gif.ctx1_alpha_c = g_gif.alpha_c;
        g_gif.ctx1_alpha_d = g_gif.alpha_d;
        g_gif.ctx1_alpha_fix = g_gif.alpha_fix;
    } break;
    case GS_REG_ALPHA_2: {
        /* Context 2's ALPHA - identical bitfield to ALPHA_1 above,
         * written ONLY into the ctx2_alpha_xxx permanent fields. */
        g_gif.ctx2_alpha_a = (data_lo >> ALPHA_A_SHIFT) & ALPHA_ABCD_MASK;
        g_gif.ctx2_alpha_b = (data_lo >> ALPHA_B_SHIFT) & ALPHA_ABCD_MASK;
        g_gif.ctx2_alpha_c = (data_lo >> ALPHA_C_SHIFT) & ALPHA_ABCD_MASK;
        g_gif.ctx2_alpha_d = (data_lo >> ALPHA_D_SHIFT) & ALPHA_ABCD_MASK;
        g_gif.ctx2_alpha_fix = data_hi & ALPHA_FIX_MASK;
    } break;

    case GS_REG_SCISSOR_1: {
        /* Round 96: real SCISSOR bit layout per the official GS
         * Users Manual: word0 bits[10:0]=SCAX0, bits[26:16]=SCAX1;
         * word1(data_hi) bits[10:0]=SCAY0, bits[26:16]=SCAY1 -
         * upper-left/lower-right corners of the enabled drawing area,
         * inclusive, in the same window coordinate system this
         * project's rasterizers already use for x/y. */
        uint32_t x0 = data_lo & 0x7FFu;
        uint32_t x1 = (data_lo >> 16) & 0x7FFu;
        uint32_t y0 = data_hi & 0x7FFu;
        uint32_t y1 = (data_hi >> 16) & 0x7FFu;
        g_gif.scissor_x0 = x0; g_gif.scissor_x1 = x1;
        g_gif.scissor_y0 = y0; g_gif.scissor_y1 = y1;
        g_gif.scissor_configured = 1;
        g_gif.ctx1_scissor_x0 = x0; g_gif.ctx1_scissor_x1 = x1;
        g_gif.ctx1_scissor_y0 = y0; g_gif.ctx1_scissor_y1 = y1;
        g_gif.ctx1_scissor_configured = 1;
    } break;
    case GS_REG_SCISSOR_2: {
        uint32_t x0 = data_lo & 0x7FFu;
        uint32_t x1 = (data_lo >> 16) & 0x7FFu;
        uint32_t y0 = data_hi & 0x7FFu;
        uint32_t y1 = (data_hi >> 16) & 0x7FFu;
        g_gif.ctx2_scissor_x0 = x0; g_gif.ctx2_scissor_x1 = x1;
        g_gif.ctx2_scissor_y0 = y0; g_gif.ctx2_scissor_y1 = y1;
        g_gif.ctx2_scissor_configured = 1;
    } break;
    case GS_REG_CLAMP_1: {
        /* Round 98 (139th finding, task #254): real CLAMP bit layout
         * per the official GS Users Manual: word0(data_lo) bits[1:0]=
         * WMS, bits[3:2]=WMT, bits[13:4]=MINU, bits[23:14]=MAXU;
         * MINV/MAXV (10-bit fields each) straddle the 64-bit
         * register's word boundary - MINV is bits[33:24] (low 8 bits
         * in data_lo's top byte, top 2 bits in data_hi's bottom 2
         * bits), MAXV is bits[43:34] (entirely within data_hi,
         * bits[11:2]) - same generic 64-bit A+D word0/word1 mapping
         * this file already uses for FOG/FOGCOL (Round 97). */
        uint32_t wms = data_lo & 0x3u;
        uint32_t wmt = (data_lo >> 2) & 0x3u;
        uint32_t minu = (data_lo >> 4) & 0x3FFu;
        uint32_t maxu = (data_lo >> 14) & 0x3FFu;
        uint32_t minv = ((data_lo >> 24) & 0xFFu) | ((data_hi & 0x3u) << 8);
        uint32_t maxv = (data_hi >> 2) & 0x3FFu;
        g_gif.clamp_wms = wms; g_gif.clamp_wmt = wmt;
        g_gif.clamp_minu = minu; g_gif.clamp_maxu = maxu;
        g_gif.clamp_minv = minv; g_gif.clamp_maxv = maxv;
        g_gif.clamp_configured = 1;
        g_gif.ctx1_clamp_wms = wms; g_gif.ctx1_clamp_wmt = wmt;
        g_gif.ctx1_clamp_minu = minu; g_gif.ctx1_clamp_maxu = maxu;
        g_gif.ctx1_clamp_minv = minv; g_gif.ctx1_clamp_maxv = maxv;
        g_gif.ctx1_clamp_configured = 1;
    } break;
    case GS_REG_CLAMP_2: {
        uint32_t wms = data_lo & 0x3u;
        uint32_t wmt = (data_lo >> 2) & 0x3u;
        uint32_t minu = (data_lo >> 4) & 0x3FFu;
        uint32_t maxu = (data_lo >> 14) & 0x3FFu;
        uint32_t minv = ((data_lo >> 24) & 0xFFu) | ((data_hi & 0x3u) << 8);
        uint32_t maxv = (data_hi >> 2) & 0x3FFu;
        g_gif.ctx2_clamp_wms = wms; g_gif.ctx2_clamp_wmt = wmt;
        g_gif.ctx2_clamp_minu = minu; g_gif.ctx2_clamp_maxu = maxu;
        g_gif.ctx2_clamp_minv = minv; g_gif.ctx2_clamp_maxv = maxv;
        g_gif.ctx2_clamp_configured = 1;
    } break;
    case GS_REG_TEX1_1: {
        /* GIFRegTEX1 (Round 28) - real bit layout, moderate
         * confidence (same session-limited-research caveat as this
         * round's other new registers - see docs/STATUS.md's "GS
         * Round 28" section): word0 = LCM:1(bit0), MXL:3(bits2-4),
         * MMAG:1(bit9), MMIN:3(bits10-12), MTBA:1(bit14); word1 =
         * L:2(bits0-1), K:12(bits2-13, signed, 1/16 units). Round 29
         * continued (15th change): also mirrors into ctx1_tex1_xxx,
         * same pattern as every other _1 register in this function. */
        g_gif.tex1_lcm = (data_lo & 0x1u) ? 1 : 0;
        g_gif.tex1_mxl = (data_lo >> 2) & 0x7u;
        g_gif.tex1_mmag = (data_lo & 0x200u) ? 1 : 0;
        g_gif.tex1_mmin = (data_lo >> 10) & 0x7u;
        g_gif.tex1_mtba = (data_lo & 0x4000u) ? 1 : 0;
        g_gif.tex1_l = data_hi & 0x3u;
        {
            /* K is signed 12 bits (bits 2-13 of word1) - sign-extend
             * from bit 11 (the field's own top bit). */
            int32_t k_raw = (int32_t)((data_hi >> 2) & 0xFFFu);
            if (k_raw & 0x800) k_raw -= 0x1000; /* sign-extend a 12-bit value */
            g_gif.tex1_k = k_raw;
        }
        g_gif.ctx1_tex1_lcm = g_gif.tex1_lcm;
        g_gif.ctx1_tex1_mxl = g_gif.tex1_mxl;
        g_gif.ctx1_tex1_mmag = g_gif.tex1_mmag;
        g_gif.ctx1_tex1_mmin = g_gif.tex1_mmin;
        g_gif.ctx1_tex1_mtba = g_gif.tex1_mtba;
        g_gif.ctx1_tex1_l = g_gif.tex1_l;
        g_gif.ctx1_tex1_k = g_gif.tex1_k;
    } break;
    case GS_REG_TEX1_2: {
        /* Context 2's TEX1 - identical bitfield to TEX1_1 above,
         * written ONLY into the ctx2_tex1_xxx permanent fields
         * (Round 29 continued, 15th change). */
        g_gif.ctx2_tex1_lcm = (data_lo & 0x1u) ? 1 : 0;
        g_gif.ctx2_tex1_mxl = (data_lo >> 2) & 0x7u;
        g_gif.ctx2_tex1_mmag = (data_lo & 0x200u) ? 1 : 0;
        g_gif.ctx2_tex1_mmin = (data_lo >> 10) & 0x7u;
        g_gif.ctx2_tex1_mtba = (data_lo & 0x4000u) ? 1 : 0;
        g_gif.ctx2_tex1_l = data_hi & 0x3u;
        {
            int32_t k_raw = (int32_t)((data_hi >> 2) & 0xFFFu);
            if (k_raw & 0x800) k_raw -= 0x1000;
            g_gif.ctx2_tex1_k = k_raw;
        }
    } break;
    case GS_REG_MIPTBP1_1: {
        /* GIFRegMIPTBP1 (Round 28) - modeled as a plain sequential
         * 64-bit bitfield (TBP1:14, TBW1:6, TBP2:14, TBW2:6, TBP3:14,
         * TBW3:6, spare:4 = 64 bits total, no word-alignment padding
         * between fields) - moderate confidence, same citation-
         * honesty note as TEX1_1 above. TBP2/TBP3 straddle the
         * word0/word1 boundary, decoded with the same cross-word
         * technique this project already uses for TEX0's TW/TH
         * field (see that case's own comment, several rounds ago).
         * Round 29 continued (15th change): also mirrors into
         * ctx1_tex_mip_tbp/tbw[0..2]. */
        g_gif.tex_mip_tbp[0] = data_lo & 0x3FFFu;             /* TBP1: bits 0-13 */
        g_gif.tex_mip_tbw[0] = ((data_lo >> 14) & 0x3Fu) * 64u; /* TBW1: bits 14-19 */
        if (g_gif.tex_mip_tbw[0] == 0) g_gif.tex_mip_tbw[0] = 640;
        g_gif.tex_mip_tbp[1] = ((data_lo >> 20) & 0xFFFu) | ((data_hi & 0x3u) << 12); /* TBP2: bits 20-33 */
        g_gif.tex_mip_tbw[1] = (((data_hi >> 2) & 0x3Fu)) * 64u; /* TBW2: bits 34-39 */
        if (g_gif.tex_mip_tbw[1] == 0) g_gif.tex_mip_tbw[1] = 640;
        g_gif.tex_mip_tbp[2] = (data_hi >> 8) & 0x3FFFu;      /* TBP3: bits 40-53 */
        g_gif.tex_mip_tbw[2] = ((data_hi >> 22) & 0x3Fu) * 64u; /* TBW3: bits 54-59 */
        if (g_gif.tex_mip_tbw[2] == 0) g_gif.tex_mip_tbw[2] = 640;
        g_gif.ctx1_tex_mip_tbp[0] = g_gif.tex_mip_tbp[0]; g_gif.ctx1_tex_mip_tbw[0] = g_gif.tex_mip_tbw[0];
        g_gif.ctx1_tex_mip_tbp[1] = g_gif.tex_mip_tbp[1]; g_gif.ctx1_tex_mip_tbw[1] = g_gif.tex_mip_tbw[1];
        g_gif.ctx1_tex_mip_tbp[2] = g_gif.tex_mip_tbp[2]; g_gif.ctx1_tex_mip_tbw[2] = g_gif.tex_mip_tbw[2];
    } break;
    case GS_REG_MIPTBP1_2: {
        /* Context 2's MIPTBP1 - identical layout to MIPTBP1_1 above,
         * written ONLY into ctx2_tex_mip_tbp/tbw[0..2] (Round 29
         * continued, 15th change). */
        g_gif.ctx2_tex_mip_tbp[0] = data_lo & 0x3FFFu;
        g_gif.ctx2_tex_mip_tbw[0] = ((data_lo >> 14) & 0x3Fu) * 64u;
        if (g_gif.ctx2_tex_mip_tbw[0] == 0) g_gif.ctx2_tex_mip_tbw[0] = 640;
        g_gif.ctx2_tex_mip_tbp[1] = ((data_lo >> 20) & 0xFFFu) | ((data_hi & 0x3u) << 12);
        g_gif.ctx2_tex_mip_tbw[1] = (((data_hi >> 2) & 0x3Fu)) * 64u;
        if (g_gif.ctx2_tex_mip_tbw[1] == 0) g_gif.ctx2_tex_mip_tbw[1] = 640;
        g_gif.ctx2_tex_mip_tbp[2] = (data_hi >> 8) & 0x3FFFu;
        g_gif.ctx2_tex_mip_tbw[2] = ((data_hi >> 22) & 0x3Fu) * 64u;
        if (g_gif.ctx2_tex_mip_tbw[2] == 0) g_gif.ctx2_tex_mip_tbw[2] = 640;
    } break;
    case GS_REG_MIPTBP2_1: {
        /* GIFRegMIPTBP2 - identical layout to MIPTBP1 above, for
         * levels 4/5/6 instead of 1/2/3. Round 29 continued (15th
         * change): also mirrors into ctx1_tex_mip_tbp/tbw[3..5]. */
        g_gif.tex_mip_tbp[3] = data_lo & 0x3FFFu;
        g_gif.tex_mip_tbw[3] = ((data_lo >> 14) & 0x3Fu) * 64u;
        if (g_gif.tex_mip_tbw[3] == 0) g_gif.tex_mip_tbw[3] = 640;
        g_gif.tex_mip_tbp[4] = ((data_lo >> 20) & 0xFFFu) | ((data_hi & 0x3u) << 12);
        g_gif.tex_mip_tbw[4] = (((data_hi >> 2) & 0x3Fu)) * 64u;
        if (g_gif.tex_mip_tbw[4] == 0) g_gif.tex_mip_tbw[4] = 640;
        g_gif.tex_mip_tbp[5] = (data_hi >> 8) & 0x3FFFu;
        g_gif.tex_mip_tbw[5] = ((data_hi >> 22) & 0x3Fu) * 64u;
        if (g_gif.tex_mip_tbw[5] == 0) g_gif.tex_mip_tbw[5] = 640;
        g_gif.ctx1_tex_mip_tbp[3] = g_gif.tex_mip_tbp[3]; g_gif.ctx1_tex_mip_tbw[3] = g_gif.tex_mip_tbw[3];
        g_gif.ctx1_tex_mip_tbp[4] = g_gif.tex_mip_tbp[4]; g_gif.ctx1_tex_mip_tbw[4] = g_gif.tex_mip_tbw[4];
        g_gif.ctx1_tex_mip_tbp[5] = g_gif.tex_mip_tbp[5]; g_gif.ctx1_tex_mip_tbw[5] = g_gif.tex_mip_tbw[5];
    } break;
    case GS_REG_MIPTBP2_2: {
        /* Context 2's MIPTBP2 - identical layout to MIPTBP2_1 above,
         * written ONLY into ctx2_tex_mip_tbp/tbw[3..5] (Round 29
         * continued, 15th change). */
        g_gif.ctx2_tex_mip_tbp[3] = data_lo & 0x3FFFu;
        g_gif.ctx2_tex_mip_tbw[3] = ((data_lo >> 14) & 0x3Fu) * 64u;
        if (g_gif.ctx2_tex_mip_tbw[3] == 0) g_gif.ctx2_tex_mip_tbw[3] = 640;
        g_gif.ctx2_tex_mip_tbp[4] = ((data_lo >> 20) & 0xFFFu) | ((data_hi & 0x3u) << 12);
        g_gif.ctx2_tex_mip_tbw[4] = (((data_hi >> 2) & 0x3Fu)) * 64u;
        if (g_gif.ctx2_tex_mip_tbw[4] == 0) g_gif.ctx2_tex_mip_tbw[4] = 640;
        g_gif.ctx2_tex_mip_tbp[5] = (data_hi >> 8) & 0x3FFFu;
        g_gif.ctx2_tex_mip_tbw[5] = ((data_hi >> 22) & 0x3Fu) * 64u;
        if (g_gif.ctx2_tex_mip_tbw[5] == 0) g_gif.ctx2_tex_mip_tbw[5] = 640;
    } break;
    case GS_REG_BITBLTBUF: {
        /* GIFRegBITBLTBUF (Round 26): word0 = SBP:14(0-13),
         * SBW:6(16-21), SPSM:6(22-27); word1 = DBP:14(0-13),
         * DBW:6(16-21), DPSM:6(22-27). Only the destination fields
         * matter for the host-to-local path this project implements
         * - source fields are parsed for completeness/documentation
         * but unused (no local-to-host/local-to-local support - see
         * this register's header comment). DBP/DBW used directly as
         * this project's own gs_mem bp/bw convention, exactly like
         * FRAME_1/TEX0_1/ZBUF_1 elsewhere. */
        g_gif.trx_dbp = data_hi & 0x3FFFu;
        uint32_t dbw_field = (data_hi >> 16) & 0x3Fu;
        g_gif.trx_dbw = dbw_field * 64u;
        if (g_gif.trx_dbw == 0) g_gif.trx_dbw = 640;
        g_gif.trx_dpsm = (data_hi >> 22) & 0x3Fu;
    } break;
    case GS_REG_TRXPOS: {
        /* GIFRegTRXPOS: word0 = SSAX:11(0-10), SSAY:11(16-26);
         * word1 = DSAX:11(0-10), DSAY:11(16-26). Only DSAX/DSAY
         * matter here (destination start position) - source position
         * is parsed but unused, same scope note as BITBLTBUF. */
        g_gif.trx_dsax = data_hi & 0x7FFu;
        g_gif.trx_dsay = (data_hi >> 16) & 0x7FFu;
    } break;
    case GS_REG_TRXREG:
        /* GIFRegTRXREG: word0 = RRW:12(0-11), RRH:12(16-27) - the
         * transfer rectangle's width/height in pixels. word1 unused. */
        g_gif.trx_rrw = data_lo & 0xFFFu;
        g_gif.trx_rrh = (data_lo >> 16) & 0xFFFu;
        break;
    case GS_REG_TRXDIR: {
        /* GIFRegTRXDIR: word0 = XDIR:2(0-1). Writing this register is
         * what actually TRIGGERS the transfer on real hardware -
         * resets the progress cursor to (0,0) relative to DSAX/DSAY.
         * trx_active is only set for XDIR=host-to-local AND a
         * PSMCT32 destination (the only format this project's gs_mem
         * can actually store) - anything else (an unsupported
         * direction, or a non-PSMCT32 destination format) leaves
         * trx_active false, so a subsequent IMAGE packet's data is
         * safely byte-skipped rather than misinterpreted (see
         * process_one_packet's IMAGE-mode handling below). */
        g_gif.trx_xdir = data_lo & 0x3u;
        g_gif.trx_cur_x = 0;
        g_gif.trx_cur_y = 0;
        g_gif.trx_active = (g_gif.trx_xdir == TRXDIR_HOST_TO_LOCAL) &&
                            (g_gif.trx_dpsm == TEX_PSM_PSMCT32);
    } break;
    default:
        break; /* unhandled register - ignored, not an error */
    }
}

/* Extracts the 4-bit register-descriptor nibble for register index
 * 'idx' (0-15) from the GIFtag's REGS field (tag words 2 and 3 - 8
 * nibbles each, low nibble = register 0). */
static inline uint32_t regs_nibble(uint32_t w2, uint32_t w3, uint32_t idx)
{
    if (idx < 8)
        return (w2 >> (idx * 4)) & 0xFu;
    return (w3 >> ((idx - 8) * 4)) & 0xFu;
}

/* Processes one complete PACKED-mode GIF packet (tag + NLOOP*NREG data
 * qwords) starting at 'p', which must have at least 16 bytes (the
 * tag) available. Returns the number of bytes consumed, or 0 if 'len'
 * isn't enough for even the tag. */
static uint32_t process_one_packet(const uint8_t *p, uint32_t len)
{
    if (len < 16) return 0;

    uint32_t tag_w0 = rd_le32(p + 0);
    uint32_t tag_w1 = rd_le32(p + 4);
    uint32_t tag_w2 = rd_le32(p + 8);
    uint32_t tag_w3 = rd_le32(p + 12);

    uint32_t nloop = tag_w0 & 0x7FFFu;
    uint32_t flg   = (tag_w1 >> 26) & 0x3u;
    uint32_t nreg  = (tag_w1 >> 28) & 0xFu;
    uint32_t pre   = (tag_w1 >> 14) & 0x1u;
    uint32_t prim  = (tag_w1 >> 15) & 0x7FFu;

    if (pre) {
        g_gif.prim = prim;
        reset_tri_vseq();
    }

    uint32_t consumed = 16;

    /* Round 26: REGLIST mode (FLG=1). Real hardware packs TWO plain
     * 64-bit register values per 128-bit qword (register A in words
     * 0-1, register B in words 2-3), looping NLOOP times through the
     * tag's NREG-register REGS descriptor - exactly the same REGS/
     * NREG tag fields PACKED mode uses, just interpreted as a flat
     * stream of 64-bit values instead of PACKED's per-register
     * 128-bit expanded encodings. Total registers = NLOOP*NREG; total
     * qwords = ceil(total/2) (the last qword's upper half is unused
     * padding when the total is odd - real hardware behavior).
     *
     * Every register in the stream is routed through apply_ad_write()
     * uniformly: it already implements the exact "natural" 64-bit
     * encoding REGLIST uses for PRIM/RGBAQ/XYZ2/TEX0_1/FRAME_1/ZBUF_1/
     * TEST_1/ALPHA_1/etc (the same encoding A+D writes use in PACKED
     * mode) - reusing it here is both more complete than duplicating
     * PACKED's own narrower inline switch and, more importantly,
     * already tested. Note this inherits this project's existing,
     * already-documented A+D XYZ2 simplification (no real Z - see
     * apply_ad_write's own GS_REG_XYZ2 case) for REGLIST-mode XYZ2
     * writes too - a consistent, not a new, limitation. */
    if (flg == 1 /* REGLIST */ && nreg != 0) {
        uint32_t total_regs = nloop * nreg;
        for (uint32_t i = 0; i < total_regs; i++) {
            uint32_t qword_idx = i / 2;
            uint32_t half = i % 2;
            uint32_t qoff = consumed + qword_idx * 16u;
            if (qoff + 16 > len) {
                /* Incomplete - report only what's fully consumed so
                 * far (whole qwords), same policy as PACKED mode. */
                return consumed + qword_idx * 16u;
            }
            const uint8_t *q = p + qoff;
            uint32_t lo = half == 0 ? rd_le32(q + 0) : rd_le32(q + 8);
            uint32_t hi = half == 0 ? rd_le32(q + 4) : rd_le32(q + 12);
            uint32_t reg_code = regs_nibble(tag_w2, tag_w3, i % nreg);
            apply_ad_write(reg_code, lo, hi);
        }
        uint32_t total_qwords = (total_regs + 1u) / 2u; /* ceil(total/2) */
        g_gif.quadwords_seen += total_qwords; /* one count per whole qword actually consumed, matching PACKED's per-qword accounting */
        return consumed + total_qwords * 16u;
    }

    if (flg == 2 || flg == 3 /* IMAGE (3 is the reserved/disabled variant, treated the same) */ || nreg == 0) {
        /* IMAGE mode: NLOOP qwords of completely raw pixel data - no
         * register interpretation, no REGS descriptor involved at
         * all (byte accounting was already correct before this
         * round: IMAGE's data size really is exactly NLOOP qwords).
         * Round 26 adds REAL pixel writing when a host-to-local
         * PSMCT32 transfer is active (g_gif.trx_active, set by a
         * prior TRXDIR write - see apply_ad_write's GS_REG_TRXDIR
         * case) - each qword holds exactly 4 raw PSMCT32 pixels,
         * consumed in raster order and written via the destination
         * rectangle's (trx_dbp,trx_dbw) at (trx_dsax+cur_x,
         * trx_dsay+cur_y), wrapping at trx_rrw and stopping once
         * trx_rrh rows are filled (matching real hardware's
         * rectangle-bounded transfer). Anything trx_active doesn't
         * cover (unsupported XDIR, non-PSMCT32 destination, or a
         * degenerate nreg==0 tag with flg=0/PACKED that isn't really
         * IMAGE data at all) falls back to the pre-existing safe
         * byte-skip - not interpreted, but the stream stays in sync. */
        uint32_t skip_qwords = nloop;
        uint32_t skip_bytes = skip_qwords * 16u;
        if (consumed + skip_bytes > len) return consumed; /* not enough data yet */

        if ((flg == 2 || flg == 3) && g_gif.trx_active) {
            for (uint32_t i = 0; i < nloop && g_gif.trx_active; i++) {
                const uint8_t *q = p + consumed + i * 16u;
                uint32_t px[4];
                px[0] = rd_le32(q + 0);
                px[1] = rd_le32(q + 4);
                px[2] = rd_le32(q + 8);
                px[3] = rd_le32(q + 12);
                for (int k = 0; k < 4 && g_gif.trx_active; k++) {
                    gs_mem_write_psmct32(g_gif.trx_dbp, g_gif.trx_dbw,
                                          g_gif.trx_dsax + g_gif.trx_cur_x,
                                          g_gif.trx_dsay + g_gif.trx_cur_y,
                                          px[k]);
                    g_gif.trx_cur_x++;
                    if (g_gif.trx_cur_x >= g_gif.trx_rrw) {
                        g_gif.trx_cur_x = 0;
                        g_gif.trx_cur_y++;
                        if (g_gif.trx_cur_y >= g_gif.trx_rrh)
                            g_gif.trx_active = 0; /* transfer complete - a new TRXDIR write is required to start another */
                    }
                }
            }
        }

        return consumed + skip_bytes;
    }

    for (uint32_t loop = 0; loop < nloop; loop++) {
        for (uint32_t reg = 0; reg < nreg; reg++) {
            if (consumed + 16 > len)
                return consumed; /* incomplete - caller decides what to do */

            const uint8_t *q = p + consumed;
            uint32_t w0 = rd_le32(q + 0);
            uint32_t w1 = rd_le32(q + 4);
            uint32_t w2 = rd_le32(q + 8);
            uint32_t w3 = rd_le32(q + 12);
            (void)w3; /* word 4 of the qword (XYZ2's ADC/context bit, etc.) - not used in this simplified model. w2 (Z, for XYZ2) IS used as of task #89 - see the GIF_REG_XYZ2 case below. */

            uint32_t reg_code = regs_nibble(tag_w2, tag_w3, reg);

            switch (reg_code) {
            case GIF_REG_PRIM:  g_gif.prim = w0; reset_tri_vseq(); break;
            case GIF_REG_RGBAQ: apply_rgbaq(w0, w1, w2); break;
            case GIF_REG_XYZ2:  apply_xyz2_kick(w0, w1, w2, 1); break; /* w2 = real Z (task #89) */
            /* Round 97 (138th finding, task #254): XYZF2 - real
             * GIFPackedXYZF2 layout (cross-checked against PCSX2's own
             * GS/GSRegs.h): X in w0 (same as XYZ2), Y in w1 (same as
             * XYZ2), w2's low 24 bits = Z (narrower than XYZ2's full
             * 32-bit Z - real hardware trades Z range for the F byte),
             * w2's top 8 bits = Fog coefficient F. F is latched into
             * cur_fog BEFORE the shared vertex-kick call so it's
             * captured into the vertex's own tri_f/line_f/v0f slot
             * exactly like Z is. */
            case GIF_REG_XYZF2:
                g_gif.cur_fog = (w2 >> 24) & 0xFFu;
                apply_xyz2_kick(w0, w1, w2 & 0xFFFFFFu, 1);
                break;
            /* XYZ3/XYZF3: same field layouts as XYZ2/XYZF2 above, but
             * "vertex kick WITHOUT drawing kick" (do_draw_kick=0) -
             * see apply_xyz2_kick()'s own comment and the official GS
             * Users Manual's XYZ3 description. */
            case GIF_REG_XYZ3:
                apply_xyz2_kick(w0, w1, w2, 0);
                break;
            case GIF_REG_XYZF3:
                g_gif.cur_fog = (w2 >> 24) & 0xFFu;
                apply_xyz2_kick(w0, w1, w2 & 0xFFFFFFu, 0);
                break;
            /* Round 97 (138th finding, task #254): standalone FOG tag
             * - real hardware replicates a 64-bit register's value
             * into PACKED mode's low 64 bits (w0=bits31:0, w1=bits
             * 63:32, the same generic mapping this file already uses
             * implicitly for e.g. RGBAQ's word0/word1 split) - F sits
             * at bits 63:56 of that 64-bit value, i.e. the top byte of
             * w1. */
            case GIF_REG_FOG:
                g_gif.cur_fog = (w1 >> 24) & 0xFFu;
                break;
            case GIF_REG_AD:    apply_ad_write(w2 & 0xFFu, w0, w1); break; /* A+D: DATA in words 0-1, ADDR in word2's low byte */
            case GIF_REG_NOP:   default: break;
            }

            consumed += 16;
            g_gif.quadwords_seen++;
        }
    }

    return consumed;
}

void gif_process_quadwords(int channel, const uint8_t *data, uint32_t qwc)
{
    (void)channel;
    uint32_t len = qwc * 16u;
    uint32_t off = 0;

    while (off < len) {
        uint32_t used = process_one_packet(data + off, len - off);
        if (used == 0)
            break; /* not enough data for even a tag - stop, nothing more we can do this call */
        off += used;
        if (used < 16)
            break; /* incomplete packet - safety, shouldn't normally happen */
    }
}
