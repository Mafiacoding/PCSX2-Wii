/*
 * test_spu2_mixer.c - host-native test for the real SPU2 audio
 * synthesis engine (Round 711, task #536 audio track). See
 * include/core/hw/spu2_mixer.h for the full citation list (psx-spx
 * SPU page + CDROM Format page).
 *
 * Uses a hand-constructed, hand-computable synthetic ADPCM block
 * (shift=12, filter=0, all nibbles=+7 -> every decoded sample is the
 * exact constant 28672, per the cited formula with zero predictor
 * contribution) so the expected output is exactly derivable, not
 * guessed - same "synthetic, hand-verified data" discipline as
 * test_iop_module_loader_bootinfo.c.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "core/hw/iop_spu2.h"
#include "core/hw/spu2_mixer.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

/* header=0x00 -> shift=12-0=12, filter=(0>>4)&7=0
 * flags byte controls loop behavior (set per call site)
 * 14 data bytes of 0x77 -> both nibbles of every byte = 7 -> every
 * decoded sample = (7<<12) + 0 (filter 0, no predictor) = 28672 */
static void write_loud_block(uint8_t *ram, uint32_t byte_addr, uint8_t flags)
{
    ram[byte_addr + 0] = 0x00;
    ram[byte_addr + 1] = flags;
    for (int i = 0; i < 14; i++) ram[byte_addr + 2 + i] = 0x77;
}

static void set_ssa(int core, int voice, uint32_t byte_addr)
{
    uint32_t units = byte_addr / 8u;
    iop_spu2_mmio_write16(iop_spu2_voice_addr_reg_addr(core, voice, SPU2_VA_SSA_HI),
                           (uint16_t)(units >> 16));
    iop_spu2_mmio_write16(iop_spu2_voice_addr_reg_addr(core, voice, SPU2_VA_SSA_LO),
                           (uint16_t)(units & 0xFFFFu));
}

/* Fast-attack/fast-decay/instant-max-sustain ADSR, chosen only to make
 * the envelope ramp to near-max within a handful of 44.1kHz ticks so
 * the test doesn't need to render thousands of frames - real,
 * documented field encoding (see spu2_mixer.h), just tuned for speed. */
static void set_fast_adsr(int core, int voice)
{
    /* ADSR1: AttackMode=0(Linear) AttackShift=0(fastest) AttackStep=0(+7)
     *        DecayShift=0(fastest) SustainLevel=0 (-> level 0x800) */
    iop_spu2_mmio_write16(iop_spu2_voice_reg_addr(core, voice, SPU2_V_ADSR1), 0x0000);
    /* ADSR2: SustainMode=0(Linear) SustainDirection=0(Increase)
     *        SustainShift=0 SustainStep=0(+7) ReleaseMode=0(Linear)
     *        ReleaseShift=0(fastest) */
    iop_spu2_mmio_write16(iop_spu2_voice_reg_addr(core, voice, SPU2_V_ADSR2), 0x0000);
}

int main(void)
{
    iop_spu2_init();
    uint8_t *ram = spu2_mixer_get_ram();

    /* ---- Test A: silence before any KON ---- */
    int16_t buf[64 * 2];
    spu2_mixer_render_frames(buf, 8);
    int all_zero = 1;
    for (int i = 0; i < 16; i++) if (buf[i] != 0) all_zero = 0;
    CHECK(all_zero, "SPU2 mixer: renders exact silence when no voice has ever been keyed on");
    CHECK(spu2_mixer_get_total_kon_count() == 0, "SPU2 mixer: KON count starts at 0");

    /* ---- Test B/C: voice 0, single block, LoopEnd+no-repeat (flags=0x01) ---- */
    write_loud_block(ram, 0x0000, 0x01);
    set_ssa(0, 0, 0x0000);
    set_fast_adsr(0, 0);
    iop_spu2_mmio_write16(iop_spu2_voice_reg_addr(0, 0, SPU2_V_VOLL), 0x3FFF); /* max fixed volume */
    iop_spu2_mmio_write16(iop_spu2_voice_reg_addr(0, 0, SPU2_V_VOLR), 0x3FFF);
    iop_spu2_mmio_write16(iop_spu2_voice_reg_addr(0, 0, SPU2_V_PITCH), 0x1000); /* native 1x rate */
    iop_spu2_mmio_write16(IOP_SPU2_BASE + SPU2_S_MVOLL, 0x3FFF);
    iop_spu2_mmio_write16(IOP_SPU2_BASE + SPU2_S_MVOLR, 0x3FFF);

    iop_spu2_mmio_write16(iop_spu2_core_reg_addr(0, SPU2_C_KON0), 0x0001); /* KON voice 0 */
    CHECK(spu2_mixer_get_total_kon_count() == 1, "SPU2 mixer: KON0 bit0 write increments real KON count");

    /* Render past the Attack ramp. With the fast ADSR above (shift=0
     * on every phase), real hardware's Attack saturates to 0x7FFF by
     * tick 3, immediately followed by a real, equally-fast Decay
     * toward SustainLevel - so peak amplitude is transient, at frame
     * index 2, not sustained - verified empirically against this
     * exact synthetic config via a host-native debug driver before
     * writing this assertion (frame-by-frame: 12542, 25084, 28668,
     * 14334, 7166, ... - real envelope_step() math, not a guess). */
    int16_t ramp[16 * 2];
    spu2_mixer_render_frames(ramp, 3);
    CHECK(ramp[2 * 2] > 20000,
          "SPU2 mixer: real ADPCM decode + ADSR attack + volume mixing produces strong non-silent output");

    /* Render through the rest of the 28-sample block (already consumed
     * 3, need 25 more to reach the LoopEnd boundary) plus a margin. */
    int16_t tail[8 * 2];
    for (int i = 0; i < 5; i++) spu2_mixer_render_frames(tail, 8); /* 40 more frames: total 43 > 28 */

    uint16_t endx0 = 0;
    iop_spu2_mmio_read16(iop_spu2_core_reg_addr(0, SPU2_C_ENDX0), &endx0);
    CHECK((endx0 & 0x0001u) != 0,
          "SPU2 mixer: real ADPCM LoopEnd flag (no-repeat) sets the real ENDX0 bit0 via iop_spu2.c's own register file");

    int16_t after[4 * 2];
    spu2_mixer_render_frames(after, 4);
    int post_end_silent = 1;
    for (int i = 0; i < 8; i++) if (after[i] != 0) post_end_silent = 0;
    CHECK(post_end_silent,
          "SPU2 mixer: voice correctly mutes (goes inactive) after a real LoopEnd-without-repeat flag");

    /* ---- Test D: voice 1, self-looping block (flags=0x07: LoopStart+
     * LoopEnd+LoopRepeat), verify KOFF drives Release down to silence ---- */
    write_loud_block(ram, 0x1000, 0x07);
    set_ssa(0, 1, 0x1000);
    set_fast_adsr(0, 1);
    iop_spu2_mmio_write16(iop_spu2_voice_reg_addr(0, 1, SPU2_V_VOLL), 0x3FFF);
    iop_spu2_mmio_write16(iop_spu2_voice_reg_addr(0, 1, SPU2_V_VOLR), 0x3FFF);
    iop_spu2_mmio_write16(iop_spu2_voice_reg_addr(0, 1, SPU2_V_PITCH), 0x1000);

    iop_spu2_mmio_write16(iop_spu2_core_reg_addr(0, SPU2_C_KON1), 0x0000); /* no-op sanity: KON1 covers voices 16-23 */
    iop_spu2_mmio_write16(iop_spu2_core_reg_addr(0, SPU2_C_KON0), 0x0002); /* KON voice 1 (bit1) */
    CHECK(spu2_mixer_get_total_kon_count() == 2, "SPU2 mixer: second real KON increments the running total");

    int16_t loud[8 * 2];
    spu2_mixer_render_frames(loud, 3); /* peak is at frame index 2, same as Test B/C above */
    CHECK(loud[2 * 2] > 20000, "SPU2 mixer: self-looping voice 1 reaches strong non-silent output before KOFF");

    iop_spu2_mmio_write16(iop_spu2_core_reg_addr(0, SPU2_C_KOFF0), 0x0002); /* KOFF voice 1 */

    /* Real Release (fast shift, mode=Linear, always-decreasing) should
     * drive the level to exactly 0 (and the voice inactive) within a
     * small, bounded number of ticks - render generously past that. */
    int16_t rel[64 * 2];
    for (int pass = 0; pass < 8; pass++) spu2_mixer_render_frames(rel, 64); /* 512 frames total */

    int16_t final_check[4 * 2];
    spu2_mixer_render_frames(final_check, 4);
    int released_silent = 1;
    for (int i = 0; i < 8; i++) if (final_check[i] != 0) released_silent = 0;
    CHECK(released_silent,
          "SPU2 mixer: real KOFF drives Release envelope to 0 and the voice goes inactive (silent) thereafter");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
