/*
 * Round 773 scratch: dump SYSTEM.CNF content from a real disc image
 * using the already-tested, standalone host-side ISO9660 parser
 * (iso_loader.c, Round 139/170) - verification only, never committed.
 */
#include <stdio.h>
#include <string.h>
#include "core/iso_loader.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <disc_path>\n", argv[0]); return 1; }
    iso_image_t img;
    if (iso_open(argv[1], &img) != 0) { fprintf(stderr, "iso_open fail\n"); return 1; }
    printf("opened ok: root_lba=%u root_size=%u stride=%u data_offset=%u\n",
           img.root_lba, img.root_size, img.physical_stride, img.data_offset);

    iso_dirent_t de;
    const char *names[] = { "SYSTEM.CNF;1", "SYSTEM.CNF", NULL };
    int found = 0;
    for (int i = 0; names[i]; i++) {
        if (iso_find_in_root(&img, names[i], &de) == 0) {
            printf("found %s at lba=%u size=%u\n", names[i], de.lba, de.size);
            found = 1;
            uint8_t buf[2048];
            if (iso_read_sector(&img, de.lba, buf) == 0) {
                uint32_t n = de.size < sizeof(buf) ? de.size : sizeof(buf);
                printf("--- content (%u bytes) ---\n", n);
                fwrite(buf, 1, n, stdout);
                printf("\n--- end ---\n");
            }
            break;
        }
    }
    if (!found) printf("SYSTEM.CNF not found in root directory\n");
    iso_close(&img);
    return 0;
}
