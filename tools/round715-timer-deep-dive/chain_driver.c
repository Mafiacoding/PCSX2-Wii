/*
 * Round 715: checkpoint-chained driver (mirrors this project's own
 * established Round 382/383 checkpoint-chaining pattern) - runs a
 * fixed instruction budget per invocation, saving/loading a checkpoint
 * file so a deep, multi-hundred-million-instruction survey can proceed
 * across several separate tool-call-sized runs, each comfortably
 * inside the sandbox's per-call time budget.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <checkpoint_path> <start|continue> [budget]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *ckpt_path = argv[2];
    const char *mode = argv[3];
    uint64_t budget = argc > 4 ? strtoull(argv[4], NULL, 10) : 100000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }

    if (strcmp(mode, "start") == 0) {
        if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    } else {
        if (checkpoint_load(ckpt_path, &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }
    }

    uint64_t chunk = 4000000ull, done = 0;
    while (done < budget) {
        system_run_interleaved(chunk);
        done += chunk;
    }

    if (checkpoint_save(ckpt_path) != 0) { fprintf(stderr, "checkpoint_save fail\n"); return 1; }
    printf("[R715-CHAIN] ran %llu more instructions, checkpoint saved to %s\n",
           (unsigned long long)done, ckpt_path);
    return 0;
}
