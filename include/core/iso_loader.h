#ifndef PCSX2WII_ISO_LOADER_H
#define PCSX2WII_ISO_LOADER_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/*
 * iso_loader.h - real ISO9660 disc-image loader (Round 139, task
 * #172/#296, 179th finding).
 *
 * SCOPE: this parses the standard ISO9660 filesystem (ECMA-119) - a
 * public, non-proprietary standard used by essentially all optical
 * disc images including PS2 discs, NOT any Sony-copyrighted BIOS or
 * game code, so this project's clean-room-BIOS convention does not
 * apply here (nothing about ISO9660 comes from disassembling the
 * PS2 BIOS - it's a public filesystem spec any CD/DVD image follows).
 * Real, cited structure (ECMA-119 / ISO 9660:1988): 2048-byte
 * sectors; the Primary Volume Descriptor sits at logical sector 16;
 * it begins with a 1-byte type (1 = Primary Volume Descriptor), a
 * 5-byte standard identifier "CD001" at offset 1, and contains a
 * 34-byte Root Directory Record at offset 156. Each Directory Record
 * is: length(1) + ext-attr-length(1) + extent-LBA both-endian(8,
 * 4-byte-LE then 4-byte-BE) + data-length both-endian(8) + date(7) +
 * flags(1, bit1=directory) + unit-size(1) + interleave(1) +
 * volume-seq both-endian(4) + name-length(1) + name(variable).
 *
 * PS2 discs are single-session DVD-ROMs using plain 2048-byte
 * sectors (unlike PS1 CD images, which sometimes use a 2352-byte
 * raw/subchannel format requiring a separate .cue sheet) - so a
 * ".bin" raw dump of a PS2 disc is, for this project's purposes,
 * the same 2048-byte-sector format as a ".iso" and both are handled
 * identically here; no CUE-sheet parsing is implemented or needed.
 *
 * What this module DOES provide: opening a raw disc image file,
 * parsing its Primary Volume Descriptor and root directory, looking
 * up a file by name in the root directory (single level - sufficient
 * for locating SYSTEM.CNF, which real PS2 discs always place at the
 * root), and reading arbitrary 2048-byte sectors by LBA.
 *
 * What this module explicitly does NOT do (honest scope limit):
 * subdirectory traversal, Rock Ridge/Joliet extension parsing,
 * multi-session/multi-extent handling, or any wiring into this
 * project's live IOP CDVD boot trace. This project's current,
 * carefully-verified boot progress (Round 130+) is specifically a
 * DISKLESS, BIOS-only boot scenario - iop_cdvd.c's NODISC/TRAY_OPEN
 * defaults are an intentional, cited choice matching that scenario,
 * not an oversight. Wiring a loaded ISO into the live CDVD register
 * model to make the boot path see "disc present" is a real, separate,
 * larger future increment (it would change the boot scenario this
 * project's entire traced history to date has been validated against)
 * - this module is real, tested, standalone infrastructure for that
 * future increment, not a live behavior change this round.
 */

#define ISO_SECTOR_SIZE 2048u
#define ISO_PVD_LBA     16u

typedef struct {
    uint32_t lba;           /* extent location (sector number) */
    uint32_t size;          /* data length in bytes */
    uint8_t  is_directory;
    char     name[224];     /* ISO9660 max name length is 222 bytes; +NUL, generous */
} iso_dirent_t;

typedef struct {
    FILE    *fp;
    uint32_t root_lba;
    uint32_t root_size;
    uint8_t  opened;
} iso_image_t;

/* Opens a raw ISO9660/BIN disc image from a host/libfat path and
 * parses its Primary Volume Descriptor + root directory record.
 * Returns 0 on success, -1 on failure (file missing, PVD "CD001"
 * signature not found at the expected offset, or I/O error). */
int iso_open(const char *path, iso_image_t *out);

void iso_close(iso_image_t *img);

/* Reads exactly one 2048-byte sector at the given LBA into buf (must
 * be at least ISO_SECTOR_SIZE bytes). Returns 0 on success, -1 on
 * failure (not open, out-of-range read, I/O error). */
int iso_read_sector(iso_image_t *img, uint32_t lba, uint8_t *buf);

/* Looks up a file by name (case-sensitive, exact match against the
 * stored ISO9660 name INCLUDING any ";N" version suffix if present -
 * callers that don't know the version should try both forms) directly
 * in the root directory (single level, see header scope note).
 * Returns 0 and fills *out on success, -1 if not found or on error. */
int iso_find_in_root(iso_image_t *img, const char *name, iso_dirent_t *out);

#endif
