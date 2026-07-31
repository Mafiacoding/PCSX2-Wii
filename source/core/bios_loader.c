/*
 * bios_loader.c - PS2 BIOS ROM image loader
 *
 * PS2 BIOS dumps contain a ROMDIR table (an array of 16-byte entries:
 * 10-byte ASCII name, 2-byte "extinfo" size, 4-byte little-endian
 * payload size) whose own file position VARIES by BIOS revision/size
 * - there is no single fixed offset that works across all dumps. An
 * earlier version of this loader assumed a fixed ROMDIR_OFFSET of
 * 0x100, which is WRONG - confirmed once a real BIOS dump (SCPH-10000,
 * provided by the project's user, a legally-dumped BIOS from hardware
 * they own) was available to test against: its ROMDIR table is
 * actually at file offset 0x2700, not 0x100. That bug meant ROMVER
 * parsing silently failed on real data despite "working" against this
 * project's all-zero synthetic test BIOS images, which never actually
 * exercised the ROMDIR-walking code path meaningfully - a good
 * reminder that synthetic test fixtures can't catch every bug a real
 * data sample will.
 *
 * Fixed by locating the table with a scan instead of a fixed offset:
 * by strong, universal PS2 BIOS convention, ROMDIR entry 0 is always
 * named "RESET" and entry 1 (the next 16 bytes) is always named
 * "ROMDIR" itself (the table describes itself as one of its own
 * entries) - this signature is reliable regardless of where the table
 * physically ends up in a given dump.
 *
 * Once located, module payloads are packed SEQUENTIALLY starting from
 * FILE OFFSET 0 (not from some position "after" the table) - the
 * RESET module's own code occupies the very first bytes of the file,
 * the ROMDIR table's bytes ARE the payload for the "ROMDIR" entry
 * (which is why the table's file position always exactly equals the
 * aligned size of the RESET entry that precedes it), and every
 * subsequent entry's payload begins where the previous one's ends,
 * each rounded up to a 16-byte boundary. This loader still only walks
 * the table to fish out the ROMVER string for display - it is NOT a
 * full IOP/EE module loader and does not resolve/map any of the other
 * listed modules (OSDSYS, SIO2MAN, etc.) that the real BIOS boot
 * process depends on - see docs/STATUS.md.
 */

#include "core/bios_loader.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#define ROMDIR_ENTRY_SZ 16
#define ROMDIR_MAX_ENTRIES 512
/* Real BIOS dumps keep RESET+ROMDIR well within the first 64KB - a
 * conservative scan bound so this stays fast and doesn't risk a false
 * match somewhere deep in unrelated module data. */
#define ROMDIR_SCAN_LIMIT 0x10000u

/* PS2 BIOS dumps are little-endian; the Wii (our build target) is
 * big-endian, so a raw memcpy into a packed struct here would
 * misread the 16/32-bit fields. Decode them explicitly instead. */
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Scans for the ROMDIR table via the RESET+ROMDIR name signature
 * (see the file header comment). Returns 1 and fills *table_off on
 * success, 0 if no such signature was found within the scan bound. */
static int find_romdir_table(const bios_image_t *bios, uint32_t *table_off)
{
    if (bios->size < ROMDIR_ENTRY_SZ * 2)
        return 0;

    uint32_t limit = bios->size - ROMDIR_ENTRY_SZ * 2;
    if (limit > ROMDIR_SCAN_LIMIT)
        limit = ROMDIR_SCAN_LIMIT;

    for (uint32_t off = 0; off <= limit; off += 4) {
        const uint8_t *e0 = bios->data + off;
        const uint8_t *e1 = bios->data + off + ROMDIR_ENTRY_SZ;
        if (memcmp(e0, "RESET\0\0\0\0\0", 10) == 0 &&
            memcmp(e1, "ROMDIR\0\0\0\0", 10) == 0) {
            *table_off = off;
            return 1;
        }
    }
    return 0;
}

static void try_parse_romver(bios_image_t *bios)
{
    strcpy(bios->version_string, "unknown");

    uint32_t table_off;
    if (!find_romdir_table(bios, &table_off))
        return;

    const uint8_t *base = bios->data + table_off;
    /* Module payloads are packed sequentially starting from file
     * offset 0 (see header comment) - NOT from table_off. */
    uint32_t data_offset = 0;
    int found_romver = 0;
    uint32_t romver_off = 0, romver_size = 0;

    /* Walk entries until an empty name terminator or sane limit. */
    for (int i = 0; i < ROMDIR_MAX_ENTRIES; i++) {
        const uint8_t *entry = base + i * ROMDIR_ENTRY_SZ;
        if ((uint32_t)((entry - bios->data) + ROMDIR_ENTRY_SZ) > bios->size)
            break;

        char name[11];
        memcpy(name, entry, 10); /* name is raw ASCII bytes, no endian issue */
        name[10] = '\0';

        if (name[0] == '\0')
            break;

        uint32_t size = rd_le32(entry + 12);

        if (strncmp(name, "ROMVER", 6) == 0) {
            romver_off = data_offset;
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
        /* ROMVER strings are typically newline-terminated ASCII
         * (e.g. "0100JC20000117\n") - trim any trailing newline for
         * cleaner display. */
        size_t len = strlen(bios->version_string);
        while (len > 0 && (bios->version_string[len - 1] == '\n' ||
                           bios->version_string[len - 1] == '\r')) {
            bios->version_string[--len] = '\0';
        }
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
