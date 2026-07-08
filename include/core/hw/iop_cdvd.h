#ifndef PCSX2_WII_IOP_CDVD_H
#define PCSX2_WII_IOP_CDVD_H

#include <stdint.h>

/*
 * iop_cdvd.h - CDVD (disc drive) register block, "no disc inserted"
 * scaffold (ROADMAP section 7 item "CDVD - disc/BIOS-boot-media
 * emulation").
 *
 * Real IOP-side base address: 0x1F402000 (ps2tek's
 * https://psi-rockin.github.io/ps2tek/#cdvdioports, cross-checked
 * against real PCSX2 source: pcsx2/IopHw.cpp's psxHw4Read8/Write8,
 * whose own log messages say "[segment 0x1f40]" and which mask the
 * address to its low 8 bits before dispatching to cdvdRead/cdvdWrite
 * - i.e. real hardware mirrors these byte registers across the whole
 * 0x1F402000-0x1F402FFF (4KB) page, replicated here by masking the
 * offset to 0xFF). Registers are natively 8-bit (byte-addressed),
 * unlike most other IOP peripherals this project has modeled so far.
 *
 * SCOPE - a real, cited REGISTER BLOCK modeling the specific,
 * concrete case this project currently needs: a diskless, BIOS-only
 * boot. What's real: the register offsets themselves, and their
 * exact power-on-reset values, both ported directly from PCSX2's own
 * pcsx2/CDVD/CDVD.cpp's `cdvdReset()` (GPL-3.0, same "port real
 * semantics, don't reinvent from docs" approach already used for
 * ee_core.c/iop_core.c's opcode tables): DiscType=CDVD_TYPE_NODISC
 * (0x00), Ready=CDVD_DRIVE_READY (0x40), Status=CDVD_STATUS_TRAY_OPEN
 * (0x01), sDataIn=0x40. The ERROR register's real "read clears it"
 * behavior (`cdvdRead06`) is also replicated, since it's simple and
 * directly cited. What's NOT modeled: the real N-command/S-command
 * state machines (seek/read/standby/etc - pcsx2/CDVD/CDVD.cpp's
 * `cdvdWrite04`/`cdvdWrite16` and their many sub-commands), tray
 * open/close transitions, disc-type auto-detection, or any actual
 * data transfer. A write to the NCMD register (offset 0x04) is
 * latched (so real BIOS code polling "did my command register stick"
 * gets a truthful readback) and immediately reports a real,
 * plausible completion via INTR_STAT (Irq_CommandComplete=0, per
 * PCSX2's own CdvdIrqId enum) rather than staying busy forever -
 * this keeps a diskless boot's CDVD status polling loop from spinning
 * indefinitely on an unimplemented command, without pretending to
 * emulate what any specific command actually does. This is the same
 * "register scaffold, not full hardware" pattern already used for
 * iop_timers.c/iop_spu2.c in this project.
 */

#define IOP_CDVD_BASE 0x1F402000u
#define IOP_CDVD_SIZE 0x0100u /* real hardware mirrors across a 4KB
                               * page; this covers the addressable
                               * mirror window (offset masked to 0xFF
                               * by real hardware, see header comment
                               * above) with headroom. */

/* Real, cited constants - see pcsx2/CDVD/CDVD_internal.h. */
#define IOP_CDVD_TYPE_NODISC     0x00u
#define IOP_CDVD_STATUS_TRAY_OPEN 0x01u
#define IOP_CDVD_DRIVE_READY      0x40u
#define IOP_CDVD_IRQ_COMMAND_COMPLETE 0x00u

void iop_cdvd_init(void);

/* Same convention as every other *_mmio_read8/write8 helper in this
 * project: returns 1 and fills *out if addr falls in the modeled
 * range, 0 otherwise. */
int iop_cdvd_mmio_read8(uint32_t addr, uint8_t *out);
int iop_cdvd_mmio_write8(uint32_t addr, uint8_t value);

/* Diagnostic/test accessors - not part of the real hardware register
 * file, exposed so host-native tests can inspect internal state
 * directly without decoding register semantics themselves. */
uint8_t iop_cdvd_get_last_ncommand(void);
uint8_t iop_cdvd_get_status(void);
uint8_t iop_cdvd_get_ready(void);
uint8_t iop_cdvd_get_disc_type(void);

#endif
