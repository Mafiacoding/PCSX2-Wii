#ifndef PCSX2_WII_IOP_CDVD_H
#define PCSX2_WII_IOP_CDVD_H

#include <stdint.h>
#include "core/iso_loader.h"

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

/* Round 347 (IOP RPC re-entry architecture): public re-exports of
 * iop_cdvd.c's own internal, already-cited OFF_NCMD/OFF_NREADY
 * register offsets (ps2tek's real N-command register page - see this
 * header's own existing citation trail for the full source). Needed
 * by ee_core.c's real-dispatch path (ee_check_cdvd_ncmd_pending() and
 * its arming helper) to drive the exact same real MMIO write sequence
 * (param bytes to NREADY's write side, then the real opcode to NCMD)
 * that any real IOP-side caller would use - going through
 * iop_cdvd_mmio_write8() itself, not a shortcut, so every real side
 * effect (dispatch_ncmd(), real IRQ2 raise) happens exactly as it
 * would for a genuine hardware-level write. Distinct macro names from
 * iop_cdvd.c's own private OFF_NCMD/OFF_NREADY - same real values,
 * no symbol collision. */
#define IOP_CDVD_OFF_NCMD   0x04u
#define IOP_CDVD_OFF_NPARAM 0x05u
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

/* Round 259 (task #419, 299th finding): a second, real bit of this
 * same register (offset 0x05/NREADY, "N command status") - bit 3
 * (0x08). ps2tek's own community documentation (israpps.github.io/
 * ps2tek/PS2/IOP/CDVD/IO_Ports.html) labels bit 3 "Unknown/unused",
 * but this project's own real, mounted retail BIOS (SCPH-10000)
 * disassembles to a genuine, real IOP-resident function - identified
 * via its own real module-header signature (0x41C00000 magic +
 * "eeconfig" name string + version 0x0101, the exact same struct
 * layout convention real SIFMAN/DMACMAN modules use, at IOP address
 * 0x0010C510, immediately following the polling code at
 * 0x0010C070-0x0010C50C) - that explicitly polls this exact bit
 * (`andi v0,v0,0x0008`) in a real, bounded ~196608-iteration retry
 * loop (`lui s0,0x0003` = 0x00030000) before giving up and returning
 * a clean, non-fatal error code (v0=1) to its caller.
 *
 * Independently cross-confirmed against a real, dated (2003)
 * community reverse-engineered reimplementation of this exact same
 * module (EECONF.C/eeconf.c, "made by [RO]man", user-provided this
 * session): its own `eeconf_start()` entry point contains
 * `*(int*)0x3C0=0;` (matches this project's disassembly finding
 * `addiu s3,zero,960; sw zero,0(s3)` byte-for-byte - 960 decimal =
 * 0x3C0) followed by `while ((CDVDreg_READY & 8==0) && (tmp>0))
 * tmp=0x2FFFF;` where `CDVDreg_READY` is that source's own #define
 * for physical address 0x1F402005 (`cdvdman.h`) - the exact same
 * register this project already models as OFF_NREADY, and 0x2FFFF
 * (196607) is the same retry-count order of magnitude this project's
 * own fresh disassembly found (0x00030000/196608 - the off-by-one is
 * consistent with a decrement-vs-compare implementation detail, not
 * a different constant).
 *
 * EECONF's own real job (per that same source's sceCdReadConfig/
 * sceCdWriteConfig/SCMD_OPENCONFIG functions) is reading/writing a
 * small config block resident on the CD/DVD controller board itself
 * - not disc media - so this bit's real-world meaning is plausibly a
 * static drive-controller self-test/capability flag available from
 * power-on, independent of whether a disc is inserted, unlike bit 6
 * (IOP_CDVD_DRIVE_READY) which genuinely can depend on tray/media
 * state. This project's CDVD register scaffold has never set this
 * bit, causing every real BIOS boot's EECONF init call to burn its
 * full retry budget and give up - this is the first citable, dual-
 * sourced (real disassembly + independent real reimplementation)
 * evidence for what that bit should be. */
#define IOP_CDVD_NREADY_CONFIG_READY 0x08u

/* Round 260 (task #421, 300th finding continuation): a second real
 * bit of this same register, found while measuring Round 259's fix -
 * once eeconf_start() gets past the bit-3 gate, it reads NREADY
 * twice more: `andi v0,v0,0x0002; beq ...` (bails if bit 1 is clear)
 * then `andi v0,v0,0x0004; bne ...` (bails if bit 2 IS set) - fresh
 * disassembly of this project's own mounted retail BIOS at IOP
 * 0x0010C314/0x0010C328. This is the SAME dual-source evidentiary
 * bar as IOP_CDVD_NREADY_CONFIG_READY above: the user-provided real
 * 2003 EECONF.C source has the exact matching line, unprompted and
 * byte-identical in intent: `if (CDVDreg_READY & 2==0) return 1;`
 * immediately followed by `if (CDVDreg_READY & 4!=0) return 1;`.
 * Per ps2tek bit 2 is "DEV9 device connected" - this project
 * correctly reports no DEV9 hardware (bit 2 already 0), so only bit
 * 1 needs a fix here; ps2tek labels bit 1 "Unknown/unused" the same
 * way it did bit 3 before Round 259. Real EECONF.C's own final
 * comment ("return 1; //does not get resident") confirms this
 * module always exits after this point regardless of outcome - bit 1
 * gates whether it does its real optional config read/write work,
 * not whether the wider boot proceeds. */
#define IOP_CDVD_NREADY_CONFIG2_READY 0x02u

/* Round 170 (task #172 continuation, real user-provided disc image):
 * two more real, cited constants from the same PCSX2 source
 * (pcsx2/CDVD/CDVD_internal.h / CDVDcommon.h), used by
 * iop_cdvd_set_disc_present() below.
 *   IOP_CDVD_TYPE_PS2CD (0x12, "PS2 CD"): the real disc-type value
 *   for a PS2 data disc on CD media (as opposed to DVD,
 *   CDVD_TYPE_PS2DVD=0x14) - this project's iso_loader.c now
 *   distinguishes real CD-XA raw-sector images (2352-byte physical
 *   sectors, Round 170) from plain/DVD-style flat images (2048-byte),
 *   so this value is evidence-based per opened image, not a guess.
 *   IOP_CDVD_STATUS_PAUSE (0x0A): the real Status value PCSX2's own
 *   cdvdCtrlTrayClose() sets immediately when VMManager::Internal::
 *   IsFastBootInProgress() is true (real, cited "fast boot" path -
 *   skips the real tray-detection delay/state machine entirely and
 *   goes straight to Ready+Engaged) - the closest real, cited
 *   analogue to this project's own existing philosophy of reporting
 *   immediate readiness rather than modeling multi-step real timing
 *   state machines (same rationale as OFF_NCMD's immediate-completion
 *   behavior below). */
#define IOP_CDVD_TYPE_PS2CD       0x12u
#define IOP_CDVD_STATUS_PAUSE     0x0Au

/* Round 261 (task #422, 301st finding): the CDVD S-command register
 * block (offset 0x16 SCOMMAND, 0x17 SDATAIN, 0x18 SDATAOUT) - the
 * "one level deeper" gap flagged at the end of Round 260, now that
 * EECONF's real code has advanced far enough to actually drive it.
 *
 * Fresh disassembly of this project's own real, mounted retail BIOS
 * (SCPH-10000) at IOP 0x0010BB30-0x0010BC7C - the function called by
 * the three small wrapper routines at 0x0010BC9C (a0=0x40),
 * 0x0010BCF4 (a0=0x43) and 0x0010BD2C (a0=0x41), i.e. the real
 * `sceCdSCmd()`-equivalent - shows a genuine, functioning register
 * protocol (confirmed by direct decode of the raw MIPS words, not
 * assumed):
 *   1. Read SDATAIN (0x17); if bit 7 set, bail out immediately (busy).
 *   2. Drain stale result bytes: while ((SDATAIN & 0x40) == 0),
 *      read SDATAOUT (0x18) and discard.
 *   3. Write each parameter byte to SDATAIN (0x17, write side).
 *   4. Write the command byte to SCOMMAND (0x16); read SCOMMAND back
 *      once (discarded).
 *   5. Busy-wait: while (SDATAIN & 0x80) - real hardware busy bit.
 *   6. If SDATAIN & 0x40 is clear (data available), read result bytes
 *      one at a time from SDATAOUT until SDATAIN & 0x40 becomes set
 *      again (no more data).
 *
 * This is independently cross-confirmed by the same user-provided
 * real 2003 EECONF.C source already cited above for
 * IOP_CDVD_NREADY_CONFIG_READY/CONFIG2_READY: its own `sceCdSCmd()`
 * function implements the identical sequence (its C source has an
 * `==`-vs-`&` operator-precedence typo in the loop conditions, but
 * this project's fresh disassembly of the real, compiled BIOS proves
 * the ACTUAL hardware/firmware behavior is the correctly-functioning
 * version described above - the typo is a transcription artifact in
 * that one reimplementation, not real hardware behavior).
 *
 * The specific command bytes this project's own disassembly found in
 * use (0x40, 0x41, 0x43) are independently, directly documented by
 * ps2tek's own dedicated S-command page
 * (israpps.github.io/ps2tek/PS2/IOP/CDVD/SCMD.html, fetched this
 * round) as OpenConfig (40h, "Dobiestation returns zero" - a real,
 * cited other-emulator precedent for the minimal honest result value
 * used here), ReadConfig (41h, "Output is four 32bit words" - real,
 * cited result size, honestly zero-filled since no real config data
 * is modeled) and CloseConfig (43h, result size undocumented - this
 * project uses the same single-zero-byte convention as OpenConfig for
 * consistency). This matches this project's own already-established
 * dispatch_ncmd() precedent exactly: real register protocol, real
 * command bytes, immediate synthetic completion, no fabricated
 * command-specific data beyond what's needed to keep a real busy-wait/
 * drain loop from spinning forever. */
#define IOP_CDVD_SDATAIN_BUSY   0x80u
#define IOP_CDVD_SDATAIN_NODATA 0x40u

#define SCMD_OPENCONFIG  0x40u
#define SCMD_READCONFIG  0x41u
#define SCMD_WRITECONFIG 0x42u
#define SCMD_CLOSECONFIG 0x43u

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

/* Round 347 (IOP RPC re-entry architecture): non-consuming peek at
 * the real OFF_ERROR register - see iop_cdvd.c's own definition
 * comment for the full rationale (project-internal only, never
 * reachable via real MMIO, so it cannot alter real read-clear
 * semantics real IOP code depends on). */
uint8_t iop_cdvd_peek_last_ncmd_error(void);
uint8_t iop_cdvd_get_status(void);
uint8_t iop_cdvd_get_ready(void);
uint8_t iop_cdvd_get_disc_type(void);

/* Round 170: switches the register block from this project's
 * existing "no disc" default to a genuine "disc present, tray
 * closed, drive ready" state - real, cited values (see
 * IOP_CDVD_TYPE_PS2CD/IOP_CDVD_STATUS_PAUSE above), matching PCSX2's
 * own real fast-boot tray-close path. Callers are responsible for
 * having actually validated a real disc image first (e.g. via a
 * successful iso_open() - core/iso_loader.h) - this function does not
 * touch or require any disc file itself, it only flips the register
 * values a real "disc is in and ready" state would show, so it must
 * not be called speculatively. `disc_type` lets a caller pass a
 * different real, cited CDVD_TYPE_* value (e.g. IOP_CDVD_TYPE_PS2DVD)
 * if the opened image was detected as a different real format;
 * pass IOP_CDVD_TYPE_PS2CD for the common CD-XA-sector case this
 * round's iso_loader.c detection targets. Not called from this
 * project's default (diskless) boot flow - this is real, tested
 * infrastructure for a disc-loaded boot mode, mirroring iso_loader.h's
 * own "standalone infrastructure for a future increment" precedent
 * (Round 139) - not a change to the existing, carefully-validated
 * diskless default. */
void iop_cdvd_set_disc_present(uint8_t disc_type);

/*
 * Round 206 (task #366): real N-command sector-read data path.
 *
 * Per ps2tek's directly-fetched, cited CDVD register/command pages
 * (https://psi-rockin.github.io/ps2tek/#cdvdioports /
 * #cdvdncommands / #cdvdreadsandseeks): "Write to this register to
 * send an N command"(1F402004h); "Send parameters for an N command
 * here. This must be done BEFORE the N command has been sent"
 * (1F402005h, write side); N-command 06h "ReadCd" takes a 4-byte LE
 * sector position (bytes 0-3) and 4-byte LE sector count (bytes 4-7)
 * as parameters; "Once one sector has been read, the CDVD DMA
 * channel can store the data in memory" - real hardware delivers
 * sector bytes via IOP DMA channel 3 (CDROM, matching this project's
 * own already-existing iop_dma.h channel table), not a data-FIFO
 * register on this page. This project's own iso_loader.c (Round 139,
 * already verified byte-exact against the real Tekken Tag Tournament
 * demo disc's own SYSTEM.CNF in Round 170) supplies the real sector
 * bytes once a disc image is mounted here.
 *
 * iop_cdvd_mount_iso()/iop_cdvd_unmount_iso() are this interface's
 * own disc-image binding - deliberately separate from
 * iop_cdrom_legacy.c's identically-shaped iop_cdrom_legacy_mount_iso()
 * rather than sharing one global mounted-disc object, matching this
 * project's own already-established "each caller needs its own slice"
 * precedent (cited in the 71st finding's romdir_lookup() discussion)
 * since these are two independent real hardware register blocks that
 * real BIOS code could in principle address independently.
 *
 * Honest scope: only N-command 06h (ReadCd) and 08h (ReadDvd, treated
 * identically to ReadCd here since this project's own iso_loader.c
 * already normalizes CD-XA/DVD-style sector access to one 2048-byte-
 * per-sector convention - see iso_loader.h) actually move real data.
 * 00h/01h/02h/03h/04h/05h/09h are accepted and acknowledged (real
 * per-command IRQ raised) but have no other modeled side effect - the
 * same "real for the concrete case, not full hardware" scope already
 * used throughout this project's other register models. Real seek-
 * timing delays (ps2tek's "Reads and Seeks" formulas) are not
 * modeled - completion is reported synchronously on the same MMIO
 * write, matching this project's own existing dma_channel_kick()/
 * iop_dma_sif0_try_transfer() simplification elsewhere. */
int iop_cdvd_mount_iso(const char *path);
void iop_cdvd_unmount_iso(void);

/* Round 367: real cdrom0:/cdrom1: FILEIO lookup/read support - thin
 * pass-through to the already-mounted, already-parsed g_disc (see
 * iop_cdvd.c's own citation for the full grounding). Returns 1/0
 * (find_file) or a real byte count via iso_read_sector()'s own 0/-1
 * convention (read_sector) - never fabricates data; both fail
 * honestly (0 / -1) if no disc is mounted or the name/LBA genuinely
 * isn't found/valid. */
int iop_cdvd_disc_find_file(const char *name, uint32_t *out_lba, uint32_t *out_size);
int iop_cdvd_disc_read_sector(uint32_t lba, uint8_t *buf);

#endif
