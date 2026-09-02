/*
 * Round 777 (task #785): dump a window of EE RAM from the live KOF
 * checkpoint to a flat binary file, for disassembly with the existing
 * Round 655 EE disassembler (tools/round655-ee-disasm/disasm.c).
 * Companion to r777_kof_census.c - answers "what code is actually at
 * the resting pc values" rather than just "what state is the thread
 * in".
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
    if (argc < 6) {
        fprintf(stderr, "usage: %s <bios_path> <ckpt_path> <ram_addr_hex> <len_bytes> <out_path>\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *ckpt_path = argv[2];
    uint32_t addr = strtoul(argv[3], NULL, 16);
    uint32_t len = strtoul(argv[4], NULL, 10);
    const char *out_path = argv[5];

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    uint32_t phys = addr & 0x1FFFFFFF;

    FILE *f = fopen(out_path, "wb");
    if (!f) { fprintf(stderr, "fopen fail\n"); return 1; }
    fwrite(ee->ram + phys, 1, len, f);
    fclose(f);
    printf("[R777-RAMDUMP] wrote %u bytes from 0x%08x (phys 0x%08x) to %s\n", len, addr, phys, out_path);
    return 0;
}
