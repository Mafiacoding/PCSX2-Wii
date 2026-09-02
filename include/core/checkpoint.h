#ifndef PCSX2_WII_CHECKPOINT_H
#define PCSX2_WII_CHECKPOINT_H

#include <stdint.h>
#include "core/bios_loader.h"

/*
 * checkpoint.h - host-native checkpoint/resume tooling (Round 575,
 * task #550). This module never runs on the Wii target - it exists
 * purely to let host-native boot-survey drivers (tools/roundNNN-*)
 * save the current EE+IOP+peripheral state to a file and resume
 * from it later, instead of re-running an entire slow boot from
 * reset every single time a test needs a few more instructions
 * past a known-good milestone (the same problem driver_r313.c's
 * never-shipped/committed checkpoint format solved for Rounds
 * 307-449 - see include/core/system.h and include/core/hw/
 * iop_heap.h's citations of that prior work).
 *
 * This is a FRESH implementation, not a recovery of driver_r313.c
 * (which was always host-only scratch code, never tracked in git -
 * per its own citation trail - so there was nothing to recover).
 * It reuses the exact restore-time API contract that project
 * already built and documented for this purpose across several
 * rounds: dma_bind_ee_ram()/dma_bind_scratchpad()/
 * iop_dma_bind_iop_ram() (re-bind heap-allocated RAM pointers),
 * ee_core_rebind_dma_sinks()/system_rebind_iop_bridge() (re-bind
 * cross-module function/context pointers to THIS process's own
 * valid addresses, since a resuming process is a different,
 * ASLR-relocated instance of the same binary), and
 * iop_cdrom_legacy_rebind_iso()/iop_cdvd_rebind_iso() (reopen a
 * disc image file handle, since file descriptors don't survive
 * serialization).
 *
 * FILE FORMAT: magic "PW2K" (4 bytes) + version (u32 LE, currently
 * 1), then a sequence of blocks - tag (4 ASCII bytes) + size (u32
 * LE) + that many raw payload bytes - terminated by a "END0" tag
 * with size 0. Each block is simply a memcpy() of one module's
 * entire static state struct (or, for ee_hle_thread.c, an opaque
 * blob - see its own get_checkpoint_blob() citation for why), so
 * this format is inherently fragile to struct-layout changes
 * across builds - it is a host-native TESTING aid for THIS
 * project's own iterative development, not a stable savestate
 * format for end users, exactly like driver_r313.c never was.
 *
 * SECURITY / LEAK-PREVENTION: checkpoint files NEVER embed BIOS
 * ROM or disc/ISO image bytes - only pointers to caller-owned,
 * already-loaded bios_image_t structs are needed at restore time
 * (checkpoint_load() takes them as parameters, exactly like
 * iop_cdrom_legacy_rebind_iso() takes a path string instead of
 * embedded disc bytes). This project's standing rule (never commit/
 * push/rsync real Sony copyrighted BIOS/disc data) applies to
 * checkpoint files exactly as it does to git commits - keep them
 * out of the tracked repo and out of any outputs rsync, same as
 * any other file containing or derived from BIOS/disc bytes (a
 * checkpoint's EE/IOP RAM dump WILL contain BIOS-derived code/data
 * that was DMA'd or executed into RAM, so checkpoint files are
 * treated as leak-check-scope artifacts, never committed).
 *
 * KNOWN LIMITATION (documented, not a silent gap): ipu.c,
 * iop_spu2.c, and iop_spu_legacy.c have no _get_state() accessor
 * and are NOT captured by this version. Per Round 521-525's own
 * audit, these are confirmed off the critical GS/boot-progress
 * path (skeleton-only register models with no consumer reading
 * them back into control flow yet), so omitting them from v1 is a
 * scoped, acceptable gap. If a future round wires real behavior
 * into these registers, add accessors following the exact pattern
 * ee_hle_thread_get_checkpoint_blob() established this round, and
 * a new tag block in checkpoint.c.
 */

/* Saves the current EE core, IOP core, and every hw/ peripheral's
 * state (see the module-level comment above for exactly which ones
 * and the one documented omission) to `path`. Returns 0 on success,
 * -1 on I/O failure. */
int checkpoint_save(const char *path);

/* Restores state previously saved by checkpoint_save() from `path`.
 * ee_bios/iop_bios must be the SAME already-loaded BIOS images used
 * when the checkpoint was taken (checkpoint files never embed BIOS
 * bytes - see the leak-prevention note above); iso_path is the disc
 * image path to reopen via iop_cdrom_legacy_rebind_iso()/
 * iop_cdvd_rebind_iso(), or NULL for a diskless-boot checkpoint.
 * Validates the entire block sequence into a scratch buffer BEFORE
 * touching any live state, so a truncated/corrupt file fails
 * cleanly (returns -1) without leaving the caller in a half-restored
 * state. On success, every module's static state exactly matches
 * what it was at checkpoint_save() time, and all rebind-time
 * pointers/handles are freshly re-established for this process
 * (see the module comment's rebind-function list). Returns 0 on
 * success, -1 on failure. */
int checkpoint_load(const char *path, const bios_image_t *ee_bios,
                     const bios_image_t *iop_bios, const char *iso_path);

#endif
