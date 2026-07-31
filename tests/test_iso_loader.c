/*
 * test_iso_loader.c - host-native test for source/core/iso_loader.c
 *
 * Builds a small, entirely synthetic ISO9660 image on disk (a public,
 * non-proprietary filesystem format - not PS2 BIOS/game content, see
 * iso_loader.h's scope note) with a Primary Volume Descriptor and a
 * root directory containing one file, then exercises iso_open(),
 * iso_find_in_root(), and iso_read_sector() against it.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "core/iso_loader.c"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("ok:   %s\n", msg); } \
    else { printf("FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}
static void write_both32(uint8_t *p, uint32_t v)
{
    write_le32(p, v);
    write_be32(p + 4, v);
}

int main(void)
{
    const char *path = "/tmp/test_synth.iso";
    const uint32_t root_lba = 20;      /* directory extent */
    const uint32_t file_lba = 21;      /* file "SYSTEM.CNF;1" extent */
    const char *file_name = "SYSTEM.CNF;1";
    const char *file_contents = "BOOT2 = cdrom0:\\SLUS_123.45;1\r\n";
    uint32_t file_len = (uint32_t)strlen(file_contents);

    FILE *fp = fopen(path, "wb");
    if (!fp) { printf("FAIL: could not create synthetic test image\n"); return 1; }

    uint8_t zero_sector[ISO_SECTOR_SIZE];
    memset(zero_sector, 0, sizeof(zero_sector));

    /* Sectors 0-15: unused (real ISO9660 reserves these as "system
     * area" - left as zero padding here, not exercised by this
     * project's parser). */
    for (int i = 0; i < 16; i++)
        fwrite(zero_sector, 1, ISO_SECTOR_SIZE, fp);

    /* Sector 16: Primary Volume Descriptor. */
    uint8_t pvd[ISO_SECTOR_SIZE];
    memset(pvd, 0, sizeof(pvd));
    pvd[0] = 1; /* type = Primary Volume Descriptor */
    memcpy(&pvd[1], "CD001", 5);
    pvd[6] = 1; /* version */

    uint8_t *root_rec = &pvd[156];
    root_rec[0] = 34;             /* directory record length */
    write_both32(&root_rec[2], root_lba);
    write_both32(&root_rec[10], ISO_SECTOR_SIZE); /* one sector of directory content */
    root_rec[25] = 0x02;          /* flags: is directory */
    root_rec[32] = 1;             /* name length */
    root_rec[33] = 0x00;          /* name: self ("\0") */
    fwrite(pvd, 1, ISO_SECTOR_SIZE, fp);

    /* Sector 17: Volume Descriptor Set Terminator (type 255). */
    uint8_t term[ISO_SECTOR_SIZE];
    memset(term, 0, sizeof(term));
    term[0] = 255;
    memcpy(&term[1], "CD001", 5);
    fwrite(term, 1, ISO_SECTOR_SIZE, fp);

    /* Sectors 18-19: padding to reach root_lba=20. */
    fwrite(zero_sector, 1, ISO_SECTOR_SIZE, fp);
    fwrite(zero_sector, 1, ISO_SECTOR_SIZE, fp);

    /* Sector 20 (root_lba): root directory content - self entry,
     * parent entry, then our one file entry. */
    uint8_t dir[ISO_SECTOR_SIZE];
    memset(dir, 0, sizeof(dir));
    uint32_t off = 0;

    /* self */
    dir[off + 0] = 34;
    write_both32(&dir[off + 2], root_lba);
    write_both32(&dir[off + 10], ISO_SECTOR_SIZE);
    dir[off + 25] = 0x02;
    dir[off + 32] = 1;
    dir[off + 33] = 0x00;
    off += 34;

    /* parent */
    dir[off + 0] = 34;
    write_both32(&dir[off + 2], root_lba);
    write_both32(&dir[off + 10], ISO_SECTOR_SIZE);
    dir[off + 25] = 0x02;
    dir[off + 32] = 1;
    dir[off + 33] = 0x01;
    off += 34;

    /* our file */
    uint8_t name_len = (uint8_t)strlen(file_name);
    uint8_t rec_len = (uint8_t)(33 + name_len);
    dir[off + 0] = rec_len;
    write_both32(&dir[off + 2], file_lba);
    write_both32(&dir[off + 10], file_len);
    dir[off + 25] = 0x00; /* not a directory */
    dir[off + 32] = name_len;
    memcpy(&dir[off + 33], file_name, name_len);
    off += rec_len;

    fwrite(dir, 1, ISO_SECTOR_SIZE, fp);

    /* Sector 21 (file_lba): file contents. */
    uint8_t file_sector[ISO_SECTOR_SIZE];
    memset(file_sector, 0, sizeof(file_sector));
    memcpy(file_sector, file_contents, file_len);
    fwrite(file_sector, 1, ISO_SECTOR_SIZE, fp);

    fclose(fp);

    /* --- now exercise the real loader against this synthetic image --- */
    iso_image_t img;
    CHECK(iso_open(path, &img) == 0, "iso_open succeeds on a well-formed synthetic image");
    CHECK(img.root_lba == root_lba, "parsed root directory LBA matches");
    CHECK(img.root_size == ISO_SECTOR_SIZE, "parsed root directory size matches");

    iso_dirent_t ent;
    CHECK(iso_find_in_root(&img, file_name, &ent) == 0, "iso_find_in_root locates the file by exact name");
    CHECK(ent.lba == file_lba, "found entry's LBA matches");
    CHECK(ent.size == file_len, "found entry's size matches");
    CHECK(ent.is_directory == 0, "found entry is correctly not a directory");

    iso_dirent_t missing;
    CHECK(iso_find_in_root(&img, "NOSUCHFILE.TXT;1", &missing) != 0, "iso_find_in_root correctly fails for a nonexistent name");

    uint8_t sector_buf[ISO_SECTOR_SIZE];
    CHECK(iso_read_sector(&img, ent.lba, sector_buf) == 0, "iso_read_sector succeeds for the file's extent");
    CHECK(memcmp(sector_buf, file_contents, file_len) == 0, "sector contents match what was written");

    iso_image_t bad;
    CHECK(iso_open("/tmp/this_path_does_not_exist_12345.iso", &bad) != 0, "iso_open correctly fails for a missing file");

    iso_close(&img);
    remove(path);

    if (g_fail) {
        printf("\nSOME CHECKS FAILED.\n");
        return 1;
    }
    printf("\nAll checks passed.\n");
    return 0;
}
