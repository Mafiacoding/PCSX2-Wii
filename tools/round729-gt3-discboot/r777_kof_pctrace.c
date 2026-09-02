/*
 * Round 777 (task #785): fine-grained PC trace. Resumes from the live
 * KOF checkpoint and single-steps a short window, logging every
 * distinct PC value visited (with a repeat counter to keep output
 * manageable) and the raw instruction word at each new PC, to see
 * exactly what code KOF's EE core is executing right now - not a
 * stale snapshot from an earlier checkpoint generation.
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
        fprintf(stderr, "usage: %s <bios_path> <ckpt_path> <num_steps>\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *ckpt_path = argv[2];
    long steps = strtol(argv[3], NULL, 10);

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    uint32_t last_pc = 0xFFFFFFFF;
    long run = 0;

    for (long i = 0; i < steps && !ee->halted; i++) {
        uint32_t pc = ee->pc;
        if (pc != last_pc) {
            if (last_pc != 0xFFFFFFFF)
                printf("  (repeated %ld times)\n", run);
            uint32_t phys = pc & 0x1FFFFFFF;
            uint32_t instr = 0;
            memcpy(&instr, ee->ram + phys, 4);
            printf("[R777-PCTRACE] i=%ld pc=0x%08x instr=0x%08x", i, pc, instr);
            last_pc = pc;
            run = 0;
        }
        run++;
        ee_core_step();
    }
    printf("  (repeated %ld times)\n", run);
    printf("[R777-PCTRACE] done, final pc=0x%08x total_instr=%llu halted=%u\n",
           ee->pc, (unsigned long long)ee->instructions_executed, ee->halted);
    return 0;
}
