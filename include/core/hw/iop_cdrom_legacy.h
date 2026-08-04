#ifndef PCSX2_WII_IOP_CDROM_LEGACY_H
#define PCSX2_WII_IOP_CDROM_LEGACY_H

#include <stdint.h>
#include "core/iso_loader.h"

/*
 * iop_cdrom_legacy.h - PS1-legacy CD-ROM controller: real command/
 * response protocol (Round 145, task #172/#298, 185th finding).
 *
 * Real hardware base address: 0x1F801800-0x1F801803, a completely
 * separate register block from this project's already-modeled,
 * PS2-native CDVD page (0x1F402xxx, iop_cdvd.c/h). This is the
 * original PS1 CD-ROM controller, kept present on real IOP hardware
 * for PS1-backward-compatibility mode - see psx-spx's "CDROM Drive"
 * page (https://psx-spx.consoledev.net/cdromdrive/), the single,
 * comprehensive, real, directly-fetched citation for everything in
 * this file: the bank-switched register layout, the command/
 * response FIFO protocol, the INT0-INT5 interrupt-cause convention,
 * the status(stat) byte bit layout, and the full command opcode
 * table (parameters, acknowledge/completion response bytes).
 *
 * HISTORY: Round 133 (173rd finding) modeled only the single status
 * bit (PRMEMPT) this project's own traced boot-time polling loop
 * needed, deliberately declining to fabricate the rest of the real
 * protocol without a citation. Round 145 closes that gap for real,
 * following the user's explicit "bau sie komplett ein" (build them
 * in completely) instruction after Round 144 confirmed neither the
 * CD-ROM nor memory-card subsystem could ever satisfy a real
 * completion-event wait as bare register scaffolds.
 *
 * REAL REGISTER BANKING (psx-spx, directly cited): the interface is
 * bank-switched, four banks of four 8-bit registers. 0x1F801800 is
 * ADDRESS on write (bits 0-1 select the bank, R/W) and HSTS on read
 * (mirrors ADDRESS's bank bits plus five real status flags -
 * ADPBUSY/PRMEMPT/PRMWRDY/RSLRRDY/DRQSTS/BUSYSTS). The other three
 * registers' meaning depends on the selected bank AND on whether the
 * access is a read or a write - this project models bank 0 (COMMAND/
 * PARAMETER/HCHPCTL on write, RESULT/RDDATA on read - the command
 * dispatch, parameter FIFO, and result FIFO path every real CD-ROM
 * driver uses) and bank 1 (HINTMSK/HCLRCTL - the real interrupt
 * enable/acknowledge protocol) fully; banks 2/3 (CI/ATV0-3/ADPCTL -
 * audio mixing volume-level registers, used only for real CD-DA/XA-
 * ADPCM audio streaming) are accepted as plain read/write storage
 * with no side effects, since this project does not implement real
 * audio sector streaming (matches the existing, already-honest
 * iop_spu2.c/iop_spu_legacy.c "register scaffold" scope for audio
 * hardware elsewhere in this project).
 *
 * REAL INTERRUPT-CAUSE PROTOCOL (psx-spx, directly cited): bits 0-2
 * of HINTSTS are NOT three independent flags - the HC05 controller
 * uses them as a single 3-bit "interrupt type" value:
 *   INT0 NoIntr      no interrupt pending
 *   INT1 DataReady   new sector (ReadN/ReadS) or report packet ready
 *   INT2 Complete    command finished (second response, some cmds)
 *   INT3 Acknowledge command received and acknowledged (all cmds)
 *   INT4 DataEnd     reached end of disc/track
 *   INT5 DiskError   command error, read error, or lid opened
 * HINTMSK (bits 0-2, ENINT) gates which of these actually raise the
 * real IOP_IRQ_CDROM (2) hardware line via iop_intc_raise(2) - real
 * hardware ORs (HINTMSK & HINTSTS); this project follows suit.
 * HCLRCTL bit0-2 (CLRINT) acknowledges/clears HINTSTS, matching real
 * "write 07h to reset the HC05 interrupt flags" usage; after
 * acknowledge, a queued second response (e.g. Init/Standby/Stop/
 * Pause/SeekL/SeekP/Setsession/ReadTOC/GetID's real INT2-after-INT3
 * sequencing) is delivered exactly as real hardware does.
 *
 * REAL COMMAND SET IMPLEMENTED (psx-spx's "Command Summary" table,
 * directly cited opcodes/params/responses): Nop(01h), Setloc(02h),
 * Play(03h, ack-only - no real audio streaming, see scope note),
 * ReadN(06h)/ReadS(1Bh) (real sector data via iso_loader.c, Round
 * 139, if a disc image is mounted - see iop_cdrom_legacy_mount_iso()
 * - else the real, cited "80h - cannot respond yet... also appears
 * if no disk inserted at all" INT5 error, matching real no-disc
 * hardware behavior honestly rather than silently succeeding),
 * Standby(07h), Stop(08h), Pause(09h), Init(0Ah), Mute(0Bh),
 * Demute(0Ch), Setfilter(0Dh), Setmode(0Eh), Getparam(0Fh),
 * GetlocL(10h), GetlocP(11h), Setsession(12h), GetTN(13h), GetTD
 * (14h), SeekL(15h), SeekP(16h), Test/19h,20h (BIOS date/version
 * only - the one real sub-function psx-spx notes "the Kernel seems
 * to be using"), GetID(1Ah), Reset(1Ch), ReadTOC(1Eh). All other
 * opcodes (0x00, 0x17-0x18, 0x1F, 0x20-0xFF, all 19h sub-functions
 * besides 20h) return the real, cited generic-unused-opcode response
 * (INT5: stat with Error bit set, error byte 40h "Invalid command"),
 * matching real hardware's own documented behavior for unimplemented
 * opcodes rather than silently ignoring them.
 *
 * NOT implemented (honest scope limit, matching this project's
 * established "real for the concrete case, not full hardware"
 * convention used throughout - see iop_spu2.h/iop_spu_legacy.h/
 * iop_sio2.h for the same pattern): real CD-DA/XA-ADPCM audio
 * playback and streaming (Play/Forward/Backward's real INT1 report-
 * packet stream), sub-channel Q data (GetQ, 1Dh), the secret-unlock
 * command family (50h-57h, region-lock bypass - irrelevant to
 * booting a real, legitimately-licensed disc), Video CD commands
 * (1Fh), and the full 19h test/debug sub-function catalogue beyond
 * the one (20h) real BIOS code is cited to actually use. Real IRQ
 * timing (real hardware's documented multi-frame command latency) is
 * NOT modeled - this project's commands complete on the same IOP
 * step they're issued, matching this project's iop_dma.c/iop_timers.c
 * precedent of prioritizing correct eventual state over cycle-exact
 * timing.
 */

#define IOP_CDROM_LEGACY_BASE 0x1F801800u
#define IOP_CDROM_LEGACY_SIZE 0x0004u

/* Real, cited bit values for the Index/Status Register (ADDRESS on
 * write, HSTS on read). */
#define IOP_CDROM_LEGACY_STATUS_ADPBUSY  0x04u
#define IOP_CDROM_LEGACY_STATUS_PRMEMPT  0x08u
#define IOP_CDROM_LEGACY_STATUS_PRMWRDY  0x10u
#define IOP_CDROM_LEGACY_STATUS_RSLRRDY  0x20u
#define IOP_CDROM_LEGACY_STATUS_DRQSTS   0x40u
#define IOP_CDROM_LEGACY_STATUS_BUSYSTS  0x80u

/* Real, cited stat(us) byte bits (returned by Nop and most other
 * commands as the first response byte). */
#define IOP_CDROM_STAT_ERROR      0x01u
#define IOP_CDROM_STAT_MOTOR      0x02u
#define IOP_CDROM_STAT_SEEKERROR  0x04u
#define IOP_CDROM_STAT_IDERROR    0x08u
#define IOP_CDROM_STAT_SHELLOPEN  0x10u
#define IOP_CDROM_STAT_READ       0x20u
#define IOP_CDROM_STAT_SEEK       0x40u
#define IOP_CDROM_STAT_PLAY       0x80u

/* Real, cited INTSTS "interrupt type" values (bits 0-2 of HINTSTS). */
#define IOP_CDROM_INT_NONE 0u
#define IOP_CDROM_INT1     1u
#define IOP_CDROM_INT2     2u
#define IOP_CDROM_INT3     3u
#define IOP_CDROM_INT4     4u
#define IOP_CDROM_INT5     5u

void iop_cdrom_legacy_init(void);

/* Optional: mount a real ISO9660/BIN disc image (via iso_loader.c,
 * Round 139) so ReadN/ReadS/GetlocL/GetTN/GetTD/GetID/ReadTOC return
 * real, disc-backed data instead of the real, cited "no disc" error
 * response. NOT called by default/at init - this project's verified
 * boot-progress history (Round 130+) is a diskless BIOS-only
 * scenario (see iso_loader.h's own scope note); calling this is a
 * separate, deliberate opt-in for future live-boot-with-disc work or
 * for testing the real read path. Returns 0 on success, -1 on
 * failure (matches iso_open()'s own convention). */
int iop_cdrom_legacy_mount_iso(const char *path);
void iop_cdrom_legacy_unmount_iso(void);
int iop_cdrom_legacy_rebind_iso(const char *path); /* Round 449 - checkpoint-resume-safe reopen, see .c citation */

/* Same convention as every other *_mmio_read8/write8 helper in this
 * project: returns 1 and fills *out if addr falls in the modeled
 * range, 0 otherwise. */
int iop_cdrom_legacy_mmio_read8(uint32_t addr, uint8_t *out);
int iop_cdrom_legacy_mmio_write8(uint32_t addr, uint8_t value);

#endif
