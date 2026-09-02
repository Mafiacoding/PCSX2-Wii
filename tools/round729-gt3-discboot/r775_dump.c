/*
 * Round 775 scratch: load a checkpoint and dump raw instruction words
 * around given PCs to stdout as "addr word" pairs, for feeding into
 * tools/round655-ee-disasm/disasm.c. Not committed as a fix - pure
 * read-only diagnostic reusing the existing checkpoint_load() path.
 */
#include <stdio.h>
#include <stdlib.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s <bios> <disc> <ckpt> <hexaddr> [count]\n", argv[0]); return 1; }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios fail\n"); return 1; }
    if (checkpoint_load(argv[3], &bios, &bios, argv[2]) != 0) { fprintf(stderr, "ckpt fail\n"); return 1; }
    ee_state_t *ee = ee_core_get_state();
    unsigned int addr = strtoul(argv[4], NULL, 16);
    int count = argc > 5 ? atoi(argv[5]) : 16;
    for (int i = -count/2; i < count/2; i++) {
        unsigned int a = addr + i*4;
        unsigned int off = a & 0x1FFFFFFFu;
        if (off + 4 > 32*1024*1024) continue;
        unsigned int word = *(unsigned int*)(ee->ram + off);
        printf("%08x %08x\n", a, word);
    }
    return 0;
}
