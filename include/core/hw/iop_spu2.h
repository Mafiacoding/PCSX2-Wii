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

#endif
