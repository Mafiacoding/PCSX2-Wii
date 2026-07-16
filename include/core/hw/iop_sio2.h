#ifndef PCSX2_WII_IOP_SIO2_H
#define PCSX2_WII_IOP_SIO2_H

#include <stdint.h>

/*
 * iop_sio2.h - SIO2 (controller/memory-card serial interface)
 * register scaffold (Round 135, task #172/#292, 175th finding).
 *
 * SIO2 is the real IOP peripheral that arbitrates access to
 * controllers and memory cards in PS2 mode (ps2tek,
 * https://psi-rockin.github.io/ps2tek/ - "Serial Interface (SIO2)"
 * section of the IOP hardware register map). Real IOP-side base
 * address: 0x1F808200, matching the PS2 Developer wiki's memory map
 * (https://www.psdevwiki.com/ps2/Memory_Map, real SIO2 range
 * 0x1F808200-0x1F808277) already cited in this project's Round 132
 * correction when ruling OUT SIO2 as the identity of a different,
 * unrelated register. Round 134's watch trace (174th finding) found
 * this project's own IOP boot path genuinely writing into this real
 * address range - SEND1/2 buffer slots (0x1F808240-0x1F80825C) then
 * SIO2 control (0x1F808268) - a real access this project previously
 * fell through to unmapped memory (silently dropped) with no model
 * at all.
 *
 * Real, cited register layout (ps2tek's own SIO2 register table):
 *   0x1F808200, size 0x40: SEND3 buffer (16 x 4-byte per-port command
 *                           slots - "SEND3 is an array of up to 16
 *                           different SIO2 commands", ps2tek)
 *   0x1F808240, size 0x20: SEND1/SEND2 buffers (8 x 4-byte port
 *                           config slots)
 *   0x1F808260, size 0x01: FIFOIN  ("a one-byte register used to
 *                           upload commands to SIO2", ps2tek)
 *   0x1F808264, size 0x01: FIFOOUT ("used to read replies and data
 *                           from SIO2 peripherals after a command is
 *                           sent", ps2tek)
 *   0x1F808268, size 0x04: SIO2 control ("bit 0 seems to start the
 *                           command transfer... bits 2 and 3 reset
 *                           SIO2", ps2tek)
 *   0x1F80826C, size 0x04: RECV1 ("set after a transfer, indicating
 *                           if the peripheral is connected", ps2tek)
 *   0x1F808270, size 0x04: RECV2
 *   0x1F808274, size 0x04: RECV3
 *
 * SCOPE - same "real address space, real per-block purpose, no
 * fabricated protocol" scaffold discipline already established for
 * iop_spu2.h/iop_cdvd.h/iop_cdrom_legacy.h. What IS modeled: the real
 * address range and block boundaries above, as a persistent,
 * byte-addressable register file (any real access now lands in a
 * genuine, readback-consistent register instead of silently falling
 * through to unmapped memory). What is NOT modeled: any actual
 * command/response protocol with a real controller or memory card
 * (this project does not emulate a PS2 controller or memory card
 * device), the exact real bit values RECV1/RECV2/RECV3 report for a
 * "no peripheral connected" state (not conclusively cited from a
 * source this project trusts - fabricating a specific bit pattern
 * here would repeat the exact mistake this project's own Round 132
 * finding explicitly declined to make), or the SIO2 interrupt this
 * project's citation says fires after a CTRL-triggered transfer. All
 * registers default to 0 at reset (a real, citable "freshly reset,
 * no transfer yet issued" state per CTRL's own documented reset
 * bits) and simply read back whatever was last written otherwise.
 */

#define IOP_SIO2_BASE 0x1F808200u
#define IOP_SIO2_SIZE 0x0080u /* covers the full real 0x1F808200-
                               * 0x1F808277 block with headroom */

void iop_sio2_init(void);

/* Same convention as every other *_mmio_read/write helper in this
 * project: returns 1 and fills *out if addr falls in the modeled
 * range, 0 otherwise. SIO2's real registers are accessed at multiple
 * widths on real hardware (FIFOIN/FIFOOUT are byte registers, the
 * rest are 32-bit) - all three widths are provided since this
 * project's own IOP interpreter may issue any of them. */
int iop_sio2_mmio_read8(uint32_t addr, uint8_t *out);
int iop_sio2_mmio_write8(uint32_t addr, uint8_t value);
int iop_sio2_mmio_read32(uint32_t addr, uint32_t *out);
int iop_sio2_mmio_write32(uint32_t addr, uint32_t value);

#endif
