/*
 * Round 811 (task #812, per user's explicit trace-only EE->IOP CDVD-request
 * boundary audit spec). Resumes GT3 from a checkpoint and runs forward,
 * relying on R811_CDVDTRACE-gated fprintf(stderr, ...) instrumentation
 * added this round in source/hw/iop_cdvd.c and source/core/ee/ee_core.c
 * to log:
 *   - every SIF_CMD_RPC_BIND / SIF_CMD_RPC_CALL the EE issues
 *   - every real CDVD MMIO register write (iop_cdvd_mmio_write8)
 *   - every dispatch_ncmd()/dispatch_scmd() enter/exit
 *   - the SCMD_CLOSECONFIG enter/exit + "next command type" tracking
 * Same checkpoint-chaining pattern as r810_sigtrip.c (save_ckpt_path arg)
 * to work around the sandbox's ~178s per-bash-call wall-clock cap.
 * Purely observational - does NOT modify dispatch, semaphore, or CDVD
 * logic. No SignalSema(5) synthesis, no speculative N-command dispatch.
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
    int chunks = argc > 4 ? atoi(argv[4]) : 400;
    const char *save_ckpt_path = argc > 5 ? argv[5] : NULL;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    fprintf(stderr, "[R811-TRACE] start total_instr=%llu pc=0x%08x ncmd=%llu scmd=%llu\n",
            (unsigned long long)ee->instructions_executed, ee->pc,
            (unsigned long long)iop_cdvd_get_ncmd_call_count(),
            (unsigned long long)iop_cdvd_get_scmd_call_count());

    const long SLICE = 1000000;
    for (int c = 0; c < chunks; c++) {
        system_run_interleaved(SLICE);
        if (c % 50 == 0) {
            fprintf(stderr, "[R811-TRACE] progress chunk=%d total_instr=%llu pc=0x%08x ncmd=%llu scmd=%llu\n",
                    c, (unsigned long long)ee->instructions_executed, ee->pc,
                    (unsigned long long)iop_cdvd_get_ncmd_call_count(),
                    (unsigned long long)iop_cdvd_get_scmd_call_count());
        }
    }

    fprintf(stderr, "[R811-TRACE] FINAL total_instr=%llu pc=0x%08x ncmd=%llu scmd=%llu last_ncmd=%u last_scmd=%u\n",
            (unsigned long long)ee->instructions_executed, ee->pc,
            (unsigned long long)iop_cdvd_get_ncmd_call_count(),
            (unsigned long long)iop_cdvd_get_scmd_call_count(),
            iop_cdvd_get_last_ncmd_issued(), iop_cdvd_get_last_scmd_issued());

    if (save_ckpt_path) {
        if (checkpoint_save(save_ckpt_path) != 0) {
            fprintf(stderr, "[R811-TRACE] checkpoint_save(%s) FAILED\n", save_ckpt_path);
        } else {
            fprintf(stderr, "[R811-TRACE] checkpoint_save(%s) OK - resume from here next call\n", save_ckpt_path);
        }
    }
    return 0;
}
