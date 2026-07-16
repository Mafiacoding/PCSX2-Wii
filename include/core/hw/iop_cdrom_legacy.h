#ifndef PCSX2_WII_IOP_CDROM_LEGACY_H
#define PCSX2_WII_IOP_CDROM_LEGACY_H

#include <stdint.h>

/*
 * iop_cdrom_legacy.h - PS1-legacy CD-ROM controller Index/Status
 * register scaffold (Round 133, task #172/#221/#288, 173rd finding).
 *
 * Real hardware base address: 0x1F801800-0x1F801803, a completely
 * separate register block from this project's already-modeled,
 * PS2-native CDVD page (0x1F402xxx, iop_cdvd.c/h). This is the
 * original PS1 CD-ROM controller, kept present on real IOP hardware
 * for PS1-backward-compatibility mode (real, documented public
 * knowledge - see psx-spx's "CDROM Drive" page,
 * https://psx-spx.consoledev.net/cdromdrive/, and the PS2 Developer
 * wiki's memory map, https://www.psdevwiki.com/ps2/Memory_Map, whose
 * IOP DMA table entries 0x1F8010F0/0x1F8010F4 (DPCR/DICR) and
 * 0x1F801018 (CD-ROM BIU auto-increment config) independently
 * cross-confirm this address range against the same device-address
 * table this project traced in Round 132/172nd finding).
 *
 * SCOPE - Round 132 (172nd finding) traced a real IOP boot-time
 * busy-wait to a byte read at exactly this address, but declined to
 * fabricate a bit pattern without first identifying which bit the
 * polling code actually tests (see STATUS.md's 172nd finding,
 * "fabricating a specific bit pattern... would risk exactly the kind
 * of unprincipled hack this project has consistently declined").
 * Round 133 (this file) closes that gap: host-native diagnostic
 * tracing of this project's own raw IOP instruction words at
 * 0x00032C58-0x00032CD8 (self-read via iop_mem_read32, not the live
 * PCSX2 disassembler - see Round 131/132's established caveat that
 * RAM-resident module code differs between a differently-booted live
 * session and this project's own diskless boot) decoded the exact
 * MIPS sequence: `lbu $v0,0($t7)` (byte read through the pointer this
 * project already traced to 0x1F801800) followed by `andi $t8,$v0,8`
 * and a `beq $t8,$at,+3` where `$at==8` - i.e. the loop is gated on
 * `(byte & 0x08) == 0x08`, bit 3 of the Index/Status Register.
 *
 * Per psx-spx's real, documented bit layout for this register (bits
 * 2-7 read-only): bit0-1=RA (register bank), bit2=ADPBUSY, bit3=
 * PRMEMPT (parameter FIFO empty, 1=empty), bit4=PRMWRDY (parameter
 * FIFO not full), bit5=RSLRRDY (result FIFO not empty), bit6=DRQSTS
 * (data request pending), bit7=BUSYSTS (HC05 busy acknowledging a
 * command). Bit 3 is PRMEMPT - "parameter FIFO empty" is the real,
 * documented power-on/idle state of this register (a real driver
 * polls PRMEMPT before writing a new command's parameters, and finds
 * it set immediately on an idle/reset controller with no command in
 * flight - psx-spx's own note that bits 3/4/5 are "bound to 5-bit
 * counters" that become true after a specific count applies to
 * mid-command sequencing this project does not model any part of).
 *
 * This project has never modeled ANY of the other 7 bits, the 3
 * other legacy byte registers in this block (0x1F801801-0x1F801803),
 * or any real PS1 CD-ROM command/response protocol - modeling only
 * the specific bit this project's own traced polling loop needs,
 * exactly matching this project's established "register scaffold for
 * the concrete case at hand, not full hardware" pattern (see
 * iop_cdvd.h/iop_timers.c/iop_spu2.c's own header comments for the
 * same convention). Real PS2 hardware booting a real PS2 disc almost
 * never exercises genuine PS1 CD-ROM controller activity - modeling
 * a fixed idle/reset value here does not pretend to emulate the real
 * command/response state machine, only the one specific status bit
 * this project's own boot trace depends on.
 */

#define IOP_CDROM_LEGACY_BASE 0x1F801800u
#define IOP_CDROM_LEGACY_SIZE 0x0004u

/* Real, cited bit values for the Index/Status Register (psx-spx). */
#define IOP_CDROM_LEGACY_STATUS_ADPBUSY  0x04u
#define IOP_CDROM_LEGACY_STATUS_PRMEMPT  0x08u
#define IOP_CDROM_LEGACY_STATUS_PRMWRDY  0x10u
#define IOP_CDROM_LEGACY_STATUS_RSLRRDY  0x20u
#define IOP_CDROM_LEGACY_STATUS_DRQSTS   0x40u
#define IOP_CDROM_LEGACY_STATUS_BUSYSTS  0x80u

void iop_cdrom_legacy_init(void);

/* Same convention as every other *_mmio_read8/write8 helper in this
 * project: returns 1 and fills *out if addr falls in the modeled
 * range, 0 otherwise. */
int iop_cdrom_legacy_mmio_read8(uint32_t addr, uint8_t *out);
int iop_cdrom_legacy_mmio_write8(uint32_t addr, uint8_t value);

#endif
