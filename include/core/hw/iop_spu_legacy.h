#ifndef PCSX2_WII_IOP_SPU_LEGACY_H
#define PCSX2_WII_IOP_SPU_LEGACY_H

#include <stdint.h>

/*
 * iop_spu_legacy.h - PS1-legacy SPU (Sound Processing Unit) register
 * scaffold (Round 136, task #172/#293, 177th finding).
 *
 * Real IOP-side base address: 0x1F801C00 (psx-spx, "Sound Processing
 * Unit (SPU)", https://psx-spx.consoledev.net/soundprocessingunitspu/
 * - the original PS1 SPU, kept present on real IOP hardware for PS1-
 * backward-compatibility mode). This is a COMPLETELY SEPARATE real
 * hardware block from this project's already-modeled PS2-native SPU2
 * register set (`iop_spu2.c`/`.h`, base 0x1F900000, per ps2tek) -
 * exactly the same "two distinct real hardware blocks, only one
 * modeled" pattern this project already found and fixed for the CD-
 * ROM controller (Round 132/133: PS2-native CDVD vs. PS1-legacy CD-
 * ROM at 0x1F801800). Round 134's watch trace (174th finding) found
 * this project's own IOP boot path genuinely writing into this real
 * address range in a pattern matching a per-voice register reset
 * sweep (a real, documented 0x10-byte stride per voice, matching
 * psx-spx's own voice register layout) - this project had no model
 * for this block at all before this round.
 *
 * Real, cited register layout (psx-spx): 24 voices at a 0x10-byte
 * stride starting at 0x1F801C00, each with Volume Left (+0x00),
 * Volume Right (+0x02), ADPCM Sample Rate/pitch (+0x04), ADPCM Start
 * Address (+0x06), ADSR lo (+0x08), ADSR hi (+0x0A), ADSR current
 * volume (+0x0C), ADPCM Repeat Address (+0x0E) - covering
 * 0x1F801C00-0x1F801D7F (24 * 0x10 = 0x180 bytes). Followed by the
 * SPU control-register page starting at 0x1F801D80 (Main Volume L/R,
 * Reverb Output L/R, Voice ON/OFF, Voice Pitch Modulation, Voice
 * Noise, Voice Reverb, Voice Status, and further control/status
 * registers up to around 0x1F801DBC per psx-spx's own documented
 * layout).
 *
 * SCOPE - same "real address space, real per-block purpose, no
 * fabricated protocol or synthesis" scaffold discipline already
 * established for iop_spu2.h/iop_sio2.h/iop_cdrom_legacy.h. What IS
 * modeled: the real address range as a persistent, byte-addressable,
 * 16-bit-granularity register file (real PS1 SPU registers are
 * natively 16-bit, matching this project's own existing iop_spu2.h
 * precedent for its sibling PS2-native block) - any real access now
 * lands in a genuine, readback-consistent register instead of
 * silently falling through to unmapped memory. What is explicitly
 * NOT provided, and should not be assumed by anyone extending this
 * file: any actual audio synthesis, ADPCM decoding, DMA-to-SPU data
 * pipeline, voice mixing, or real sound output through Wii hardware
 * (libogc's AESND or any other audio API) - "SPU2 = Audio" in the
 * sense of audible sound coming out of Wii speakers is a completely
 * separate, much larger feature this round does not attempt. This
 * scaffold only stops real PS1-legacy SPU register writes (most
 * plausibly the kernel's own boot-time "reset audio hardware" pass)
 * from being silently dropped, matching this project's own existing
 * SPU2 scaffold's honestly-stated limitations.
 *
 * Size: 0x200 bytes (512B) - covers the full real per-voice block
 * (0x180 bytes) plus the documented control-register page with
 * headroom.
 */

#define IOP_SPU_LEGACY_BASE 0x1F801C00u
#define IOP_SPU_LEGACY_SIZE 0x0200u

/*
 * Round 523/524 addition (task #490): real, cited named offsets for
 * the Voice-Flags and Control/Status registers, needed to implement
 * real KON-clears-ENDX and CTRL[5:0]->STATUS[5:0] semantics (see
 * iop_spu_legacy.c). Source: psx-spx
 * (https://psx-spx.consoledev.net/soundprocessingunitspu/), which
 * documents this exact PS1 SPU register block that PS2's IOP keeps
 * present for backward compatibility (see file header above). All
 * five registers below are real 32-bit-wide fields per psx-spx's own
 * "Voice 0..23 Flags (six 1bit flags per voice)" description of the
 * 0x1F801D88-0x1F801D9F region (24 bits used, one per voice).
 */
#define SPU_LEGACY_KON     0x188u /* 1F801D88h, Key ON  (W) - offset from base */
#define SPU_LEGACY_KOFF    0x18Cu /* 1F801D8Ch, Key OFF (W) - offset from base */
#define SPU_LEGACY_ENDX    0x19Cu /* 1F801D9Ch, ENDX status (R) - offset from base */
#define SPU_LEGACY_CTRL    0x1AAu /* 1F801DAAh, SPUCNT control (R/W) - offset from base */
#define SPU_LEGACY_STATUS  0x1AEu /* 1F801DAEh, SPUSTAT status (R) - offset from base */

void iop_spu_legacy_init(void);

/* Same convention as every other *_mmio_read/write helper in this
 * project: returns 1 and fills *out if addr falls in the modeled
 * range, 0 otherwise. Real PS1 SPU registers are 16-bit-native (see
 * header above); 32-bit accessors are also provided since this
 * project's own IOP interpreter may still issue LW/SW against them,
 * matching iop_spu2.h's own existing precedent for its sibling
 * PS2-native block. */
int iop_spu_legacy_mmio_read16(uint32_t addr, uint16_t *out);
int iop_spu_legacy_mmio_write16(uint32_t addr, uint16_t value);
int iop_spu_legacy_mmio_read32(uint32_t addr, uint32_t *out);
int iop_spu_legacy_mmio_write32(uint32_t addr, uint32_t value);

#endif
