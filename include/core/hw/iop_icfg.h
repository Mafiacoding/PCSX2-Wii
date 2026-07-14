/*
 * iop_icfg.h - IOP "ICFG" configuration register (real PS2 IOP
 * hardware address 0x1F801450, per PCSX2's pcsx2/IopHw.h:
 * "HW_ICFG = 0x1f801450").
 *
 * Task #214 (task #172 continuation, 85th finding): root-causing the
 * EE poll loop at pc=0x8000CFD0-0x8000CFD4 (see docs/STATUS.md) found
 * it waits on EE INTC_STAT bit 1 (real INTC_SBUS, per PCSX2's
 * pcsx2/Dmac.h "enum INTCIrqs { INTC_GS=0, INTC_SBUS, ... }" and
 * ps2sdk's ee/kernel/include/kernel.h identical real ten-source
 * list), which this project never raised. Tracing real PCSX2 source
 * (pcsx2/ps2/Iop/IopHwWrite.cpp's `_HwWrite_16or32_Page1()`, the
 * `case 0x450:` branch - masked_addr 0x450 corresponds to the real
 * full IOP address 0x1f801450/HW_ICFG since that function only
 * handles addresses already prefixed 0x1f801xxx) found the EXACT
 * real, cited mechanism:
 *
 *   case 0x450:
 *       psxHu(addr) = val;
 *       if (val & (1 << 1))
 *       {
 *           hwIntcIrq(INTC_SBUS);
 *       }
 *       break;
 *
 * i.e. real hardware/PCSX2 raises the EE's INTC_SBUS (bit 1) the
 * moment the IOP writes a value with bit 1 (0x2) set to this exact
 * address. ps2sdk's common/include/iop_regs.h names this same address
 * `GM_IF` ("Bit 31 of GM_IF is for the IOP type") - a real IOP
 * configuration/identification register separate from the SIF SBUS
 * mailbox block (SBUS_F2xx/0x1000F2xx-0x1000F260, already modeled in
 * sif.c/sif.h) - modeled here, not there, to keep the "IOP
 * configuration register" and "SIF mailbox" real hardware blocks
 * distinct even though both ultimately cross the EE/IOP boundary.
 *
 * SCOPE: only the one real, cited write-side effect above is
 * modeled. The read side (GM_IF's real bit-31 IOP-type indicator) is
 * a plain passthrough of whatever was last written/reset-to-zero -
 * this project has no confirmed citation for which specific IOP-type
 * value real OSDSYS/BIOS code expects to read back here, so rather
 * than fabricate one, reads simply return stored state (0 after
 * reset), an honest "unknown, not modeled" gap.
 */
#ifndef PCSX2_WII_IOP_ICFG_H
#define PCSX2_WII_IOP_ICFG_H

#include <stdint.h>

void iop_icfg_init(void);

/* Returns 1 and fills *out if addr (any KUSEG/KSEG0/KSEG1 alias) is
 * the real IOP ICFG address 0x1F801450, 0 otherwise - same
 * convention as sif_iop_mmio_read32/iop_intc_mmio_read32. */
int iop_icfg_mmio_read32(uint32_t addr, uint32_t *out);
int iop_icfg_mmio_write32(uint32_t addr, uint32_t value);

#endif
