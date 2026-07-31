/*
 * ee_elf_loader.c - see include/core/ee_elf_loader.h for scope/
 * citations (Round 171, task #172 continuation).
 */
#include "core/ee_elf_loader.h"
#include <string.h>

static inline uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Bounds-checked ELF-image field reads, same pattern as iop_elf.c's
 * img_u32()/img_u16() (from the raw file bytes, not EE RAM). */
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

int ee_elf_load(ee_state_t *st, const uint8_t *image, uint32_t image_size,
                 ee_elf_load_result_t *out, const char **err_out)
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
    uint16_t e_type      = img_u16(image, image_size, 16, &ok);
    uint16_t e_machine   = img_u16(image, image_size, 18, &ok);
    uint32_t e_entry     = img_u32(image, image_size, 24, &ok);
    uint32_t e_phoff     = img_u32(image, image_size, 28, &ok);
    uint16_t e_phentsize = img_u16(image, image_size, 42, &ok);
    uint16_t e_phnum     = img_u16(image, image_size, 44, &ok);
    if (!ok) { *err_out = "truncated ELF header"; return -1; }
    if (e_machine != 8 /* EM_MIPS */) { *err_out = "not a MIPS ELF"; return -1; }
    /* e_type is logged/available via *err_out-free inspection only -
     * a real ET_EXEC (2) is expected (see header comment), but this
     * is not hard-rejected: the PT_LOAD-driven load procedure below
     * is valid ELF32 behavior regardless of e_type, and rejecting on
     * a field this loader doesn't otherwise need would be an
     * unnecessary, unverified assumption about every real disc's
     * exact e_type value. */
    (void)e_type;

    uint32_t load_start = 0xFFFFFFFFu;
    uint32_t load_end = 0;
    int any_load = 0;

    for (uint16_t i = 0; i < e_phnum; i++) {
        uint32_t ph = e_phoff + (uint32_t)i * e_phentsize;
        uint32_t p_type   = img_u32(image, image_size, ph + 0, &ok);
        uint32_t p_offset = img_u32(image, image_size, ph + 4, &ok);
        uint32_t p_vaddr  = img_u32(image, image_size, ph + 8, &ok);
        uint32_t p_filesz = img_u32(image, image_size, ph + 16, &ok);
        uint32_t p_memsz  = img_u32(image, image_size, ph + 20, &ok);
        if (!ok) { *err_out = "truncated program header"; return -1; }
        if (p_type != 1u /* PT_LOAD */) continue;

        if (p_offset + p_filesz > image_size) { *err_out = "PT_LOAD segment exceeds image size"; return -1; }
        if ((uint64_t)p_vaddr + p_memsz > st->ram_size) { *err_out = "PT_LOAD segment exceeds EE RAM"; return -1; }

        for (uint32_t b = 0; b < p_filesz; b++)
            ee_mem_write8(st, p_vaddr + b, image[p_offset + b]);
        for (uint32_t b = p_filesz; b < p_memsz; b++) /* bss portion of this segment */
            ee_mem_write8(st, p_vaddr + b, 0);

        if (p_vaddr < load_start) load_start = p_vaddr;
        if (p_vaddr + p_memsz > load_end) load_end = p_vaddr + p_memsz;
        any_load = 1;
    }
    if (!any_load) { *err_out = "no PT_LOAD segments found"; return -1; }

    out->entry = e_entry;
    out->load_start = load_start;
    out->load_end = load_end;
    return 0;
}
