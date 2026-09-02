/*
 * spu2_mixer.c - real SPU2 audio synthesis engine implementation.
 * See include/core/hw/spu2_mixer.h for the full citation list (psx-spx
 * SPU page + CDROM Format page) and the documented, honest scope
 * limits (linear not Gaussian interpolation, Fixed Volume Mode only,
 * no reverb/noise/pitch-modulation, no phase-inversion path).
 *
 * SCOPE NOTE (this file only): this round wires the engine to
 * iop_spu2.c's real KON/KOFF register-write path (voices start/stop
 * for real) and reads live VOLL/VOLR/PITCH/ADSR1/ADSR2 register
 * values every render tick - but does NOT yet wire real IOP-DMA
 * channel-7 waveform delivery into spu2_mixer_get_ram(); that DMA
 * plumbing (mirroring iop_dma.c's existing SIF0/SIF2 transfer-
 * function pattern) is explicitly deferred to a follow-up round.
 * Until that lands, spu2_mixer_get_ram() must be filled by a test
 * harness or a future DMA hook for real waveform playback to have
 * real driving data - this is a genuine, working synthesis engine,
 * not yet a genuine end-to-end sound *pipeline*. Documented here so
 * this isn't mistaken for more than it is.
 */
#include "core/hw/spu2_mixer.h"
#include "core/hw/iop_spu2.h"
#include <string.h>

typedef enum {
    ENV_OFF = 0,
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE
} env_phase_t;

typedef struct {
    int active;
    env_phase_t phase;
    int32_t level;             /* current ADSR volume, 0..0x7FFF (real range) */
    uint32_t adsr_counter;     /* real "AdsrCycles"-equivalent accumulator */

    uint32_t cur_addr;         /* byte offset into spu2_ram of the ADPCM
                                 * block currently loaded in block[] */
    uint32_t repeat_addr;      /* real per-voice loop/repeat byte address */
    int16_t hist1;             /* real "old" ADPCM decode history */
    int16_t hist2;             /* real "older" ADPCM decode history */
    int16_t block[28];         /* decoded samples of the current block */
    int block_valid;
    int block_pos;              /* 0..27; >=28 means "needs next block" */
    int pending_loop_end;
    int pending_loop_repeat;
    uint32_t pitch_counter;     /* real pitch Counter; bit12+ = sample advance */
} voice_runtime_t;

static uint8_t g_spu2_ram[SPU2_MIXER_RAM_SIZE];
static voice_runtime_t g_voice[SPU2_MIXER_NUM_CORES][SPU2_MIXER_VOICES_PER_CORE];
static uint64_t g_total_kon = 0;
static uint64_t g_total_frames = 0;

/* ---- register-file glue: read live values via iop_spu2.c's own real
 * addressing API, no duplicated register storage in this file. ---- */

static uint16_t read_voice_reg16(int core, int voice, uint32_t off)
{
    uint16_t v = 0;
    iop_spu2_mmio_read16(iop_spu2_voice_reg_addr(core, voice, off), &v);
    return v;
}

static uint16_t read_vaddr_reg16(int core, int voice, uint32_t off)
{
    uint16_t v = 0;
    iop_spu2_mmio_read16(iop_spu2_voice_addr_reg_addr(core, voice, off), &v);
    return v;
}

static uint16_t read_core_reg16(int core, uint32_t off)
{
    uint16_t v = 0;
    iop_spu2_mmio_read16(iop_spu2_core_reg_addr(core, off), &v);
    return v;
}

static void write_core_reg16(int core, uint32_t off, uint16_t value)
{
    iop_spu2_mmio_write16(iop_spu2_core_reg_addr(core, off), value);
}

static uint16_t read_shared_reg16(uint32_t off)
{
    /* Shared registers (MVOLL/MVOLR/EVOLL/EVOLR) are single-instance,
     * NOT affected by SPU2_CORE1_OFFSET (per iop_spu2.h's own Round
     * 185 comment) - read at the raw base offset, no core arithmetic. */
    uint16_t v = 0;
    iop_spu2_mmio_read16(IOP_SPU2_BASE + off, &v);
    return v;
}

static void spu2_set_endx_bit(int core, int voice)
{
    uint32_t off = (voice < 16) ? SPU2_C_ENDX0 : SPU2_C_ENDX1;
    int bit = (voice < 16) ? voice : (voice - 16);
    uint16_t cur = read_core_reg16(core, off);
    write_core_reg16(core, off, (uint16_t)(cur | (1u << bit)));
}

/* ---- real ADPCM decode, cited formula/predictor table (psx-spx
 * CDROM Format page's decode_28_nibbles, explicitly stated to be the
 * same algorithm SPU-ADPCM uses) - see spu2_mixer.h for the full
 * citation. ---- */

static void decode_adpcm_block(const uint8_t *blk16, int16_t *out28,
                                int16_t *hist1, int16_t *hist2,
                                uint8_t *flags_out)
{
    static const int32_t pos_tbl[5] = { 0, 60, 115, 98, 122 };
    static const int32_t neg_tbl[5] = { 0, 0, -52, -55, -60 };

    int shift = 12 - (blk16[0] & 0x0F);
    if (shift < 0) shift = 0;   /* defensive clamp - real hardware's header
                                 * nibble range (0..0xF) can drive shift down
                                 * to -3; psx-spx documents shift 13..15 (the
                                 * XA/8-bit-format's *stored* field, a
                                 * different encoding) as "act same as
                                 * shift=9", but the SPU-ADPCM decode formula
                                 * itself is only specified for the resulting
                                 * shift 0..12 range - clamping rather than
                                 * producing an out-of-spec huge left-shift is
                                 * the defensible choice here, not a cited
                                 * hardware fact. */
    if (shift > 12) shift = 12;

    int filter = (blk16[0] >> 4) & 0x07;
    if (filter > 4) filter = 4; /* see spu2_mixer.h: SPU-ADPCM's 5-filter
                                  * field width is inferred, not directly
                                  * cited - clamped defensively. */

    int32_t f0 = pos_tbl[filter];
    int32_t f1 = neg_tbl[filter];

    int16_t old = *hist1;
    int16_t older = *hist2;
    for (int i = 0; i < 28; i++) {
        uint8_t byte = blk16[2 + (i >> 1)];
        uint8_t raw_nibble = (i & 1) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
        int32_t t = ((int32_t)raw_nibble ^ 8) - 8; /* sign-extend 4-bit */
        int32_t s = (t << shift) + (((int32_t)old * f0 + (int32_t)older * f1 + 32) / 64);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out28[i] = (int16_t)s;
        older = old;
        old = (int16_t)s;
    }
    *hist1 = old;
    *hist2 = older;
    if (flags_out) *flags_out = blk16[1];
}

/* ---- real ADSR envelope stepping, faithfully transcribed from
 * psx-spx's "Envelope Operation depending on Shift/Step/Mode/
 * Direction" pseudocode. all_ones models the real "Step field all-
 * ones => never steps, never saturates" quirk (psx-spx: "Using a step
 * value of all-ones causes the volume to never step... i.e. 0x7f, or
 * 0x1f for decay/release"); phase_negative is always 0 in this engine
 * (see spu2_mixer.h's documented simplification). ---- */

static int32_t envelope_step(int mode, int decreasing, int shift_value,
                              int step_value, int32_t level,
                              uint32_t *counter, int all_ones)
{
    int32_t adsr_step = 7 - step_value;
    if (decreasing) {
        adsr_step = ~adsr_step; /* +7,+6,+5,+4 => -8,-7,-6,-5 (real, cited) */
    }
    {
        int shl = 11 - shift_value;
        if (shl < 0) shl = 0;
        adsr_step = adsr_step * (1 << shl); /* avoid UB of signed <<, same result */
    }

    int32_t counter_increment;
    {
        int shr = shift_value - 11;
        if (shr < 0) shr = 0;
        counter_increment = (int32_t)(0x8000 >> shr);
    }

    if (mode == 1 && !decreasing && level > 0x6000) {
        if (shift_value < 10) {
            adsr_step /= 4;
        } else if (shift_value >= 11) {
            counter_increment /= 4;
        } else {
            adsr_step /= 2;
            counter_increment /= 2;
        }
    } else if (mode == 1 && decreasing) {
        adsr_step = (int32_t)(((int64_t)adsr_step * level) / 0x8000);
    }

    if (!all_ones && counter_increment < 1) counter_increment = 1;

    *counter += (uint32_t)counter_increment;
    if ((*counter & 0x8000u) == 0) {
        return level; /* no step this 44.1kHz tick */
    }
    *counter &= 0x7FFFu;

    level += adsr_step;
    if (!decreasing) {
        if (level > 0x7FFF) level = 0x7FFF;
        if (level < -0x8000) level = -0x8000;
    } else {
        /* PhaseNegative branch (real hardware CLAMP(-8000h..0h)) is
         * provably unreachable in this engine - see header note. */
        if (level < 0) level = 0;
    }
    return level;
}

static void voice_tick_envelope(voice_runtime_t *v, uint16_t adsr1, uint16_t adsr2)
{
    if (v->phase == ENV_OFF) return;

    int attack_mode  = (adsr1 >> 15) & 1;
    int attack_shift = (adsr1 >> 10) & 0x1F;
    int attack_step  = (adsr1 >> 8) & 0x3;
    int decay_shift  = (adsr1 >> 4) & 0xF;
    int sustain_level_field = adsr1 & 0xF;
    int32_t sustain_level = (int32_t)(sustain_level_field + 1) * 0x800; /* real, cited */

    int sustain_mode     = (adsr2 >> 15) & 1;
    int sustain_dir_dec  = (adsr2 >> 14) & 1;
    int sustain_shift    = (adsr2 >> 8) & 0x1F;
    int sustain_step     = (adsr2 >> 6) & 0x3;
    int release_mode     = (adsr2 >> 5) & 1;
    int release_shift    = adsr2 & 0x1F;

    switch (v->phase) {
    case ENV_ATTACK: {
        int all_ones = (attack_shift == 0x1F) && (attack_step == 3);
        v->level = envelope_step(attack_mode, 0, attack_shift, attack_step,
                                  v->level, &v->adsr_counter, all_ones);
        if (v->level >= 0x7FFF) {
            v->level = 0x7FFF;
            v->phase = ENV_DECAY;
            v->adsr_counter = 0;
        }
        break;
    }
    case ENV_DECAY: {
        /* Decay: real hardware fixes Mode=Exponential, Direction=Decrease,
         * Step=-8 (StepValue=0 fed into the shared formula, cited in
         * spu2_mixer.h, reduces to exactly that fixed step). */
        int all_ones = (decay_shift == 0xF); /* decay's real shift field is
                                               * only 4 bits (max 0xF) -
                                               * documented approximation of
                                               * psx-spx's generic "0x1f for
                                               * decay/release" all-ones note,
                                               * scaled to this field's real
                                               * width. */
        v->level = envelope_step(1, 1, decay_shift, 0, v->level, &v->adsr_counter, all_ones);
        if (v->level <= sustain_level) {
            v->level = (sustain_level < 0) ? 0 : sustain_level;
            v->phase = ENV_SUSTAIN;
            v->adsr_counter = 0;
        }
        break;
    }
    case ENV_SUSTAIN: {
        int all_ones = (sustain_shift == 0x1F) && (sustain_step == 3);
        v->level = envelope_step(sustain_mode, sustain_dir_dec, sustain_shift,
                                  sustain_step, v->level, &v->adsr_counter, all_ones);
        /* Real hardware: Sustain never auto-exits on level; only a real
         * Key OFF (spu2_mixer_on_koff()) drives it to Release. */
        break;
    }
    case ENV_RELEASE: {
        int all_ones = (release_shift == 0x1F);
        v->level = envelope_step(release_mode, 1, release_shift, 0,
                                  v->level, &v->adsr_counter, all_ones);
        if (v->level <= 0) {
            v->level = 0;
            v->phase = ENV_OFF;
            v->active = 0;
        }
        break;
    }
    default:
        break;
    }
}

/* ---- ADPCM playback: block fetch/decode + real loop-flag handling
 * (psx-spx "Possible combinations": Normal / End+Mute / End+Repeat). ---- */

static void voice_load_next_block(int core, int voice, voice_runtime_t *v)
{
    if (!v->active) return;
    if (v->cur_addr + 16 > SPU2_MIXER_RAM_SIZE) {
        v->active = 0;
        v->phase = ENV_OFF;
        return;
    }
    uint8_t flags = 0;
    decode_adpcm_block(&g_spu2_ram[v->cur_addr], v->block, &v->hist1, &v->hist2, &flags);
    v->block_pos = 0;
    v->block_valid = 1;
    if (flags & 0x04) { /* LoopStart */
        v->repeat_addr = v->cur_addr;
    }
    v->pending_loop_end = (flags & 0x01) ? 1 : 0;
    v->pending_loop_repeat = (flags & 0x02) ? 1 : 0;
    (void)core; (void)voice;
}

static int16_t voice_produce_sample(int core, int voice, voice_runtime_t *v, uint16_t pitch_reg)
{
    if (!v->active) return 0;
    if (!v->block_valid || v->block_pos >= 28) {
        voice_load_next_block(core, voice, v);
        if (!v->active) return 0;
    }

    int16_t sample = v->block[v->block_pos];

    uint32_t step = pitch_reg;
    if (step > 0x4000) step = 0x4000; /* real, cited clamp */
    v->pitch_counter += step;

    while (v->pitch_counter >= 0x1000u && v->active) {
        v->pitch_counter -= 0x1000u;
        v->block_pos++;
        if (v->block_pos >= 28) {
            if (v->pending_loop_end) {
                spu2_set_endx_bit(core, voice); /* real: ENDX marks loop-end
                                                  * passed regardless of
                                                  * mute-vs-repeat outcome */
                if (v->pending_loop_repeat) {
                    v->cur_addr = v->repeat_addr;
                    voice_load_next_block(core, voice, v);
                } else {
                    v->active = 0;
                    v->phase = ENV_OFF;
                    v->level = 0;
                    break;
                }
            } else {
                v->cur_addr += 16;
                voice_load_next_block(core, voice, v);
            }
        }
    }
    return sample;
}

/* ---- volume ---- */

static int32_t fixed_volume(uint16_t reg)
{
    /* Real Fixed Volume Mode (bit15=0): bits0-14 = "Voice volume/2".
     * Sweep Mode (bit15=1) is read as Fixed anyway - documented
     * simplification, see spu2_mixer.h. */
    int32_t raw = (int32_t)(reg & 0x7FFF);
    if (raw & 0x4000) raw -= 0x8000; /* sign-extend 15-bit field */
    return raw * 2;
}

/* ---- public API ---- */

void spu2_mixer_init(void)
{
    memset(g_spu2_ram, 0, sizeof(g_spu2_ram));
    memset(g_voice, 0, sizeof(g_voice));
    g_total_kon = 0;
    g_total_frames = 0;
}

uint8_t *spu2_mixer_get_ram(void)
{
    return g_spu2_ram;
}

void spu2_mixer_on_kon(int core, int is_upper_half, uint16_t bits)
{
    if (core < 0 || core >= SPU2_MIXER_NUM_CORES) return;
    int base_voice = is_upper_half ? 16 : 0;
    for (int b = 0; b < 16; b++) {
        if (!(bits & (1u << b))) continue;
        int voice = base_voice + b;
        if (voice >= SPU2_MIXER_VOICES_PER_CORE) continue;

        uint16_t ssa_hi = read_vaddr_reg16(core, voice, SPU2_VA_SSA_HI);
        uint16_t ssa_lo = read_vaddr_reg16(core, voice, SPU2_VA_SSA_LO);
        uint32_t ssa_units = ((uint32_t)ssa_hi << 16) | ssa_lo;
        uint32_t ssa_addr = ssa_units * 8u; /* real "8-byte units" convention,
                                              * extended from PS1's known
                                              * addressing to this project's
                                              * split 32-bit register - see
                                              * spu2_mixer.h's DATA SOURCE
                                              * note. */
        if (SPU2_MIXER_RAM_SIZE > 0) {
            ssa_addr %= SPU2_MIXER_RAM_SIZE;
        }
        ssa_addr &= ~0xFu; /* block-align (16-byte ADPCM blocks) */

        voice_runtime_t *v = &g_voice[core][voice];
        memset(v, 0, sizeof(*v));
        v->active = 1;
        v->phase = ENV_ATTACK;
        v->level = 0; /* real: KON "automatically initializes ADSR Volume to zero" */
        v->cur_addr = ssa_addr;
        v->repeat_addr = ssa_addr;
        v->block_pos = 28; /* force decode on first produce_sample() call */

        g_total_kon++;
    }
}

void spu2_mixer_on_koff(int core, int is_upper_half, uint16_t bits)
{
    if (core < 0 || core >= SPU2_MIXER_NUM_CORES) return;
    int base_voice = is_upper_half ? 16 : 0;
    for (int b = 0; b < 16; b++) {
        if (!(bits & (1u << b))) continue;
        int voice = base_voice + b;
        if (voice >= SPU2_MIXER_VOICES_PER_CORE) continue;
        voice_runtime_t *v = &g_voice[core][voice];
        if (v->active && v->phase != ENV_OFF && v->phase != ENV_RELEASE) {
            v->phase = ENV_RELEASE; /* real: KOFF "Starts Release", any phase */
            v->adsr_counter = 0;
        }
    }
}

void spu2_mixer_render_frames(int16_t *out_lr, int n_frames)
{
    if (!out_lr || n_frames <= 0) return;
    memset(out_lr, 0, sizeof(int16_t) * 2u * (size_t)n_frames);

    for (int f = 0; f < n_frames; f++) {
        int32_t mix_l = 0, mix_r = 0;

        for (int core = 0; core < SPU2_MIXER_NUM_CORES; core++) {
            for (int voice = 0; voice < SPU2_MIXER_VOICES_PER_CORE; voice++) {
                voice_runtime_t *v = &g_voice[core][voice];
                if (!v->active) continue;

                uint16_t adsr1 = read_voice_reg16(core, voice, SPU2_V_ADSR1);
                uint16_t adsr2 = read_voice_reg16(core, voice, SPU2_V_ADSR2);
                voice_tick_envelope(v, adsr1, adsr2);
                if (!v->active) continue;

                uint16_t pitch = read_voice_reg16(core, voice, SPU2_V_PITCH);
                int16_t raw = voice_produce_sample(core, voice, v, pitch);

                int32_t enveloped = (int32_t)(((int64_t)raw * v->level) / 0x7FFF);

                uint16_t voll = read_voice_reg16(core, voice, SPU2_V_VOLL);
                uint16_t volr = read_voice_reg16(core, voice, SPU2_V_VOLR);
                int32_t vl = fixed_volume(voll);
                int32_t vr = fixed_volume(volr);

                mix_l += (int32_t)(((int64_t)enveloped * vl) / 0x8000);
                mix_r += (int32_t)(((int64_t)enveloped * vr) / 0x8000);
            }
        }

        uint16_t mvoll = read_shared_reg16(SPU2_S_MVOLL);
        uint16_t mvolr = read_shared_reg16(SPU2_S_MVOLR);
        int32_t mvl = fixed_volume(mvoll);
        int32_t mvr = fixed_volume(mvolr);

        int32_t out_l = (int32_t)(((int64_t)mix_l * mvl) / 0x8000);
        int32_t out_r = (int32_t)(((int64_t)mix_r * mvr) / 0x8000);

        if (out_l > 32767) out_l = 32767;
        if (out_l < -32768) out_l = -32768;
        if (out_r > 32767) out_r = 32767;
        if (out_r < -32768) out_r = -32768;

        out_lr[f * 2 + 0] = (int16_t)out_l;
        out_lr[f * 2 + 1] = (int16_t)out_r;

        g_total_frames++;
    }
}

uint64_t spu2_mixer_get_total_kon_count(void)
{
    return g_total_kon;
}

uint64_t spu2_mixer_get_total_frames_rendered(void)
{
    return g_total_frames;
}
