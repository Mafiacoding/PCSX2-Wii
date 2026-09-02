/*
 * Round 812 (task #813, user-relayed external-review plan): checkpoint-
 * chained driver identical in structure to chain_driver.c, but linked
 * against an ee_hle_thread.c compiled WITH -DR812_EVENTLOG so every
 * WaitSema/WakeupThread/wake_one_sema_waiter/reschedule/save-context/
 * load-context/status-transition/current_thread_id-write event streams
 * to stderr as "[R812EVT] ..." lines. Diagnostic-only; not part of any
 * normal build (see ee_hle_thread.c's own Round 812 header comment).
 *
 * Purpose: settle, with direct runtime evidence rather than static
 * disassembly, whether GT3 thread 1's WAIT/SEMA/5 state (first observed
 * by Round 811b, already present in the earliest available checkpoint
 * at total_instr=38,865,331) reflects (a) a genuine WaitSema(5) block
 * that was never cleanly woken, with the thread's saved pc drifting
 * afterward some other way, (b) a "quiet pass-through" where WaitSema
 * succeeded but its wait_type/wait_id/status fields were never cleared
 * (a real emulator bug), or (c) TCB slot 1 being freed and reallocated
 * to an unrelated logical thread that independently blocked on the same
 * semaphore. Because thread 1 is ALREADY WAIT/SEMA/5 in the earliest
 * checkpoint on disk, this driver must run from a cold boot (mode
 * "start") to capture the actual transition into that state - resuming
 * from any existing checkpoint would already be past the moment of
 * interest.
 *
 * Filtered to tid=1 by default (see ee_hle_thread_eventlog_set_filter())
 * to keep stderr volume manageable across a ~39M-instruction cold boot;
 * the filter call must run every invocation since g_evt_filter_tid is a
 * static that resets on each process start (checkpoint files do not
 * carry the diagnostic-only filter state).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/hw/gs.h"
#include "core/hw/gif.h"
#include "core/hw/vu.h"
#include "core/hw/iop_cdvd.h"

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path> <start|continue> [budget] [filter_tid]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    const char *mode = argv[4];
    uint64_t budget = argc > 5 ? strtoull(argv[5], NULL, 10) : 5000000ull;
    int filter_tid = argc > 6 ? atoi(argv[6]) : 1;

    ee_hle_thread_eventlog_set_filter(filter_tid);

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }

    if (strcmp(mode, "start") == 0) {
        if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
        if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
        iop_cdvd_set_disc_present(0x12 /* CDVD_TYPE_PS2CD, Round 170's cited constant, Round 750 fix */);
    } else {
        if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }
    }

    ee_state_t  *ee  = ee_core_get_state();

    uint64_t chunk = 1000000ull, done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
    }

    int tid = ee_hle_thread_get_current_thread_id();
    printf("[R812-CHAIN] ran %llu more, total_instr=%llu pc=0x%08x halted=%u tid=%d\n",
           (unsigned long long)done, (unsigned long long)ee->instructions_executed,
           ee->pc, ee->halted, tid);

    if (ee->halted) {
        printf("[R812-CHAIN] EE halted: %s\n", ee->halt_reason);
        return 0;
    }

    if (checkpoint_save(ckpt_path) != 0) { fprintf(stderr, "checkpoint_save fail\n"); return 1; }
    printf("[R812-CHAIN] checkpoint saved to %s\n", ckpt_path);
    return 0;
}
