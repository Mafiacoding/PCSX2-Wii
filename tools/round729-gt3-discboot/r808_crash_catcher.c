/*
 * Round 808 (task #805 fallback): resume GT3 from r781_gt3_test2.ckpt
 * (total_instr=678,449,972, thread 3 confirmed still alive/WAIT-SLEEP
 * via r808_census), and run forward in small slices until thread 3
 * transitions to DORMANT (the ra==0 null-jalr crash Round 782 patched),
 * then dump the last 256 (pc,ra) pairs ee_step() actually executed
 * (via ee_core.c's temporary g_r808_pc_hist/g_r808_ra_hist ring buffer)
 * to reconstruct exactly what code thread 3 was running immediately
 * before its call stack unwound to ra==0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"

extern uint32_t g_r808_pc_hist[256];
extern uint32_t g_r808_ra_hist[256];
extern int g_r808_pc_hist_idx;

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path>\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }

    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    printf("[R808-CRASH] start total_instr=%llu pc=0x%08x\n",
           (unsigned long long)ee->instructions_executed, ee->pc);

    /* Small slices so we can stop the moment thread 3 dies, rather than
     * overshooting by a whole 10M-instruction chunk. */
    const long SLICE = 200000;
    long slices = 0;
    const long MAX_SLICES = 4000; /* 800M instr ceiling, plenty past the ~596M gap observed */
    while (slices < MAX_SLICES) {
        system_run_interleaved(SLICE);
        slices++;
        uint32_t st3 = ee_hle_thread_get_status(3);
        if (st3 == 0x10u /* EE_THS_DORMANT */) {
            printf("[R808-CRASH] thread 3 went DORMANT after %ld slices (total_instr=%llu, pc=0x%08x)\n",
                   slices, (unsigned long long)ee->instructions_executed, ee->pc);
            break;
        }
        if (slices % 50 == 0) {
            printf("[R808-CRASH] progress: slice=%ld total_instr=%llu pc=0x%08x tid3_status=0x%x\n",
                   slices, (unsigned long long)ee->instructions_executed, ee->pc, st3);
        }
    }

    printf("[R808-CRASH] final total_instr=%llu pc=0x%08x halted=%u\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);

    printf("[R808-CRASH] last %d (pc,ra) pairs executed (oldest first):\n",
           g_r808_pc_hist_idx < 256 ? g_r808_pc_hist_idx : 256);
    int n = g_r808_pc_hist_idx < 256 ? g_r808_pc_hist_idx : 256;
    int start = g_r808_pc_hist_idx < 256 ? 0 : (g_r808_pc_hist_idx & 255);
    for (int i = 0; i < n; i++) {
        int idx = (start + i) & 255;
        printf("[R808-CRASH]   #%d pc=0x%08x ra=0x%08x\n", i, g_r808_pc_hist[idx], g_r808_ra_hist[idx]);
    }
    return 0;
}
