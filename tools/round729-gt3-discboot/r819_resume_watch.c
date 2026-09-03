/*
 * Round 819 (task #832): resume the Round 818 persisted checkpoint and
 * run forward, with two SCRATCH-only diagnostics enabled
 * (R819_ADDR_WATCH in a scratch copy of ee_core.c, R819_SLEEP_TRACE in
 * a scratch copy of ee_hle_thread.c - neither ever applied to tracked
 * source), to answer the user's point 5: does GT3's SleepThread/
 * WakeupThread poll loop at 0x0100D8D0 (found this round via static
 * disassembly of the Round 818 checkpoint's resting pc=0x0100d934)
 * ever see its guarded value at EE vaddr 0x01047B00 change, and if so
 * who writes it / who calls WakeupThread(3) to drive the loop.
 *
 * Usage: r819_resume_watch <bios_path> <disc_path> <ckpt_path> [budget]
 * (same SLICE=1,000,000-per-loop budget convention as r818_sema_producer.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/hw/iop_cdvd.h"
#include "core/checkpoint.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path> [budget]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    uint64_t budget = argc > 4 ? strtoull(argv[4], NULL, 10) : 5000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);

    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) {
        fprintf(stderr, "checkpoint_load FAILED for %s\n", ckpt_path);
        return 1;
    }

    ee_state_t *ee = ee_core_get_state();
    fprintf(stderr, "[R819-RESUME] loaded %s: total_instr=%llu pc=0x%08x cur_tid=%d budget=%llu\n",
            ckpt_path, (unsigned long long)ee->instructions_executed, ee->pc,
            ee_hle_thread_get_current_thread_id(), (unsigned long long)budget);

    const long SLICE = 1000000;
    uint64_t done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(SLICE);
        done += (uint64_t)SLICE;
    }

    fprintf(stderr, "[R819-RESUME] FINAL total_instr=%llu pc=0x%08x halted=%d cur_tid=%d ncmd=%llu scmd=%llu\n",
            (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
            ee_hle_thread_get_current_thread_id(),
            (unsigned long long)iop_cdvd_get_ncmd_call_count(),
            (unsigned long long)iop_cdvd_get_scmd_call_count());

    int count = ee_hle_thread_get_thread_count();
    for (int t = 1; t <= count; t++) {
        fprintf(stderr, "[R819-RESUME] final tid=%d status=0x%x wait_type=%u wait_id=%u saved_pc=0x%08x prio=%u\n",
                t, ee_hle_thread_get_status(t), ee_hle_thread_get_wait_type(t),
                ee_hle_thread_get_wait_id(t), ee_hle_thread_get_saved_pc(t), ee_hle_thread_get_priority(t));
    }

    return 0;
}
