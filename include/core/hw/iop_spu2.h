#ifndef PCSX2_WII_IOP_SPU2_H
#define PCSX2_WII_IOP_SPU2_H

#include <stdint.h>

/*
 * iop_spu2.h - SPU2 (sound processor) register scaffold (task #95,
 * "time permitting").
 *
 * SCOPE, read before extending: this is a register-file SCAFFOLD, not
 * an audio implementation. What IS real and cited: the real IOP-side
 * base address (0x1F900000 - consistent across every public PS2
 * hardware reference this project is aware of, e.g. ps2tek/PCSX2's
 * own IopHw address map) and the fact that real SPU2 registers are
 * natively 16-BIT (unlike most other IOP peripherals this project has
 * modeled so far, which are 32-bit) - real IOP code accesses them via
 * LH/SH, not LW/SW. What is NOT implemented: any per-register meaning
 * (voice VOLL/VOLR/PITCH/ADSR, core MMIX/master-volume/control,
 * ENDX flags, the two real per-core offsets, etc - this project does
 * not have a verified, cited exact register offset table the way
 * iop_intc.h's I_STAT/I_MASK/I_CTRL layout was directly ported from
 * real PCSX2 source) and, critically, no actual audio synthesis or
 * DMA-to-SPU2 data pipeline of any kind. What this DOES provide: any
 * real BIOS/game IOP code that reads or writes an SPU2 register in
 * the real address range now lands in a real, persistent, byte-
 * addressable 16-bit-granularity register file (readback returns
 * whatever was last written) instead of silently falling through to
 * unrelated IOP RAM/BIOS memory or being dropped - a genuine step
 * from "unmodeled address range" to "real, if semantically inert,
 * hardware register block," matching the same honest-scaffold pattern
 * this project already used for iop_hle_modules.c's registry before
 * task #92 made it real.
 *
 * Size: 0x800 bytes (2KB) - covers real Core0's full documented
 * register block plus headroom; Core1 (real offset +0x400 from
 * Core0 on actual hardware) falls within this same window since this
 * scaffold does not yet distinguish core-specific semantics anyway.
 */

#define IOP_SPU2_BASE 0x1F900000u
#define IOP_SPU2_SIZE 0x0800u

/*
 * ---------------------------------------------------------------
 * Round 185 addition: real per-register offset/meaning table
 * ---------------------------------------------------------------
 * Sources (architectural facts only, paraphrased, no verbatim
 * excerpts per this project's clean-room discipline; direct quotes
 * kept under 15 words where used):
 *   - ps2tek (https://psi-rockin.github.io/ps2tek/) memory-map page:
 *     confirms two independent cores, Core1's register block starting
 *     exactly +0x400 past Core0's.
 *   - PCSX2's historical ZeroSPU2 plugin header (GPL-2.0, same
 *     citation tier already used elsewhere in this project for real
 *     PCSX2-derived architectural facts), reg.h: independently
 *     confirms the same +0x400 Core1 offset for every register, and
 *     provides the per-voice and core-level register names/offsets
 *     below.
 *   - PCSX2's own wiki ("SPU2 is more than just sound!" page): notes
 *     SPU2 timing/behavior is commonly relied upon by real games, but
 *     does NOT name any specific boot-gate status bit or register.
 *
 * NOT documented by any of the above (explicitly - not an oversight):
 * real power-on/reset values for any SPU2 register (this project's
 * existing memset-to-zero `iop_spu2_init()` is therefore the most
 * defensible default, not a citable "real reset value"), and no
 * specific boot-time "audio ready" polling condition. Nothing in this
 * round should be read as claiming otherwise.
 *
 * SCOPE: this is a NAMING/ADDRESSING increment only, same "real
 * address space, no fabricated hardware behavior" discipline as the
 * original scaffold. The underlying storage remains the existing
 * flat, byte-addressable register file (`iop_spu2_mmio_*`) - every
 * offset below is READ/WRITE passthrough, identical to the pre-
 * existing behavior, just now given a real, cited, symbolic name
 * instead of being an anonymous blob. No envelope generation, no
 * hardware-driven ENDX/status auto-set, no actual audio synthesis or
 * DMA-to-SPU2 pipeline - unchanged, honest scope limit from the
 * original header (task #95/Round 136 precedent).
 *
 * Per-voice register block (24 voices, stride 0x10, base = core
 * base + 0x000):
 */
#define SPU2_CORE1_OFFSET 0x400u /* add to every offset below for Core1 */

#define SPU2_VOICE_STRIDE   0x10u
#define SPU2_VOICE_COUNT    24u
#define SPU2_V_VOLL         0x00u /* Voice Volume Left */
#define SPU2_V_VOLR         0x02u /* Voice Volume Right */
#define SPU2_V_PITCH        0x04u /* Pitch */
#define SPU2_V_ADSR1        0x06u /* ADSR envelope spec 1 */
#define SPU2_V_ADSR2        0x08u /* ADSR envelope spec 2 */
#define SPU2_V_ENVX         0x0Au /* current envelope value */
#define SPU2_V_VOLXL        0x0Cu /* current voice volume left */
#define SPU2_V_VOLXR        0x0Eu /* current voice volume right */

/* Per-voice address block (24 voices, stride 0x0C, base = core base +
 * 0x1C0) - real 32-bit addresses split into hi/lo 16-bit halves. */
#define SPU2_VADDR_BASE     0x1C0u
#define SPU2_VADDR_STRIDE   0x0Cu
#define SPU2_VA_SSA_HI      0x00u /* waveform start address, hi */
#define SPU2_VA_SSA_LO      0x02u /* waveform start address, lo */
#define SPU2_VA_LSA_HI      0x04u /* loop-point address, hi */
#define SPU2_VA_LSA_LO      0x06u /* loop-point address, lo */
#define SPU2_VA_NAX_HI      0x08u /* next address to be read, hi */
#define SPU2_VA_NAX_LO      0x0Au /* next address to be read, lo */

/* Core-level control/status registers (base = core base + these
 * offsets). */
#define SPU2_C_FMOD1        0x180u /* pitch modulation spec 1 */
#define SPU2_C_FMOD2        0x182u /* pitch modulation spec 2 */
#define SPU2_C_NON          0x184u /* noise generator enable */
#define SPU2_C_VMIXL        0x188u /* voice mix dry/wet, left */
#define SPU2_C_VMIXR        0x190u /* voice mix dry/wet, right */
#define SPU2_C_MMIX         0x198u /* output spec after voice mix */
#define SPU2_C_CTRL         0x19Au /* core control/attribute register */
#define SPU2_C_IRQA_HI      0x19Cu /* interrupt address, hi */
#define SPU2_C_IRQA_LO      0x19Eu /* interrupt address, lo */
#define SPU2_C_KON0         0x1A0u /* key on, voices 0-15 */
#define SPU2_C_KON1         0x1A2u /* key on, voices 16-23 */
#define SPU2_C_KOFF0        0x1A4u /* key off, voices 0-15 */
#define SPU2_C_KOFF1        0x1A6u /* key off, voices 16-23 */
#define SPU2_C_TSA_HI       0x1A8u /* DMA transfer address, hi */
#define SPU2_C_TSA_LO       0x1AAu /* DMA transfer address, lo */
#define SPU2_C_TDATA        0x1ACu /* transfer data FIFO */
#define SPU2_C_ADMAS        0x1B0u /* AutoDMA status */
#define SPU2_C_ENDX0        0x340u /* end-point-passed flags, voices 0-15 */
#define SPU2_C_ENDX1        0x342u /* end-point-passed flags, voices 16-23 */
#define SPU2_C_STATX        0x344u /* status register (real meaning of
                                     * individual bits not conclusively
                                     * documented by any source checked -
                                     * modeled as a plain register, no
                                     * bit-level semantics assumed) */

/* Shared (non-per-core) registers - single instance, NOT affected by
 * SPU2_CORE1_OFFSET, at absolute offsets from IOP_SPU2_BASE. */
#define SPU2_S_MVOLL        0x760u /* master volume left */
#define SPU2_S_MVOLR        0x762u /* master volume right */
#define SPU2_S_EVOLL        0x764u /* effect volume left */
#define SPU2_S_EVOLR        0x766u /* effect volume right */

void iop_spu2_init(void);

/* Same convention as every other *_mmio_read16/write16 helper in this
 * project: returns 1 and fills *out if addr falls in the modeled
 * range, 0 otherwise. */
int iop_spu2_mmio_read16(uint32_t addr, uint16_t *out);
int iop_spu2_mmio_write16(uint32_t addr, uint16_t value);

/* 32-bit accessors, since some real IOP code (and this project's own
 * iop_mem_read32/write32 path) may still touch these addresses with
 * LW/SW despite the real hardware being 16-bit-native - handled here
 * as two adjacent 16-bit register slots rather than left unmodeled. */
int iop_spu2_mmio_read32(uint32_t addr, uint32_t *out);
int iop_spu2_mmio_write32(uint32_t addr, uint32_t value);

/* Round 185 convenience helpers: compute the real absolute address
 * for a named per-voice / per-voice-address / core-level register
 * (see the offset table above), then read/write it via the existing
 * byte-addressable register file. core: 0 or 1. voice: 0-23. These
 * are pure address arithmetic + passthrough - no new hardware
 * behavior, matching this file's stated scope. */
uint32_t iop_spu2_voice_reg_addr(int core, int voice, uint32_t voice_reg_offset);
uint32_t iop_spu2_voice_addr_reg_addr(int core, int voice, uint32_t vaddr_reg_offset);
uint32_t iop_spu2_core_reg_addr(int core, uint32_t core_reg_offset);

#endif
