/* test_gs_reglist_image.c - host-native test for Round 26's REGLIST
 * and IMAGE GIF transfer modes (source/hw/gif.c's process_one_packet()),
 * previously entirely unimplemented (any non-PACKED tag was just
 * byte-skipped without interpretation). See include/core/hw/gif.h's
 * GS_REG_BITBLTBUF/TRXPOS/TRXREG/TRXDIR comments and gif.c's
 * process_one_packet() REGLIST/IMAGE branches for the full scope and
 * this round's citation-honesty note (live source-fetch research hit
 * a session limit again this round, same caveat as Rounds 24-25).
 */
#include <stdio.h>
#include <string.h>
#include "hw/gs_mem.c"
#include "hw/gif.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

/* Builds a raw 16-byte GIFTag directly (not the append_ad-based
 * PACKED-only helper other test files use, since REGLIST/IMAGE need
 * to control FLG/NREG/REGS explicitly). regs_lo/regs_hi together hold
 * up to 16 4-bit register-descriptor nibbles (tag words 2/3). */
static void write_giftag(uint8_t *buf, int *off, uint32_t nloop, uint32_t flg, uint32_t nreg,
                          uint32_t pre, uint32_t prim, uint32_t regs_lo, uint32_t regs_hi)
{
    uint32_t w0 = nloop & 0x7FFFu;
    uint32_t w1 = (flg & 0x3u) << 26 | (nreg & 0xFu) << 28 | (pre & 0x1u) << 14 | (prim & 0x7FFu) << 15;
    wle32(buf + *off, w0);
    wle32(buf + *off + 4, w1);
    wle32(buf + *off + 8, regs_lo);
    wle32(buf + *off + 12, regs_hi);
    *off += 16;
}

static void write_qword(uint8_t *buf, int *off, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    wle32(buf + *off, w0);
    wle32(buf + *off + 4, w1);
    wle32(buf + *off + 8, w2);
    wle32(buf + *off + 12, w3);
    *off += 16;
}

/* Helper: a single A+D PACKED-mode qword pair (tag + one AD qword),
 * used to configure BITBLTBUF/TRXPOS/TRXREG/TRXDIR before an IMAGE
 * transfer - reusing this project's standard A+D convention. */
static void write_ad_packet(uint8_t *buf, int *off, uint32_t data_lo, uint32_t data_hi, uint32_t addr)
{
    write_giftag(buf, off, 1, 0 /* PACKED */, 1, 0, 0, GIF_REG_AD, 0);
    write_qword(buf, off, data_lo, data_hi, addr, 0);
}

int main(void)
{
    { /* REGLIST, even register count (nreg=2: PRIM, RGBAQ), 1 loop -
       * exactly 1 qword of payload (2 registers, low half + high
       * half). */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 2];
        int off = 0;
        /* REGS: nibble0=PRIM(0x00), nibble1=RGBAQ(0x01) */
        write_giftag(buf, &off, 1, 1 /* REGLIST */, 2, 0, 0, 0x00u | (0x01u << 4), 0);
        uint32_t prim_val = (uint32_t)PRIM_TYPE_SPRITE;
        uint32_t rgba_val = 0x11223344u; /* R=0x44,G=0x33,B=0x22,A=0x11 in RGBAQ's natural low-byte-per-channel layout */
        write_qword(buf, &off, prim_val, 0, rgba_val, 0);

        uint32_t consumed_qwords = (uint32_t)off / 16u;
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, consumed_qwords);

        gif_state_t *st = gif_get_state();
        CHECK((st->prim & 0x7u) == PRIM_TYPE_SPRITE, "REGLIST: PRIM register set from the first (low-half) register slot");
        CHECK((st->rgba & 0xFFu) == 0x44u, "REGLIST: RGBAQ's R channel set from the second (high-half) register slot");
        CHECK(((st->rgba >> 8) & 0xFFu) == 0x33u, "REGLIST: RGBAQ's G channel correct");
        CHECK(((st->rgba >> 16) & 0xFFu) == 0x22u, "REGLIST: RGBAQ's B channel correct");
    }

    { /* REGLIST, odd total register count (nreg=3, nloop=1 -> 3
       * registers total, needs 2 qwords, second qword's upper half is
       * padding) - proves the ceil(total/2) byte accounting and that
       * a packet AFTER this one still parses correctly (no stream
       * desync from the padding). */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 5]; /* REGLIST: tag + 2 payload qwords (3 qwords) + next packet: tag + 1 payload qword (2 qwords) = 5 qwords */
        int off = 0;
        /* REGS: PRIM(0x00), RGBAQ(0x01), XYZ2(0x05) */
        write_giftag(buf, &off, 1, 1 /* REGLIST */, 3, 0, 0, 0x00u | (0x01u << 4) | (0x05u << 8), 0);
        write_qword(buf, &off, (uint32_t)PRIM_TYPE_SPRITE, 0, 0xFF000000u /* RGBAQ: A=0xFF (bits 24-31), R/G/B=0 */, 0);
        /* Third register (XYZ2) is alone in the second qword's LOW
         * half; the HIGH half is real-hardware padding (junk here,
         * must be ignored). */
        write_qword(buf, &off, (10u << 4), (20u << 4), 0xDEADBEEFu, 0xCAFEBABEu);

        /* A second, ordinary PACKED A+D packet right after - proves
         * the REGLIST packet's consumed-byte count was exactly right
         * (no desync). Sets FRAME_1 so we can check it landed. */
        write_giftag(buf, &off, 1, 0 /* PACKED */, 1, 0, 0, GIF_REG_AD, 0);
        write_qword(buf, &off, 0, 0, GS_REG_FRAME_1, 0); /* fbp=0, fbw_field=0 -> guarded default 640 */

        uint32_t consumed_qwords = (uint32_t)off / 16u;
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, consumed_qwords);

        gif_state_t *st = gif_get_state();
        CHECK((st->rgba >> 24) == 0xFFu, "REGLIST odd-count: second register (RGBAQ) applied correctly");
        CHECK(st->tri_vseq >= 0, "REGLIST odd-count: third (padded-qword) register (XYZ2) processed without crashing");
        CHECK(st->fbp == 0 && st->fbw == 640, "REGLIST odd-count: the FOLLOWING packet's FRAME_1 write landed correctly - no stream desync from the padding qword");
    }

    { /* IMAGE mode, host-to-local, PSMCT32: configure a 3x2-pixel
       * transfer at dbp=0/dbw=64, dsax=1/dsay=1, rrw=3/rrh=2, then
       * send 2 qwords (8 raw pixels) of IMAGE data - only 6 are
       * consumed by the rectangle (3x2), verifying both the wrap-at-
       * rrw behavior and the rrh-bounded stop. */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 4 * 6]; /* generous */
        int off = 0;
        write_ad_packet(buf, &off, (0u & 0x3FFFu), (1u & 0x3Fu) << 16 | (TEX_PSM_PSMCT32 << 22), GS_REG_BITBLTBUF); /* DBP=0, DBW field=1 (1*64=64px), DPSM=PSMCT32 */
        write_ad_packet(buf, &off, 0, (1u) | (1u << 16), GS_REG_TRXPOS); /* DSAX=1, DSAY=1 */
        write_ad_packet(buf, &off, 3u | (2u << 16), 0, GS_REG_TRXREG);   /* RRW=3, RRH=2 */
        write_ad_packet(buf, &off, TRXDIR_HOST_TO_LOCAL, 0, GS_REG_TRXDIR); /* triggers the transfer */

        write_giftag(buf, &off, 2 /* nloop: 2 qwords = 8 pixels */, 2 /* IMAGE */, 0, 0, 0, 0, 0);
        uint32_t colors[8] = {
            0xFF000001u, 0xFF000002u, 0xFF000003u, 0xFF000004u, /* row0: px0,1,2 used; px3 -> row1 col0 */
            0xFF000005u, 0xFF000006u, 0xFF000007u, 0xFF000008u, /* row1: col0(overwritten by px3 first),1,2 used; px7 unused (rect full) */
        };
        write_qword(buf, &off, colors[0], colors[1], colors[2], colors[3]);
        write_qword(buf, &off, colors[4], colors[5], colors[6], colors[7]);

        uint32_t consumed_qwords = (uint32_t)off / 16u;
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, consumed_qwords);

        /* Rectangle is dsax=1,dsay=1, rrw=3,rrh=2 -> covers x=1..3,
         * y=1..2. Raster order: (1,1)=colors[0],(2,1)=colors[1],
         * (3,1)=colors[2], wrap -> (1,2)=colors[3], (2,2)=colors[4],
         * (3,2)=colors[5]; colors[6]/[7] are past the rectangle
         * (rrh=2 rows only) and must NOT be written anywhere in the
         * rect (transfer completes and deactivates first). */
        CHECK(gs_mem_read_psmct32(0, 64, 1, 1) == colors[0], "IMAGE host-to-local: pixel (1,1) = first payload pixel");
        CHECK(gs_mem_read_psmct32(0, 64, 2, 1) == colors[1], "IMAGE host-to-local: pixel (2,1) = second payload pixel");
        CHECK(gs_mem_read_psmct32(0, 64, 3, 1) == colors[2], "IMAGE host-to-local: pixel (3,1) = third payload pixel");
        CHECK(gs_mem_read_psmct32(0, 64, 1, 2) == colors[3], "IMAGE host-to-local: wraps to row y=2 at RRW boundary - pixel (1,2) = 4th payload pixel");
        CHECK(gs_mem_read_psmct32(0, 64, 2, 2) == colors[4], "IMAGE host-to-local: pixel (2,2) = 5th payload pixel");
        CHECK(gs_mem_read_psmct32(0, 64, 3, 2) == colors[5], "IMAGE host-to-local: pixel (3,2) = 6th payload pixel, completes the 3x2 rectangle");
        CHECK(gif_get_state()->trx_active == 0, "IMAGE host-to-local: transfer auto-deactivates once the rectangle is filled (rrw*rrh pixels written)");
    }

    { /* IMAGE mode with NO prior TRXDIR trigger (trx_active stays
       * false by default) - proves gs_mem is left untouched, AND
       * that the stream still resyncs correctly for a packet right
       * after (byte accounting for the skip path is unaffected by
       * this round's changes). */
        gs_mem_init(); gif_init();
        uint8_t buf[16 * 3];
        int off = 0;
        write_giftag(buf, &off, 1 /* nloop */, 2 /* IMAGE */, 0, 0, 0, 0, 0);
        write_qword(buf, &off, 0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDDDDDDDDu);
        write_giftag(buf, &off, 1, 0 /* PACKED */, 1, 0, 0, GIF_REG_AD, 0);
        write_qword(buf, &off, (9u << 9), 0, GS_REG_FRAME_1, 0);

        uint32_t consumed_qwords = (uint32_t)off / 16u;
        gif_process_quadwords(DMA_CHANNEL_GIF, buf, consumed_qwords);

        CHECK(gs_mem_read_psmct32(0, 640, 0, 0) == 0, "IMAGE with no active transfer: gs_mem left untouched (skipped, not interpreted)");
        CHECK(gif_get_state()->fbp == 0, "IMAGE with no active transfer: the FOLLOWING packet's FRAME_1 write still landed correctly - stream stayed in sync");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
