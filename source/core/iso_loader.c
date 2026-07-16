/*
 * iso_loader.c - see include/core/iso_loader.h for scope notes and
 * full citation trail (Round 139, 179th finding).
 */
#include "core/iso_loader.h"
#include <string.h>
#include <stdlib.h>

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int iso_open(const char *path, iso_image_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!path || !out) return -1;

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    uint8_t sector[ISO_SECTOR_SIZE];
    if (fseek(fp, (long)(ISO_PVD_LBA * ISO_SECTOR_SIZE), SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    if (fread(sector, 1, ISO_SECTOR_SIZE, fp) != ISO_SECTOR_SIZE) {
        fclose(fp);
        return -1;
    }

    /* Real, cited ISO9660 PVD layout (ECMA-119): byte 0 = type (1 =
     * Primary Volume Descriptor), bytes 1-5 = "CD001" standard
     * identifier. */
    if (sector[0] != 1 || memcmp(&sector[1], "CD001", 5) != 0) {
        fclose(fp);
        return -1;
    }

    /* Root Directory Record: 34 bytes at offset 156 within the PVD
     * (ECMA-119). Extent LBA (both-endian, LE half) at record offset
     * 2; data length (both-endian, LE half) at record offset 10. */
    const uint8_t *root_rec = &sector[156];
    out->root_lba  = read_le32(&root_rec[2]);
    out->root_size = read_le32(&root_rec[10]);
    out->fp = fp;
    out->opened = 1;
    return 0;
}

void iso_close(iso_image_t *img)
{
    if (img && img->opened && img->fp) {
        fclose(img->fp);
        img->fp = NULL;
        img->opened = 0;
    }
}

int iso_read_sector(iso_image_t *img, uint32_t lba, uint8_t *buf)
{
    if (!img || !img->opened || !img->fp || !buf) return -1;
    if (fseek(img->fp, (long)((uint64_t)lba * ISO_SECTOR_SIZE), SEEK_SET) != 0)
        return -1;
    if (fread(buf, 1, ISO_SECTOR_SIZE, img->fp) != ISO_SECTOR_SIZE)
        return -1;
    return 0;
}

int iso_find_in_root(iso_image_t *img, const char *name, iso_dirent_t *out)
{
    if (!img || !img->opened || !name || !out) return -1;

    uint32_t remaining = img->root_size;
    uint32_t lba = img->root_lba;
    uint8_t sector[ISO_SECTOR_SIZE];

    while (remaining > 0) {
        if (iso_read_sector(img, lba, sector) != 0)
            return -1;

        uint32_t off = 0;
        /* Directory records never span a sector boundary on real
         * ISO9660 media (ECMA-119) - a length byte of 0 before the
         * sector is exhausted means "skip to next sector", the
         * standard's own documented padding convention. */
        while (off < ISO_SECTOR_SIZE) {
            uint8_t rec_len = sector[off];
            if (rec_len == 0) break; /* padding - next sector */
            if (off + rec_len > ISO_SECTOR_SIZE) break;

            const uint8_t *rec = &sector[off];
            uint8_t name_len = rec[32];
            uint8_t flags = rec[25];

            if (name_len > 0 && (33u + name_len) <= rec_len) {
                char entry_name[224];
                uint32_t copy_len = name_len < sizeof(entry_name) - 1 ? name_len : sizeof(entry_name) - 1;
                memcpy(entry_name, &rec[33], copy_len);
                entry_name[copy_len] = '\0';

                /* Skip the two special entries (name_len==1, byte
                 * 0x00 = self, 0x01 = parent - ECMA-119). */
                if (!(name_len == 1 && (rec[33] == 0x00 || rec[33] == 0x01))) {
                    if (strcmp(entry_name, name) == 0) {
                        out->lba = read_le32(&rec[2]);
                        out->size = read_le32(&rec[10]);
                        out->is_directory = (flags & 0x02u) ? 1 : 0;
                        memcpy(out->name, entry_name, copy_len + 1);
                        return 0;
                    }
                }
            }
            off += rec_len;
        }

        if (remaining <= ISO_SECTOR_SIZE) break;
        remaining -= ISO_SECTOR_SIZE;
        lba++;
    }
    return -1;
}
