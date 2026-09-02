/*
 * Round 813 (task #811/#814, user-relayed external-review plan):
 * checkpoint-chained driver identical in structure to r811_cdvdtrace.c,
 * linked against an ee_core.c compiled WITH -DR813_CDVDTRACE so every
 * real EE->IOP SIF_SID_CDVD_NCMD / SIF_SID_CDVD_SCMD RPC call streams
 * to stderr as "[R813EVT] ..." - independent of and upstream from
 * iop_cdvd.c's own dispatch_ncmd()/dispatch_scmd() call counters
 * (iop_cdvd_get_ncmd_call_count()/get_scmd_call_count(), reported
 * below via the [R811-TRACE] lines this driver also prints).
 *
 * Purpose: settle, with direct runtime evidence against the Round 812
 * scheduler fix (commit 4996c5a), exactly which edge of the decision
 * boundary
 *   GT3 EE code -> EE CDVD syscall/API wrapper -> SIF RPC or CDVD MMIO
 *   -> IOP CDVD command handler -> dispatch_ncmd() -> completion
 *   interrupt/callback -> SignalSema(5)
 * is the first one GT3 never crosses. Purely observational - does NOT
 * modify dispatch, semaphore, or CDVD logic, and R813_CDVDTRACE is
 * unset (zero cost) in every normal/Wii build.
 *
 * Usage: r813_eecdvd_trace <bios> <disc> <ckpt_in> [chunks] [ckpt_out]
 * Same resumable checkpoint-chaining pattern as r811_cdvdtrace.c to
 * work around the sandbox's ~178s per-bash-call wall-clock cap.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/hw/iop_cdvd.h"
#include "core/hw/sif.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path> [chunks] [save_ckpt_path]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    int chunks = argc > 4 ? atoi(argv[4]) : 100;
    const char *save_ckpt_path = argc > 5 ? argv[5] : NULL;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    fprintf(stderr, "[R813-TRACE] start total_instr=%llu pc=0x%08x ncmd=%llu scmd=%llu\n",
            (unsigned long long)ee->instructions_executed, ee->pc,
            (unsigned long long)iop_cdvd_get_ncmd_call_count(),
            (unsigned long long)iop_cdvd_get_scmd_call_count());

    const long SLICE = 1000000;
    for (int c = 0; c < chunks; c++) {
        system_run_interleaved(SLICE);
        if (c % 50 == 0) {
            fprintf(stderr, "[R813-TRACE] progress chunk=%d total_instr=%llu pc=0x%08x ncmd=%llu scmd=%llu\n",
                    c, (unsigned long long)ee->instructions_executed, ee->pc,
                    (unsigned long long)iop_cdvd_get_ncmd_call_count(),
                    (unsigned long long)iop_cdvd_get_scmd_call_count());
        }
    }

    fprintf(stderr, "[R813-TRACE] FINAL total_instr=%llu pc=0x%08x ncmd=%llu scmd=%llu last_ncmd=%u last_scmd=%u\n",
            (unsigned long long)ee->instructions_executed, ee->pc,
            (unsigned long long)iop_cdvd_get_ncmd_call_count(),
            (unsigned long long)iop_cdvd_get_scmd_call_count(),
            iop_cdvd_get_last_ncmd_issued(), iop_cdvd_get_last_scmd_issued());

    if (save_ckpt_path) {
        if (checkpoint_save(save_ckpt_path) != 0) {
            fprintf(stderr, "[R813-TRACE] checkpoint_save(%s) FAILED\n", save_ckpt_path);
        } else {
            fprintf(stderr, "[R813-TRACE] checkpoint_save(%s) OK - resume from here next call\n", save_ckpt_path);
        }
    }
    return 0;
}
