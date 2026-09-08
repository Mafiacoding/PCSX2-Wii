/*
 * Round 833 (task #811 continuation, per user request "fix all ee
 * issues check the sdk source and pcsx2 also check for corrupt epc
 * and bad address"): fine-grained investigation of the pc==0x0
 * park Round 832 discovered mid-way through GT3's checkpoint chain.
 *
 * ee_core.c already has a Round 630/782 guard for exactly this
 * signature (pc==0 && Status.EXL==0): it either bounces to $ra (if
 * nonzero) or calls ee_hle_thread_exit_current() (if $ra==0, per the
 * real ps2sdk/kernel "threads never return" convention already cited
 * there) and counts every hit in g_ee_null_jalr_guard_hits. Round 832
 * did NOT read that counter, so it's unknown whether the guard is
 * even firing, or whether something else is happening (e.g. a
 * genuinely corrupt EPC/nested-exception state the guard's own
 * !EXL condition doesn't cover). This driver resumes the same
 * persisted checkpoint and, every 2,000,000 slices, dumps: pc, tid,
 * g_ee_null_jalr_guard_hits (cumulative), and a full thread census
 * (status/entry/saved_pc/wakeup_count/ra for every in-use TCB slot)
 * so the actual live behavior - not just its net effect - is visible.
 *
 * Usage: r833_pc0_investigate <bios_path> <disc_path> <ckpt_path> [n_steps] [slices_per_step]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/hw/iop_cdvd.h"
#include "core/hw/dma.h"

extern long g_ee_null_jalr_guard_hits;

#define EE_HLE_THREAD_MAX_THREADS 32

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path> [n_steps] [slices_per_step]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    int n_steps = argc > 4 ? atoi(argv[4]) : 5;
    uint64_t slices_per_step = argc > 5 ? strtoull(argv[5], NULL, 10) : 2000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();

    printf("[R833] resumed at total_instr=%llu pc=0x%08x guard_hits(pre)=%ld\n",
           (unsigned long long)ee->instructions_executed, ee->pc, g_ee_null_jalr_guard_hits);

    for (int step = 0; step < n_steps && !ee->halted; step++) {
        system_run_interleaved(slices_per_step);

        int tid = ee_hle_thread_get_current_thread_id();
        uint64_t ra_now = ee_hle_thread_get_gpr(tid, 31);
        printf("[R833] step=%d total_instr=%llu pc=0x%08x tid=%d ra_gpr31=0x%08llx guard_hits=%ld cop0_status=0x%08x cop0_cause=0x%08x cop0_epc=0x%08x\n",
               step, (unsigned long long)ee->instructions_executed, ee->pc, tid,
               (unsigned long long)ra_now, g_ee_null_jalr_guard_hits,
               ee->cop0[12], ee->cop0[13], ee->cop0[14]);

        for (int t = 1; t <= EE_HLE_THREAD_MAX_THREADS; t++) {
            uint32_t status = ee_hle_thread_get_status(t);
            if (status == 0) continue;
            uint32_t entry = ee_hle_thread_get_entry(t);
            uint32_t saved_pc = ee_hle_thread_get_saved_pc(t);
            uint32_t wake = ee_hle_thread_get_wakeup_count(t);
            uint64_t ra = ee_hle_thread_get_gpr(t, 31);
            uint32_t wtype = ee_hle_thread_get_wait_type(t);
            uint32_t wid = ee_hle_thread_get_wait_id(t);
            printf("  [R833-TCB] tid=%d status=0x%02x entry=0x%08x saved_pc=0x%08x wakeups=%u wait_type=%u wait_id=%u ra=0x%08llx\n",
                   t, status, entry, saved_pc, wake, wtype, wid, (unsigned long long)ra);
        }
        if (ee->halted) {
            printf("[R833] EE halted: %s\n", ee->halt_reason);
        }
    }

    if (!ee->halted) {
        char outpath[1024];
        snprintf(outpath, sizeof(outpath), "%s.r833next", ckpt_path);
        if (checkpoint_save(outpath) == 0)
            printf("[R833] checkpoint saved to %s\n", outpath);
    }
    return 0;
}
