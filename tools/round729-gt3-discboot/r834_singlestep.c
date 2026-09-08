/* Round 834 (task #811 continuation): single-slice-granularity tracer
 * to see the EXACT live pc/gpr bounce sequence around the permanent
 * pc==0 park discovered in Round 832/833, since Round 833's 2M-slice
 * sampling granularity is far too coarse to see what happens between
 * samples (g_ee_null_jalr_guard_hits fires ~8 times per slice, i.e.
 * on essentially every dispatch opportunity, so real forward progress
 * within one slice is a handful of real instructions at most).
 * Silences system_run_interleaved's own per-call diagnostic printf
 * (same technique as r830_dmac_trace.c) since it fires every time at
 * single-slice granularity.
 * Usage: r834_singlestep <bios_path> <disc_path> <ckpt_path> [n_slices]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"

extern long g_ee_null_jalr_guard_hits;

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path> [n_slices]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    int n_slices = argc > 4 ? atoi(argv[4]) : 100;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();

    int saved_stdout = dup(1);
    int devnull = open("/dev/null", O_WRONLY);

    long prev_guard = g_ee_null_jalr_guard_hits;
    uint64_t prev_instr = ee->instructions_executed;
    for (int i = 0; i < n_slices; i++) {
        if (devnull >= 0) dup2(devnull, 1);
        system_run_interleaved(1);
        if (saved_stdout >= 0) dup2(saved_stdout, 1);

        long g_now = g_ee_null_jalr_guard_hits;
        printf("[R834] slice=%d pc=0x%08x instr=%llu(+%llu) v0=0x%08x v1=0x%08x a0=0x%08x a1=0x%08x s1=0x%08x s2=0x%08x ra=0x%08x guard_delta=%ld\n",
               i, ee->pc, (unsigned long long)ee->instructions_executed,
               (unsigned long long)(ee->instructions_executed - prev_instr),
               (uint32_t)ee->gpr[2].ud0, (uint32_t)ee->gpr[3].ud0,
               (uint32_t)ee->gpr[4].ud0, (uint32_t)ee->gpr[5].ud0,
               (uint32_t)ee->gpr[17].ud0, (uint32_t)ee->gpr[18].ud0,
               (uint32_t)ee->gpr[31].ud0, g_now - prev_guard);
        prev_guard = g_now;
        prev_instr = ee->instructions_executed;
    }
    if (devnull >= 0) close(devnull);
    return 0;
}
