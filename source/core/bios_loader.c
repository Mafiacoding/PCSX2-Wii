/*
 * bios_loader.c - PS2 BIOS ROM image loader
 *
 * PS2 BIOS dumps are 4MB images that begin with a ROMDIR table at
 * offset 0x100 ("ROMDIR" itself is normally *not* the first name -
 * the very first entry is usually "RESET", quickly followed by an
 * entry called "ROMVER" whose payload is a short ASCII version
 * string like "0150EC20010704"). This loader does a best-effort,
 * simplified walk of that table purely to fish out the version
 * string for display - it is NOT a full IOP/EE loader and does not
 * yet resolve or map any of the other listed modules (ROMDIR entries
 * such as OSDSYS, SIO2MAN, etc. that the real BIOS boot process
 * depends on). That resolution is unimplemented - see docs/STATUS.md.
 */

#include "core/bios_loader.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#define ROMDIR_OFFSET   0x100
#define ROMDIR_ENTRY_SZ 16

/* PS2 BIOS dumps are little-endian; the Wii (our build target) is
 * big-endian, so a raw memcpy into a packed struct here would
 * misread the 16/32-bit fields. Decode them explicitly instead. */
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void try_parse_romver(bios_image_t *bios)
{
    strcpy(bios->version_string, "unknown");

    if (bios->size < ROMDIR_OFFSET + ROMDIR_ENTRY_SZ * 32)
        return;

    const uint8_t *base = bios->data + ROMDIR_OFFSET;
    uint32_t data_offset = 0;
    int found_romver = 0;
    uint32_t romver_off = 0, romver_size = 0;

    /* Walk entries until an empty name terminator or sane limit. */
    for (int i = 0; i < 512; i++) {
        const uint8_t *entry = base + i * ROMDIR_ENTRY_SZ;

        char name[11];
        memcpy(name, entry, 10); /* name is raw ASCII bytes, no endian issue */
        name[10] = '\0';

        if (name[0] == '\0')
            break;

        uint32_t size = rd_le32(entry + 12);

        if (strncmp(name, "ROMVER", 6) == 0) {
            romver_off = ROMDIR_OFFSET + 512 * ROMDIR_ENTRY_SZ + data_offset;
            romver_size = size;
            found_romver = 1;
        }

        /* file payloads are packed sequentially, 16-byte aligned */
        data_offset += (size + 15) & ~15u;
    }

    if (found_romver && romver_size > 0 && romver_size < sizeof(bios->version_string)
        && romver_off + romver_size <= bios->size) {
        memcpy(bios->version_string, bios->data + romver_off, romver_size);
        bios->version_string[romver_size] = '\0';
    }
}

int bios_load(const char *path, bios_image_t *out)
{
    if (!path || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > BIOS_MAX_SIZE) {
        fclose(f);
        return -1;
    }

    /* 32-byte aligned alloc, BIOS lives in MEM2 for the duration of the run */
    out->data = memalign(32, BIOS_MAX_SIZE);
    if (!out->data) {
        fclose(f);
        return -1;
    }
    memset(out->data, 0xFF, BIOS_MAX_SIZE);

    size_t rd = fread(out->data, 1, (size_t)sz, f);
    fclose(f);

    if (rd != (size_t)sz) {
        free(out->data);
        out->data = NULL;
        return -1;
    }

    out->size = (uint32_t)sz;

    const char *slash = strrchr(path, '/');
    strncpy(out->name, slash ? slash + 1 : path, sizeof(out->name) - 1);

    try_parse_romver(out);

    out->loaded = 1;
    return 0;
}

void bios_free(bios_image_t *bios)
{
    if (bios && bios->data) {
        free(bios->data);
        bios->data = NULL;
        bios->loaded = 0;
    }
}
