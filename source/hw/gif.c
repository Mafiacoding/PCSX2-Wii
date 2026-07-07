/*
 * gif.c - see include/core/hw/gif.h for scope notes and references.
 */

#include "core/hw/gif.h"
#include "core/hw/gs_mem.h"
#include <string.h>

static gif_state_t g_gif;

void gif_init(void)
{
    memset(&g_gif, 0, sizeof(g_gif));
    g_gif.fbw = 640; /* sane default so an A+D FRAME write isn't strictly required for tests/demos */
}

gif_state_t *gif_get_state(void) { return &g_gif; }

/* GIF_REG_* / GS_REG_* / PRIM_TYPE_* are now public - see gif.h. */

static inline uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Resets the triangle vertex-accumulation sequence - called whenever
 * PRIM is written (real hardware starts a fresh vertex queue on a new
 * PRIM, so a mid-strip primitive-type change can't accidentally draw
 * a triangle from mismatched vertices). */
static void reset_tri_vseq(void) { g_gif.tri_vseq = 0; }

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

static void rasterize_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                uint32_t c0, uint32_t c1, uint32_t c2,
                                int32_t u0, int32_t v0, int32_t u1, int32_t v1, int32_t u2, int32_t v2,
                                float s0, float t0, float q0, float s1, float t1, float q1, float s2, float t2, float q2)
{
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

    /* Degenerate (zero-area) triangle - nothing to draw. Also guards
     * against divide-by-zero-shaped edge cases below. */
    int32_t area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;

    int gouraud = (g_gif.prim & PRIM_IIP_MASK) != 0;
    int textured = (g_gif.prim & PRIM_TME_MASK) != 0;
    /* Flat shading uses the LAST vertex's color on real hardware
     * (matches this rasterizer's pre-Gouraud behavior, which always
     * used whichever RGBAQ was active when the triangle completed -
     * i.e. exactly c2's value, since c2 is always the most-recently-
     * kicked vertex's color). */
    uint32_t flat_r = rgba_channel(c2, 0), flat_g = rgba_channel(c2, 8);
    uint32_t flat_b = rgba_channel(c2, 16), flat_a = rgba_channel(c2, 24);

    double inv_area = 1.0 / (double)area;

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

                uint32_t shaded_r, shaded_g, shaded_b, shaded_a;
                if (!gouraud) {
                    shaded_r = flat_r; shaded_g = flat_g; shaded_b = flat_b; shaded_a = flat_a;
                } else {
                    uint32_t r = (uint32_t)(b0 * rgba_channel(c0, 0)  + b1 * rgba_channel(c1, 0)  + b2 * rgba_channel(c2, 0)  + 0.5);
                    uint32_t g = (uint32_t)(b0 * rgba_channel(c0, 8)  + b1 * rgba_channel(c1, 8)  + b2 * rgba_channel(c2, 8)  + 0.5);
                    uint32_t b = (uint32_t)(b0 * rgba_channel(c0, 16) + b1 * rgba_channel(c1, 16) + b2 * rgba_channel(c2, 16) + 0.5);
                    uint32_t a = (uint32_t)(b0 * rgba_channel(c0, 24) + b1 * rgba_channel(c1, 24) + b2 * rgba_channel(c2, 24) + 0.5);
                    if (r > 255) r = 255;
                    if (g > 255) g = 255;
                    if (b > 255) b = 255;
                    if (a > 255) a = 255;
                    shaded_r = r; shaded_g = g; shaded_b = b; shaded_a = a;
                }

                uint32_t out;
                if (!textured) {
                    out = rgba_pack(shaded_r, shaded_g, shaded_b, shaded_a);
                } else {
                    double tu, tv;
                    if (g_gif.prim & PRIM_FST_MASK) {
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
                    uint32_t texel = gs_mem_read_psmct32(g_gif.tex_tbp0, g_gif.tex_tbw,
                                                          (uint32_t)tex_x, (uint32_t)tex_y);
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
                        if (r > 255) r = 255;
                        if (g > 255) g = 255;
                        if (b > 255) b = 255;
                        if (a > 255) a = 255;
                        out = rgba_pack(r, g, b, a);
                    }
                }
                gs_mem_write_psmct32(g_gif.fbp, g_gif.fbw, (uint32_t)xx, (uint32_t)yy, out);
            }
        }
    }
    g_gif.triangles_drawn++;
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
                              float s0, float t0, float q0, float s1, float t1, float q1)
{
    int textured = (g_gif.prim & PRIM_TME_MASK) != 0;

    /* Resolve each corner's texture coordinate into texel space
     * BEFORE the min/max reordering below, so the X/Y->U/V
     * correspondence direction is preserved regardless of which
     * corner the caller gave first (real hardware allows either
     * vertex order). */
    double tex_u0 = 0.0, tex_v0 = 0.0, tex_u1 = 0.0, tex_v1 = 0.0;
    if (textured) {
        if (g_gif.prim & PRIM_FST_MASK) {
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

    double x_span = (double)(x1 - x0);
    double y_span = (double)(y1 - y0);

    for (int32_t yy = sy0; yy < sy1; yy++) {
        if (yy < 0) continue;
        double frac_y = (y_span != 0.0) ? ((double)yy - (double)y0) / y_span : 0.0;
        for (int32_t xx = sx0; xx < sx1; xx++) {
            if (xx < 0) continue;

            uint32_t out;
            if (!textured) {
                out = g_gif.rgba;
            } else {
                double frac_x = (x_span != 0.0) ? ((double)xx - (double)x0) / x_span : 0.0;
                double tu = tex_u0 + frac_x * (tex_u1 - tex_u0);
                double tv = tex_v0 + frac_y * (tex_v1 - tex_v0);
                int32_t tex_x = (tu < 0.0) ? 0 : (int32_t)(tu + 0.5);
                int32_t tex_y = (tv < 0.0) ? 0 : (int32_t)(tv + 0.5);
                uint32_t texel = gs_mem_read_psmct32(g_gif.tex_tbp0, g_gif.tex_tbw,
                                                      (uint32_t)tex_x, (uint32_t)tex_y);
                if (g_gif.tex_tfx == TEX_TFX_DECAL) {
                    out = texel;
                } else {
                    uint32_t r = (rgba_channel(texel, 0)  * rgba_channel(g_gif.rgba, 0))  / 128u;
                    uint32_t g = (rgba_channel(texel, 8)  * rgba_channel(g_gif.rgba, 8))  / 128u;
                    uint32_t b = (rgba_channel(texel, 16) * rgba_channel(g_gif.rgba, 16)) / 128u;
                    uint32_t a = (rgba_channel(texel, 24) * rgba_channel(g_gif.rgba, 24)) / 128u;
                    if (r > 255) r = 255;
                    if (g > 255) g = 255;
                    if (b > 255) b = 255;
                    if (a > 255) a = 255;
                    out = rgba_pack(r, g, b, a);
                }
            }
            gs_mem_write_psmct32(g_gif.fbp, g_gif.fbw, (uint32_t)xx, (uint32_t)yy, out);
        }
    }
    g_gif.sprites_drawn++;
}

static void apply_xyz2(uint32_t word0, uint32_t word1)
{
    /* PACKED XYZ2 layout (GS/GSRegs.h-compatible bit positions): X in
     * bits 0-15 of word0, Y in bits 0-15 of word1 (both 12.4
     * fixed-point, offset-relative). Z and the ADC/context bits exist
     * in the other two words of the qword but aren't used here. */
    int32_t raw_x = (int32_t)(word0 & 0xFFFFu);
    int32_t raw_y = (int32_t)(word1 & 0xFFFFu);

    int32_t x = (raw_x - (int32_t)g_gif.xyoffset_x) >> 4;
    int32_t y = (raw_y - (int32_t)g_gif.xyoffset_y) >> 4;

    uint32_t ptype = g_gif.prim & 0x7u;

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
            if (g_gif.tri_vseq % 3 == 0)
                rasterize_triangle(g_gif.tri_x[0], g_gif.tri_y[0], g_gif.tri_x[1], g_gif.tri_y[1], g_gif.tri_x[2], g_gif.tri_y[2],
                                    g_gif.tri_rgba[0], g_gif.tri_rgba[1], g_gif.tri_rgba[2],
                                    g_gif.tri_u[0], g_gif.tri_v[0], g_gif.tri_u[1], g_gif.tri_v[1], g_gif.tri_u[2], g_gif.tri_v[2],
                                    g_gif.tri_s[0], g_gif.tri_t[0], g_gif.tri_q[0],
                                    g_gif.tri_s[1], g_gif.tri_t[1], g_gif.tri_q[1],
                                    g_gif.tri_s[2], g_gif.tri_t[2], g_gif.tri_q[2]);
        } else if (ptype == PRIM_TYPE_TRIANGLE_STRIP) {
            /* TRIANGLE_STRIP: each new vertex (from the 3rd onward)
             * forms a triangle with the previous 2 - a rolling
             * 3-slot window. */
            g_gif.tri_x[0] = g_gif.tri_x[1]; g_gif.tri_y[0] = g_gif.tri_y[1]; g_gif.tri_rgba[0] = g_gif.tri_rgba[1];
            g_gif.tri_u[0] = g_gif.tri_u[1]; g_gif.tri_v[0] = g_gif.tri_v[1];
            g_gif.tri_s[0] = g_gif.tri_s[1]; g_gif.tri_t[0] = g_gif.tri_t[1]; g_gif.tri_q[0] = g_gif.tri_q[1];
            g_gif.tri_x[1] = g_gif.tri_x[2]; g_gif.tri_y[1] = g_gif.tri_y[2]; g_gif.tri_rgba[1] = g_gif.tri_rgba[2];
            g_gif.tri_u[1] = g_gif.tri_u[2]; g_gif.tri_v[1] = g_gif.tri_v[2];
            g_gif.tri_s[1] = g_gif.tri_s[2]; g_gif.tri_t[1] = g_gif.tri_t[2]; g_gif.tri_q[1] = g_gif.tri_q[2];
            g_gif.tri_x[2] = x; g_gif.tri_y[2] = y; g_gif.tri_rgba[2] = g_gif.rgba;
            g_gif.tri_u[2] = g_gif.cur_u; g_gif.tri_v[2] = g_gif.cur_v;
            g_gif.tri_s[2] = g_gif.cur_s; g_gif.tri_t[2] = g_gif.cur_t; g_gif.tri_q[2] = g_gif.cur_q;
            if (g_gif.tri_vseq >= 3)
                rasterize_triangle(g_gif.tri_x[0], g_gif.tri_y[0], g_gif.tri_x[1], g_gif.tri_y[1], g_gif.tri_x[2], g_gif.tri_y[2],
                                    g_gif.tri_rgba[0], g_gif.tri_rgba[1], g_gif.tri_rgba[2],
                                    g_gif.tri_u[0], g_gif.tri_v[0], g_gif.tri_u[1], g_gif.tri_v[1], g_gif.tri_u[2], g_gif.tri_v[2],
                                    g_gif.tri_s[0], g_gif.tri_t[0], g_gif.tri_q[0],
                                    g_gif.tri_s[1], g_gif.tri_t[1], g_gif.tri_q[1],
                                    g_gif.tri_s[2], g_gif.tri_t[2], g_gif.tri_q[2]);
        } else { /* PRIM_TYPE_TRIANGLE_FAN */
            /* TRIANGLE_FAN: the first vertex is a fixed anchor
             * (slot 0, never overwritten); each new vertex forms a
             * triangle with the anchor and the previous vertex. */
            if (g_gif.tri_vseq == 1) {
                g_gif.tri_x[0] = x; g_gif.tri_y[0] = y; g_gif.tri_rgba[0] = g_gif.rgba;
                g_gif.tri_u[0] = g_gif.cur_u; g_gif.tri_v[0] = g_gif.cur_v;
                g_gif.tri_s[0] = g_gif.cur_s; g_gif.tri_t[0] = g_gif.cur_t; g_gif.tri_q[0] = g_gif.cur_q;
                g_gif.tri_x[1] = x; g_gif.tri_y[1] = y; g_gif.tri_rgba[1] = g_gif.rgba; /* also seed "previous" so vseq==2 has something to pair with */
                g_gif.tri_u[1] = g_gif.cur_u; g_gif.tri_v[1] = g_gif.cur_v;
                g_gif.tri_s[1] = g_gif.cur_s; g_gif.tri_t[1] = g_gif.cur_t; g_gif.tri_q[1] = g_gif.cur_q;
            } else {
                g_gif.tri_x[2] = x; g_gif.tri_y[2] = y; g_gif.tri_rgba[2] = g_gif.rgba;
                g_gif.tri_u[2] = g_gif.cur_u; g_gif.tri_v[2] = g_gif.cur_v;
                g_gif.tri_s[2] = g_gif.cur_s; g_gif.tri_t[2] = g_gif.cur_t; g_gif.tri_q[2] = g_gif.cur_q;
                if (g_gif.tri_vseq >= 3)
                    rasterize_triangle(g_gif.tri_x[0], g_gif.tri_y[0], g_gif.tri_x[1], g_gif.tri_y[1], g_gif.tri_x[2], g_gif.tri_y[2],
                                        g_gif.tri_rgba[0], g_gif.tri_rgba[1], g_gif.tri_rgba[2],
                                        g_gif.tri_u[0], g_gif.tri_v[0], g_gif.tri_u[1], g_gif.tri_v[1], g_gif.tri_u[2], g_gif.tri_v[2],
                                        g_gif.tri_s[0], g_gif.tri_t[0], g_gif.tri_q[0],
                                        g_gif.tri_s[1], g_gif.tri_t[1], g_gif.tri_q[1],
                                        g_gif.tri_s[2], g_gif.tri_t[2], g_gif.tri_q[2]);
                g_gif.tri_x[1] = g_gif.tri_x[2]; g_gif.tri_y[1] = g_gif.tri_y[2]; g_gif.tri_rgba[1] = g_gif.tri_rgba[2];
                g_gif.tri_u[1] = g_gif.tri_u[2]; g_gif.tri_v[1] = g_gif.tri_v[2];
                g_gif.tri_s[1] = g_gif.tri_s[2]; g_gif.tri_t[1] = g_gif.tri_t[2]; g_gif.tri_q[1] = g_gif.tri_q[2];
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
        g_gif.has_vertex0 = 1;
        return;
    }

    /* Second vertex: if we're drawing a SPRITE, fill the rectangle
     * between v0 and this vertex now. */
    if (ptype == PRIM_TYPE_SPRITE) {
        rasterize_sprite(g_gif.v0x, g_gif.v0y, x, y,
                          g_gif.v0u, g_gif.v0v, g_gif.cur_u, g_gif.cur_v,
                          g_gif.v0s, g_gif.v0t, g_gif.v0q, g_gif.cur_s, g_gif.cur_t, g_gif.cur_q);
    } else {
        g_gif.unsupported_prims_seen++;
    }

    /* SPRITE only ever accumulates 2 vertices at a time before
     * restarting - it has no strip/fan continuation on real hardware
     * either (POINT/LINE, still unimplemented here, follow the same
     * restart-every-N pattern as SPRITE, just with N=1/2). */
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
        apply_xyz2(data_lo, data_hi);
        break;
    case GS_REG_FRAME_1: {
        /* FBP: bits 0-8, FBW: bits 9-14 (units of 64px - real hardware
         * convention), PSM: bits 15-20 (ignored, PSMCT32 assumed).
         * FBP here is used directly as our gs_mem "bp" word-offset
         * convention - not a claim it matches real hardware block
         * addressing (see gs_mem.h). */
        uint32_t fbp = data_lo & 0x1FFu;
        uint32_t fbw_field = (data_lo >> 9) & 0x3Fu;
        g_gif.fbp = fbp;
        g_gif.fbw = fbw_field * 64u;
        if (g_gif.fbw == 0) g_gif.fbw = 640; /* guard against a zero FBW making every pixel alias */
    } break;
    case GS_REG_XYOFFSET_1:
        g_gif.xyoffset_x = data_lo & 0xFFFFu;
        g_gif.xyoffset_y = data_hi & 0xFFFFu;
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
    case GS_REG_TEX0_1: {
        /* TEX0 bitfield cross-checked against PCSX2's own
         * GS/GSRegs.h GIFRegTEX0: word0 = TBP0(14):TBW(6):PSM(6):
         * TW(4):pad(2); word1 = pad(2):TCC(1):TFX(2):CBP(14):CPSM(4):
         * CSM(1):CSA(5):CLD(3). Only TBP0/TBW/TFX are modeled here
         * (PSM/TW/TH/CLUT fields ignored - PSMCT32 always assumed,
         * matching gs_mem's existing format limitation). TBP0/TBW are
         * used directly as OUR gs_mem bp/bw convention, exactly like
         * FRAME_1's FBP/FBW above - not a claim of matching real
         * hardware block-swizzled addressing (see gs_mem.h). */
        uint32_t tbp0 = data_lo & 0x3FFFu;
        uint32_t tbw_field = (data_lo >> 14) & 0x3Fu;
        uint32_t tfx = (data_hi >> 3) & 0x3u;
        g_gif.tex_tbp0 = tbp0;
        g_gif.tex_tbw = tbw_field * 64u;
        if (g_gif.tex_tbw == 0) g_gif.tex_tbw = 640; /* guard against a zero TBW making every texel alias, same as FRAME_1's FBW guard */
        g_gif.tex_tfx = tfx;
        /* TW (bits 26-29 of word0) / TH (task #88: a 4-bit field that
         * straddles the 64-bit register - 2 bits from word0's top,
         * bits 30-31, plus 2 bits from word1's bottom, bits 0-1) -
         * cross-checked against PCSX2's own GS/GSRegs.h GIFRegTEX0's
         * union of two overlapping bitfield layouts. Needed to scale
         * normalized ST+Q coordinates into texel space. */
        g_gif.tex_tw = (data_lo >> 26) & 0xFu;
        g_gif.tex_th = ((data_lo >> 30) & 0x3u) | ((data_hi & 0x3u) << 2);
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

    if (flg != 0 /* PACKED */ || nreg == 0) {
        /* REGLIST/IMAGE mode or a degenerate tag - not implemented.
         * Best effort: skip the data this tag claims (nloop qwords
         * for IMAGE mode; for REGLIST it'd normally be
         * nloop*nreg/2 qwords, but we don't parse it) so a stream
         * with a mix of supported/unsupported packets doesn't get
         * permanently desynced. */
        uint32_t skip_qwords = nloop;
        uint32_t skip_bytes = skip_qwords * 16u;
        if (consumed + skip_bytes > len) return consumed; /* not enough data yet */
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
            (void)w3; /* word 4 of the qword (Z/ADC bits for XYZ2, etc.) - not used in this simplified model */

            uint32_t reg_code = regs_nibble(tag_w2, tag_w3, reg);

            switch (reg_code) {
            case GIF_REG_PRIM:  g_gif.prim = w0; reset_tri_vseq(); break;
            case GIF_REG_RGBAQ: apply_rgbaq(w0, w1, w2); break;
            case GIF_REG_XYZ2:  apply_xyz2(w0, w1); break;
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
