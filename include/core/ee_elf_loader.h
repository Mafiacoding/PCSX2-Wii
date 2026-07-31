#ifndef PCSX2WII_EE_ELF_LOADER_H
#define PCSX2WII_EE_ELF_LOADER_H

#include <stdint.h>
#include "core/ee/ee_core.h"

/*
 * ee_elf_loader.h - real ELF32/MIPS loader for PS2 GAME BOOT
 * executables (Round 171, task #172 continuation: "actually loading
 * and jumping to the real boot ELF" per docs/ROADMAP.md's Round 171
 * plan, direction (b)).
 *
 * SCOPE, AND WHY THIS IS DELIBERATELY MUCH SIMPLER THAN iop_elf.c:
 * real PS2 game boot executables (the file named by SYSTEM.CNF's
 * "BOOT2 =" line, e.g. this project's own real, user-provided
 * "SCED_500.41" - see docs/STATUS.md's 210th finding) are genuine
 * ELF32/MIPS files, but of type ET_EXEC (2) - statically linked,
 * already fully relocated at link time, no import/export tables, no
 * runtime relocations - UNLIKE iop_elf.c's IRX modules (ET_LOPROC/
 * SHT_MIPS_IOPMOD, real runtime relocation records, import/export
 * stub patching - see iop_elf.h's own citation trail for that
 * distinct, Sony/SN-Systems-specific format). This is confirmed by
 * direct inspection of real PCSX2 source fetched this round
 * (pcsx2/Elfheader.cpp, GPL-3.0, same citation tier already used
 * elsewhere in this project e.g. ee_core.c/iop_core.c's opcode
 * tables): ElfObject::LoadProgramHeaders() only ever handles p_type
 * == 1 (PT_LOAD) - there is no relocation-record or import/export
 * handling anywhere in PCSX2's own real ELF loading path for this
 * kind of file. The real, standard ELF32 "load PT_LOAD segments to
 * their p_vaddr, zero-fill the bss tail, jump to e_entry" procedure
 * (same public ELF spec citation tier as iop_elf.h's own MIPS ABI
 * relocation-type citations) is therefore both correct AND
 * sufficient here - not a narrower reimplementation missing real
 * functionality, unlike (honestly flagged) gaps elsewhere in this
 * project.
 *
 * WHAT ABOUT INITIAL REGISTER STATE (sp/gp/args)? Real PS2 EELOAD
 * (the BIOS-ROM-resident kernel module that performs this exact real
 * ELF-loading procedure on real hardware and in real PCSX2 - see
 * pcsx2/R5900.cpp's eeloadHook()/eeloadHook2(), which patch EELOAD's
 * OWN real running code rather than reimplementing ELF loading in the
 * emulator itself) does NOT set $sp or $gp before jumping to e_entry.
 * Confirmed directly from real, public ps2sdk source fetched this
 * round (ee/startup/src/crt0.c's __start(), the entry point every
 * ps2sdk-built ELF uses - including real games and OSDSYS itself):
 * the ELF's OWN compiled-in startup code sets $gp itself (`la $4,
 * _gp` / `move $gp,$4`) and then calls the real SetupThread kernel
 * syscall (EE syscall 60 - see ee_core.c's Round 171 fix, same round
 * as this file) to obtain a real, valid stack pointer, which it then
 * moves into $sp itself (`move $sp, $2`). The ONLY externally-required
 * initial state a real loader provides is: $pc = e_entry, and $a0 =
 * a pointer to a real `struct sargs_start` argument block (or NULL/0,
 * which ps2sdk's own crt0 explicitly handles as a safe, real,
 * documented case - see its `_main()`: "if (args.argc == 0 &&
 * args_start != NULL && args_start->args.argc != 0) ..." - a NULL
 * args_start pointer safely falls through to a zero-argc default,
 * not a fabricated assumption). This loader therefore only ever sets
 * $pc and (optionally) $a0 - it does NOT set $sp/$gp/$ra, matching
 * real EELOAD behavior exactly rather than guessing at values a real
 * loader never provides.
 *
 * NOT WIRED INTO THE DEFAULT BOOT FLOW: like iso_loader.c (Round 139)
 * and iop_cdrom_legacy_mount_iso() (Round 145) before it, this is
 * real, tested, standalone infrastructure - main.c's default
 * diskless boot is unchanged. Exercising it is a deliberate,
 * separate experiment (see docs/STATUS.md's Round 171 finding).
 */

typedef struct {
    uint32_t entry;      /* real e_entry, absolute EE virtual address */
    uint32_t load_start;  /* lowest PT_LOAD p_vaddr seen */
    uint32_t load_end;    /* highest (p_vaddr + p_memsz) seen */
} ee_elf_load_result_t;

/* Loads one ELF32/MIPS ET_EXEC image (raw file bytes, e.g. a full
 * game boot ELF read via iso_loader.c) into EE RAM via st's own
 * ee_mem_write8() (so normal TLB/KUSEG address translation applies,
 * same as any other EE RAM write - no raw pointer poking), applying
 * every PT_LOAD program header's real p_vaddr/p_offset/p_filesz/
 * p_memsz fields (zero-filling the memsz-filesz bss tail). Does NOT
 * modify st->pc/st->gpr - the caller decides when/how to actually
 * jump to out->entry (see the header comment for what real EELOAD
 * sets and doesn't). Returns 0 on success, -1 on a malformed image
 * (bad magic, wrong machine/class, truncated, or a segment that
 * wouldn't fit in EE RAM) with a short reason via `err_out` (may be
 * NULL). */
int ee_elf_load(ee_state_t *st, const uint8_t *image, uint32_t image_size,
                 ee_elf_load_result_t *out, const char **err_out);

#endif
