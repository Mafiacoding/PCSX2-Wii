/*
 * Round 810 (task #810 continuation): dump a window of EE RAM around
 * the three GT3 threads' saved_pc cluster (0x0101bb00-0x0101bc40, per
 * r808_thread_census against r781_gt3_test2.ckpt) so it can be fed to
 * tools/round655-ee-disasm/disasm.c and read as real MIPS code. Scratch
 * diagnostic only - not committed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <ckpt_path> <out_path> [base_hex] [len_hex]\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(argv[2], &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();

    uint32_t base = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 16) : 0x01000000u;
    uint32_t len  = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 16) : 0x2000u;

    FILE *f = fopen(argv[3], "wb");
    if (!f) { fprintf(stderr, "fopen fail\n"); return 1; }
    fwrite(ee->ram + base, 1, len, f);
    fclose(f);
    printf("wrote %s (EE RAM 0x%08x-0x%08x)\n", argv[3], base, base + len);
    return 0;
}
