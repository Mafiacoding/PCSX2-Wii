#ifndef PCSX2WII_GS_WII_OUTPUT_H
#define PCSX2WII_GS_WII_OUTPUT_H

#include <stdint.h>

/*
 * gs_wii_output - converts a rectangular region of our (simplified,
 * linear) GS local memory PSMCT32 data into the Wii's YUV (Y1CbY2Cr,
 * packed 2 pixels per 32-bit word) external framebuffer format and
 * writes it directly into an XFB buffer.
 *
 * The RGB->YCbCr conversion (RGB8_to_YCbCr below) is ported from
 * libogc's own console.c (BSD-style license, see libogc_license.txt)
 * rather than reinvented, since the Wii's video hardware expects this
 * exact packed format and libogc's implementation is the authoritative
 * reference for it (clamped to 16-240 broadcast range, BT.601-ish
 * integer coefficients).
 *
 * This is the first actual "pixels reach a real display" path in the
 * project - previously nothing produced visible output. It is NOT
 * hooked up to the GIF/DMA pipeline yet (no primitive actually gets
 * drawn into GS memory by emulated PS2 code) - see docs/ROADMAP.md.
 */

/* xfb: pointer to a Wii external framebuffer (as set up by
 * SYS_AllocateFramebuffer / VIDEO_SetNextFramebuffer).
 * xfb_width_px: the *display* width in pixels (e.g. rmode->fbWidth) -
 * the XFB is stored as xfb_width_px/2 32-bit YUV words per scanline.
 * dst_x/dst_y: top-left destination position in the XFB, in pixels
 * (dst_x should be even - see note in gs_wii_output.c).
 * gs_bp/gs_bw/src_x/src_y: which region of GS memory to read from
 * (same conventions as gs_mem_read_psmct32).
 * width/height: size of the region to blit, in pixels. */
void gs_blit_psmct32_to_xfb(void *xfb, uint32_t xfb_width_px,
                             uint32_t dst_x, uint32_t dst_y,
                             uint32_t gs_bp, uint32_t gs_bw,
                             uint32_t src_x, uint32_t src_y,
                             uint32_t width, uint32_t height);

/* Exposed for unit testing - converts one pixel pair to a packed
 * Y1CbY2Cr word, ported verbatim (integer-for-integer) from libogc's
 * console.c RGB8_to_YCbCr. */
uint32_t gs_rgb8_pair_to_ycbcr(uint8_t r1, uint8_t g1, uint8_t b1,
                                uint8_t r2, uint8_t g2, uint8_t b2);

/* Round 119 (task #172/#274, GX/Wii output verification): decodes the
 * real PS2 GS hardware DISPFB1/DISPFB2 register field layout (well-
 * established, publicly documented GS register format) - FBP (frame
 * buffer pointer, bits 0-8, units of 2048 words) and FBW (frame
 * buffer width, bits 9-14, units of 64 pixels) - into this project's
 * own gs_mem.h plain word-offset/pixel-count convention.
 *
 * Moved here from main.c (where it originated as part of
 * run_real_boot_flow()'s real production display path, task #126)
 * specifically so it can be unit-tested: main.c #includes <gccore.h>
 * (the real Wii SDK), which isn't available in this project's host-
 * native regression environment, so nothing defined there has ever
 * been directly testable. This function has no Wii-specific
 * dependency at all - it's pure integer bit-field arithmetic - so
 * moving it into this already-host-tested module (gs_wii_output.c/.h)
 * closes a real, previously-undetected test-coverage gap in exactly
 * the code path that decides what real GS memory region gets shown
 * on screen once PMODE/DISPFB1 are finally written for real. main.c
 * now just calls this declaration instead of defining its own static
 * copy - no behavior change, verified identical logic. */
void gs_decode_dispfb(uint64_t dispfb, uint32_t *out_bp_words, uint32_t *out_bw_pixels);

#endif
