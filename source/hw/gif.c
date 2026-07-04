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

/* GIF register codes (PACKED mode descriptor nibbles / A+D addresses).
 * Cross-checked against PCSX2's GS/GSRegs.h GIF_A_D_REG enum. */
#define GIF_REG_PRIM      0x00
#define GIF_REG_RGBAQ     0x01
#define GIF_REG_XYZ2      0x05
#define GIF_REG_AD        0x0E
#define GIF_REG_NOP       0x0F

#define GS_REG_PRIM       0x00
#define GS_REG_RGBAQ      0x01
#define GS_REG_XYZ2       0x05
#define GS_REG_FRAME_1    0x4C
#define GS_REG_XYOFFSET_1 0x18

#define PRIM_TYPE_SPRITE  6

static inline uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

    if (!g_gif.has_vertex0) {
        g_gif.v0x = x;
        g_gif.v0y = y;
        g_gif.has_vertex0 = 1;
        return;
    }

    /* Second vertex: if we're drawing a SPRITE, fill the rectangle
     * between v0 and this vertex now. */
    if ((g_gif.prim & 0x7u) == PRIM_TYPE_SPRITE) {
        int32_t x0 = g_gif.v0x, y0 = g_gif.v0y;
        int32_t x1 = x, y1 = y;
        if (x1 < x0) { int32_t t = x0; x0 = x1; x1 = t; }
        if (y1 < y0) { int32_t t = y0; y0 = y1; y1 = t; }

        for (int32_t yy = y0; yy < y1; yy++) {
            if (yy < 0) continue;
            for (int32_t xx = x0; xx < x1; xx++) {
                if (xx < 0) continue;
                gs_mem_write_psmct32(g_gif.fbp, g_gif.fbw, (uint32_t)xx, (uint32_t)yy, g_gif.rgba);
            }
        }
        g_gif.sprites_drawn++;
    } else {
        g_gif.unsupported_prims_seen++;
    }

    /* SPRITE (and most other GS primitives in this simplified model)
     * only ever accumulate 2 vertices at a time before restarting -
     * real triangle-strip/fan continuation isn't modeled. */
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
        break;
    case GS_REG_RGBAQ: {
        /* A+D packs RGBAQ's R/G/B/A/Q into one 64-bit data value
         * rather than 4 separate qwords like PACKED mode does: R in
         * bits 0-7, G in 8-15, B in 16-23, A in 24-31 of the low word. */
        uint8_t r = (uint8_t)(data_lo & 0xFFu);
        uint8_t g = (uint8_t)((data_lo >> 8) & 0xFFu);
        uint8_t b = (uint8_t)((data_lo >> 16) & 0xFFu);
        uint8_t a = (uint8_t)((data_lo >> 24) & 0xFFu);
        g_gif.rgba = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
        (void)data_hi;
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

    if (pre)
        g_gif.prim = prim;

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
            case GIF_REG_PRIM:  g_gif.prim = w0; break;
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
