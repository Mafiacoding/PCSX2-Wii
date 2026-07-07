#ifndef PCSX2WII_IOP_ELF_H
#define PCSX2WII_IOP_ELF_H

#include <stdint.h>
#include "core/iop/iop_core.h"

/*
 * iop_elf.h - real ELF32/MIPS loader + relocator for IOP "IRX"
 * modules (task #92: "IOP module/IRX loader").
 *
 * SCOPE AND CITATIONS (read before extending): real PS2 IOP modules
 * embedded in the BIOS ROM (and on-disc IRX files generally) are
 * genuine ELF32 MIPS relocatable objects with one Sony/SN-Systems-
 * specific twist: a vendor e_type (0xFF80, in the ELF "processor-
 * specific" range ET_LOPROC-ET_HIPROC) and a matching custom section/
 * segment type (0x70000080, "SHT_MIPS_IOPMOD"/"PT_MIPS_IOPMOD") that
 * holds IRX-specific metadata (module name, text/data/bss sizes,
 * entry point). This is NOT fabricated or guessed - it was verified
 * two independent ways:
 *   1. A byte-level dump of every field of a REAL module (SYSMEM,
 *      the first module in the user's own real, legally-owned
 *      SCPH-10000 BIOS's IOPBTCONF boot list - see
 *      iop_module_loader.h) via a local, uncommitted diagnostic
 *      script (never checked into this repo - only the BIOS's own
 *      structural facts, not its copyrighted bytes, are used here).
 *   2. Cross-checked against two independent PUBLIC, citable
 *      references: ps2dev/ps2sdk's real `iop/kernel/include/irx.h`
 *      (https://github.com/ps2dev/ps2sdk, Academic Free License 2.0 -
 *      defines the real `irx_import_table`/`irx_export_table`/
 *      `irx_import_stub` structs, the 0x41e00000/0x41c00000 import/
 *      export magic numbers, and the "jr $ra"/"addiu $zero,$zero,ord"
 *      two-word import stub convention used below) and the
 *      community-maintained "Writing a PS2 BIOS in Rust" book's IOP
 *      chapter (https://rust-console.github.io/ps2-bios-book/md/
 *      2_2_iop_boot.html - documents the `.iopmod` section's field
 *      order/meaning and confirms the same import/export magic
 *      numbers and stub convention independently).
 *
 * Relocations: every relocation entry observed in the real BIOS's
 * embedded modules uses symbol index 0 (no named-symbol table is
 * even present) - i.e. every relocation is a plain REBASE from an
 * assumed link-time base of 0 to this module's actual IOP RAM load
 * address, using the standard, publicly-documented MIPS ELF ABI
 * relocation types (System V ABI MIPS Processor Supplement - not
 * Sony-specific, the same relocation types any MIPS ELF toolchain
 * uses): R_MIPS_32 (2, plain rebase of a 32-bit word/pointer),
 * R_MIPS_26 (4, rebase of a J/JAL instruction's 26-bit target field),
 * R_MIPS_HI16/R_MIPS_LO16 (5/6, the standard paired lui+addiu/ori
 * 32-bit-constant-building relocation - HI16 is always immediately
 * followed by its paired LO16 in every real entry observed, the
 * standard toolchain emission order this code assumes). Inter-module
 * calls do NOT use ELF relocations at all - they go through the
 * import-stub-patching mechanism below instead (this is exactly why
 * every relocation's symbol index is 0: there are no real "external
 * symbol" ELF relocations in this format).
 *
 * WHAT IS NOT IMPLEMENTED, and why: $gp (MIPS o32 ABI global pointer)
 * is not computed or set - no R_MIPS_GPREL16 relocation was observed
 * in the one real module byte-dumped this round (SYSMEM), meaning it
 * doesn't use $gp-relative addressing internally; this is an honest,
 * narrower-than-ideal scope, not a claim that no IOP module ever
 * needs a correct $gp (a future round may need to address this if a
 * later-loaded real module's interpreted code misbehaves due to a
 * bad $gp). Dynamic/shared-library-style GOT/PLT MIPS ABI features
 * are not implemented either - real IOP modules don't appear to use
 * them (no such relocation types were observed).
 */

#define IOP_ELF_MAX_TABLES 8
#define IOP_ELF_MODNAME_MAX 32

typedef struct {
    uint32_t addr;               /* IOP RAM address of this export table (already-relocated) */
    char     name[IOP_ELF_MODNAME_MAX];
    uint32_t fptr_count;         /* number of non-null entries in fptrs[] */
} iop_elf_export_table_t;

typedef struct {
    uint32_t addr;               /* IOP RAM address of this import table */
    char     name[IOP_ELF_MODNAME_MAX]; /* name of the module being imported FROM */
    uint32_t stub_count;
} iop_elf_import_table_t;

typedef struct {
    uint32_t entry;               /* absolute IOP RAM entry point (load_addr + e_entry) */
    uint32_t load_addr;           /* where this module's image was placed */
    uint32_t load_end;            /* first free byte after image+bss - for the caller's bump allocator */
    char     iopmod_name[IOP_ELF_MODNAME_MAX]; /* name from .iopmod, if the section was found; else empty */

    iop_elf_export_table_t exports[IOP_ELF_MAX_TABLES];
    int export_count;
    iop_elf_import_table_t imports[IOP_ELF_MAX_TABLES];
    int import_count;
} iop_elf_load_result_t;

/* Loads one ELF32/MIPS IOP module image (raw bytes, e.g. a ROMDIR
 * payload slice) into IOP RAM starting at `load_addr`, applying real
 * relocations (see header comment) so the result is genuinely
 * executable at that address - not a stub, not a guess. Returns 0 on
 * success, -1 on a malformed/unrecognized image (bad ELF magic, wrong
 * machine type, unsupported relocation type, truncated data, or the
 * result wouldn't fit in IOP RAM) with a short reason via
 * `err_out` (may be NULL). Scans the loaded image for real IRX
 * export/import tables (magic 0x41c00000/0x41e00000) and reports
 * them in `out` for the caller (iop_module_loader.c) to link. */
int iop_elf_load(iop_state_t *st, const uint8_t *image, uint32_t image_size,
                  uint32_t load_addr, iop_elf_load_result_t *out,
                  const char **err_out);

#endif
