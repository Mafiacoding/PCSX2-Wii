/*
 * Round 810 continuation (task #810, per user's explicit spec): reversible
 * tripwire run. Resumes GT3 from r781_gt3_test2.ckpt and runs a much
 * longer window than r808_sem_watch.c did (through thread 3's death and
 * well beyond), relying on ee_hle_thread.c's R810_SIGTRIP-gated printf
 * (added this round, reverted after) to log every real SignalSema/
 * iSignalSema call as it happens. Also dumps ee_core.c's existing,
 * permanent (Round 736) AddIntcHandler observation log at the end, to
 * check whether GT3 ever registers a VBLANK handler - the leading
 * hypothesis for semaphore 5's real producer, per Round 781's citation
 * of ps2sdk's graph_add_vsync_handler() "interrupt + semaphore" idiom.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path> [chunks] [save_ckpt_path]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    int chunks = argc > 4 ? atoi(argv[4]) : 400;
    const char *save_ckpt_path = argc > 5 ? argv[5] : NULL;
    /* Round 810 chaining addendum: the sandbox's hard ~178s per-call wall
     * clock caps a single invocation to ~70 chunks (1M slices each) at
     * this interpreter's throughput - nowhere near enough to cover the
     * ~600M-slice span needed to confirm sig5 stays 0 well past thread
     * 3's death. Save a checkpoint at the end of THIS call's budget so a
     * follow-up call can resume exactly where this one left off, same
     * checkpoint-chaining pattern this project has used since Round 382. */

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    fprintf(stderr, "[R810-TRIP] start total_instr=%llu pc=0x%08x\n",
            (unsigned long long)ee->instructions_executed, ee->pc);

    const long SLICE = 1000000;
    for (int c = 0; c < chunks; c++) {
        system_run_interleaved(SLICE);
        if (c % 50 == 0) {
            fprintf(stderr, "[R810-TRIP] progress chunk=%d total_instr=%llu pc=0x%08x sig5=%llu sig0=%llu\n",
                    c, (unsigned long long)ee->instructions_executed, ee->pc,
                    (unsigned long long)ee_hle_thread_get_signal_calls(5),
                    (unsigned long long)ee_hle_thread_get_signal_calls(0));
        }
    }

    fprintf(stderr, "[R810-TRIP] FINAL total_instr=%llu pc=0x%08x sig5=%llu sig0=%llu\n",
            (unsigned long long)ee->instructions_executed, ee->pc,
            (unsigned long long)ee_hle_thread_get_signal_calls(5),
            (unsigned long long)ee_hle_thread_get_signal_calls(0));

    if (save_ckpt_path) {
        if (checkpoint_save(save_ckpt_path) != 0) {
            fprintf(stderr, "[R810-TRIP] checkpoint_save(%s) FAILED\n", save_ckpt_path);
        } else {
            fprintf(stderr, "[R810-TRIP] checkpoint_save(%s) OK - resume from here next call\n", save_ckpt_path);
        }
    }

    /* AddIntcHandler observation log (Round 736, permanent, non-diagnostic) */
    uint32_t n = ee_core_get_addintc_log_count();
    fprintf(stderr, "[R810-TRIP] AddIntcHandler log entries: %u\n", n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cause, handler_addr, next, call_pc;
        if (ee_core_get_addintc_log_entry(i, &cause, &handler_addr, &next, &call_pc)) {
            fprintf(stderr, "[R810-TRIP]   #%u cause=%u handler_addr=0x%08x next=0x%08x call_pc=0x%08x%s\n",
                    i, cause, handler_addr, next, call_pc,
                    (cause == 2 || cause == 3) ? "  <-- VBLANK" : "");
        }
    }
    return 0;
}
