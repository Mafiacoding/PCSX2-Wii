/*
 * iop_elf.c - see include/core/hw/iop_elf.h for scope/citations.
 */
#include "core/hw/iop_elf.h"
#include <string.h>

#define EXPORT_MAGIC 0x41c00000u
#define IMPORT_MAGIC 0x41e00000u

#define R_MIPS_32   2u
#define R_MIPS_26   4u
#define R_MIPS_HI16 5u
#define R_MIPS_LO16 6u

static inline uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Bounds-checked ELF-image word read (from the raw file/ROM bytes,
 * NOT from IOP RAM - used while parsing headers before anything is
 * loaded). Returns 0 (and leaves *ok untouched-if-already-0) if out
 * of range. */
static uint32_t img_u32(const uint8_t *image, uint32_t image_size, uint32_t off, int *ok)
{
    if (off + 4 > image_size) { *ok = 0; return 0; }
    return rd_le32(image + off);
}
static uint16_t img_u16(const uint8_t *image, uint32_t image_size, uint32_t off, int *ok)
{
    if (off + 2 > image_size) { *ok = 0; return 0; }
    return rd_le16(image + off);
}

int iop_elf_load(iop_state_t *st, const uint8_t *image, uint32_t image_size,
                  uint32_t load_addr, iop_elf_load_result_t *out,
                  const char **err_out)
{
    const char *dummy_err;
    if (!err_out) err_out = &dummy_err;
    *err_out = NULL;
    memset(out, 0, sizeof(*out));

    if (image_size < 52) { *err_out = "image too small for an ELF header"; return -1; }
    if (!(image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F')) {
        *err_out = "bad ELF magic";
        return -1;
    }
    if (image[4] != 1 || image[5] != 1) { *err_out = "not ELF32 little-endian"; return -1; }

    int ok = 1;
    uint16_t e_machine  = img_u16(image, image_size, 18, &ok);
    uint32_t e_entry     = img_u32(image, image_size, 24, &ok);
    uint32_t e_phoff      = img_u32(image, image_size, 28, &ok);
    uint32_t e_shoff      = img_u32(image, image_size, 32, &ok);
    uint16_t e_phentsize = img_u16(image, image_size, 42, &ok);
    uint16_t e_phnum      = img_u16(image, image_size, 44, &ok);
    uint16_t e_shentsize = img_u16(image, image_size, 46, &ok);
    uint16_t e_shnum      = img_u16(image, image_size, 48, &ok);
    uint16_t e_shstrndx   = img_u16(image, image_size, 50, &ok);
    if (!ok) { *err_out = "truncated ELF header"; return -1; }
    if (e_machine != 8 /* EM_MIPS */) { *err_out = "not a MIPS ELF"; return -1; }

    /* --- Load every PT_LOAD (type 1) segment into IOP RAM --- */
    uint32_t max_extent = 0;   /* highest (vaddr+memsz) seen - drives load_end */
    uint32_t max_filesz_extent = 0; /* highest (vaddr+filesz) - table-scan upper bound */
    for (uint16_t i = 0; i < e_phnum; i++) {
        uint32_t ph = e_phoff + (uint32_t)i * e_phentsize;
        uint32_t p_type   = img_u32(image, image_size, ph + 0, &ok);
        uint32_t p_offset = img_u32(image, image_size, ph + 4, &ok);
        uint32_t p_vaddr   = img_u32(image, image_size, ph + 8, &ok);
        uint32_t p_filesz = img_u32(image, image_size, ph + 16, &ok);
        uint32_t p_memsz   = img_u32(image, image_size, ph + 20, &ok);
        if (!ok) { *err_out = "truncated program header"; return -1; }
        if (p_type != 1u /* PT_LOAD */) continue; /* PT_MIPS_IOPMOD (0x70000080) etc. carry no loadable bytes here */

        if (p_offset + p_filesz > image_size) { *err_out = "PT_LOAD segment exceeds image size"; return -1; }
        if ((uint64_t)load_addr + p_vaddr + p_memsz > st->ram_size) { *err_out = "PT_LOAD segment exceeds IOP RAM"; return -1; }

        for (uint32_t b = 0; b < p_filesz; b++)
            iop_mem_write8(st, load_addr + p_vaddr + b, image[p_offset + b]);
        for (uint32_t b = p_filesz; b < p_memsz; b++) /* bss portion of this segment */
            iop_mem_write8(st, load_addr + p_vaddr + b, 0);

        if (p_vaddr + p_memsz > max_extent) max_extent = p_vaddr + p_memsz;
        if (p_vaddr + p_filesz > max_filesz_extent) max_filesz_extent = p_vaddr + p_filesz;
    }
    if (max_extent == 0) { *err_out = "no PT_LOAD segments found"; return -1; }

    out->entry = load_addr + e_entry;
    out->load_addr = load_addr;
    out->load_end = load_addr + max_extent;

    /* --- .iopmod section (real name/size metadata) - see header
     * comment for the byte layout this was empirically confirmed
     * against, cross-checked with the two cited public references.
     * Best-effort only: not finding it is not a load failure (some
     * modules may lack it, or this round's field-offset assumptions
     * may not hold for every module - the load itself already
     * succeeded via the generic ELF/program-header path above,
     * which does not depend on this section at all). */
    if (e_shoff != 0 && e_shnum != 0) {
        uint32_t shstr_hdr = e_shoff + (uint32_t)e_shstrndx * e_shentsize;
        int shok = 1;
        uint32_t shstr_off = img_u32(image, image_size, shstr_hdr + 16, &shok);
        for (uint16_t i = 0; shok && i < e_shnum; i++) {
            uint32_t sh = e_shoff + (uint32_t)i * e_shentsize;
            int sok = 1;
            uint32_t sh_name   = img_u32(image, image_size, sh + 0, &sok);
            uint32_t sh_type   = img_u32(image, image_size, sh + 4, &sok);
            uint32_t sh_offset = img_u32(image, image_size, sh + 16, &sok);
            uint32_t sh_size   = img_u32(image, image_size, sh + 20, &sok);
            if (!sok) break;

            if (sh_type == 0x70000080u /* SHT_MIPS_IOPMOD */ && sh_size >= 26) {
                uint32_t name_off = shstr_off + sh_name;
                if (name_off < image_size) {
                    uint32_t n = name_off;
                    while (n < image_size && image[n] != 0) n++;
                    (void)n; /* section name itself unused, just bounds-checked for safety */
                }
                uint32_t nm_off = sh_offset + 26; /* see header comment: module,start,heap,text_size,data_size,bss_size (6*u32) + version (u16) */
                uint32_t k = 0;
                while (k < IOP_ELF_MODNAME_MAX - 1 && nm_off + k < image_size && image[nm_off + k] != 0) {
                    out->iopmod_name[k] = (char)image[nm_off + k];
                    k++;
                }
                out->iopmod_name[k] = '\0';
            }

            if (sh_type == 9u /* SHT_REL */) {
                uint32_t count = sh_size / 8u;
                for (uint32_t r = 0; r < count; r++) {
                    uint32_t re = sh_offset + r * 8u;
                    int rok = 1;
                    uint32_t r_offset = img_u32(image, image_size, re + 0, &rok);
                    uint32_t r_info   = img_u32(image, image_size, re + 4, &rok);
                    if (!rok) { *err_out = "truncated relocation entry"; return -1; }
                    uint32_t r_type = r_info & 0xffu;
                    uint32_t patch_addr = load_addr + r_offset;

                    if (r_type == R_MIPS_32) {
                        uint32_t w = iop_mem_read32(st, patch_addr);
                        iop_mem_write32(st, patch_addr, w + load_addr);
                    } else if (r_type == R_MIPS_26) {
                        uint32_t w = iop_mem_read32(st, patch_addr);
                        uint32_t field = w & 0x03FFFFFFu;
                        uint32_t addr26 = (field << 2) + load_addr;
                        uint32_t neww = (w & 0xFC000000u) | ((addr26 >> 2) & 0x03FFFFFFu);
                        iop_mem_write32(st, patch_addr, neww);
                    } else if (r_type == R_MIPS_HI16) {
                        /* Standard MIPS ABI HI16/LO16 pairing - the
                         * paired LO16 is the NEXT relocation entry in
                         * this same section, per every real entry
                         * observed (see header comment). */
                        if (r + 1 >= count) { *err_out = "R_MIPS_HI16 with no following relocation"; return -1; }
                        uint32_t re2 = sh_offset + (r + 1) * 8u;
                        uint32_t r_offset2 = img_u32(image, image_size, re2 + 0, &rok);
                        uint32_t r_info2   = img_u32(image, image_size, re2 + 4, &rok);
                        if (!rok || (r_info2 & 0xffu) != R_MIPS_LO16) {
                            *err_out = "R_MIPS_HI16 not followed by R_MIPS_LO16";
                            return -1;
                        }
                        uint32_t hi_addr = patch_addr;
                        uint32_t lo_addr = load_addr + r_offset2;
                        uint32_t hi_instr = iop_mem_read32(st, hi_addr);
                        uint32_t lo_instr = iop_mem_read32(st, lo_addr);
                        uint32_t ahi = hi_instr & 0xFFFFu;
                        int32_t  alo = (int16_t)(lo_instr & 0xFFFFu); /* sign-extended */
                        uint32_t combined = (ahi << 16) + (uint32_t)alo;
                        uint32_t newval = combined + load_addr;
                        uint32_t new_hi = ((newval + 0x8000u) >> 16) & 0xFFFFu;
                        uint32_t new_lo = newval & 0xFFFFu;
                        iop_mem_write32(st, hi_addr, (hi_instr & 0xFFFF0000u) | new_hi);
                        iop_mem_write32(st, lo_addr, (lo_instr & 0xFFFF0000u) | new_lo);
                        r++; /* consumed the paired LO16 too */
                    } else if (r_type == R_MIPS_LO16) {
                        /* A LO16 not immediately consumed by a
                         * preceding HI16 above - not seen in any real
                         * entry this round; treated as an honest,
                         * unsupported case rather than guessed at. */
                        *err_out = "unpaired R_MIPS_LO16 (unsupported)";
                        return -1;
                    } else {
                        *err_out = "unsupported relocation type";
                        return -1;
                    }
                }
            }
        }
    }

    /* --- Scan the loaded (and now fully relocated) image for real
     * IRX export/import tables - see header comment for the magic
     * numbers/layout citations. Only the filesz-covered region is
     * scanned (tables are real initialized data, never .bss). */
    for (uint32_t off = 0; off + 20 <= max_filesz_extent; off += 4) {
        uint32_t addr = load_addr + off;
        uint32_t magic = iop_mem_read32(st, addr);

        if (magic == EXPORT_MAGIC && out->export_count < IOP_ELF_MAX_TABLES) {
            uint32_t zero = iop_mem_read32(st, addr + 4);
            if (zero != 0) continue; /* per the cited doc: not zero => false positive */
            iop_elf_export_table_t *e = &out->exports[out->export_count];
            e->addr = addr;
            for (int b = 0; b < 8; b++) {
                uint8_t c = (uint8_t)(iop_mem_read8(st, addr + 12 + (uint32_t)b));
                e->name[b] = (char)c;
            }
            e->name[8] = '\0';
            uint32_t cnt = 0;
            for (uint32_t fp = addr + 20; ; fp += 4) {
                if (fp + 4 > load_addr + max_filesz_extent) break;
                if (iop_mem_read32(st, fp) == 0) break;
                cnt++;
                if (cnt > 512) break; /* safety cap against a malformed/unterminated table */
            }
            e->fptr_count = cnt;
            out->export_count++;
        } else if (magic == IMPORT_MAGIC && out->import_count < IOP_ELF_MAX_TABLES) {
            uint32_t zero = iop_mem_read32(st, addr + 4);
            if (zero != 0) continue;
            iop_elf_import_table_t *im = &out->imports[out->import_count];
            im->addr = addr;
            for (int b = 0; b < 8; b++) {
                uint8_t c = (uint8_t)(iop_mem_read8(st, addr + 12 + (uint32_t)b));
                im->name[b] = (char)c;
            }
            im->name[8] = '\0';
            uint32_t cnt = 0;
            for (uint32_t sp = addr + 20; ; sp += 8) {
                if (sp + 8 > load_addr + max_filesz_extent) break;
                uint32_t w0 = iop_mem_read32(st, sp);
                uint32_t w1 = iop_mem_read32(st, sp + 4);
                if (w0 == 0 && w1 == 0) break;
                cnt++;
                if (cnt > 512) break;
            }
            im->stub_count = cnt;
            out->import_count++;
        }
    }

    return 0;
}
