/*
 * Round 814 (task #811/#818, direct continuation of Round 813 + the
 * user-relayed external-review plan's own explicit next step):
 * checkpoint-chained driver identical in structure to r812_eventlog.c/
 * r813_eecdvd_trace.c, but linked against an ee_core.c compiled WITH
 * -DR814_CLOSECONFIG_TRACE so the SIF_SID_CDVD_SCMD catch-all's own
 * rpc_number==0xF (real CD_SCMD_CLOSE_CONFIG) branch, its matching
 * delayed-delivery event in ee_check_rpc_bind_pending(), and a bounded
 * post-delivery instruction trace in ee_step() all stream to stderr as
 * "[R814EVT] ..." lines.
 *
 * WHY COLD BOOT ("start" mode) IS REQUIRED: Round 813 established that
 * SCMD_CLOSECONFIG is the LAST of GT3's initial 13-call S-command
 * config burst, which happens very early in boot (long before the
 * multi-billion-instruction resting point r812_cold.ckpt represents).
 * Resuming from any checkpoint taken after that burst would already be
 * past the moment of interest - same rationale r812_eventlog.c's own
 * header comment already established for GT3 thread 1's WAIT/SEMA/5
 * state.
 *
 * WHAT THIS ANSWERS: the exact "call -> completion -> continuation"
 * boundary chain the user's relayed external-review plan asked for:
 *   SCMD_CLOSECONFIG completion -> EE return/status value ->
 *   caller continuation -> branch or wait condition -> expected
 *   loader/read request
 * Recorded at the completion boundary: EE PC and $ra, current TID,
 * thread status/wait_type/wait_id (syscall-level context), $v0 before
 * and after, $a0-$a3, and the next several post-delivery branch
 * targets (via the bounded PC trace). Purely observational - does NOT
 * change dispatch_ncmd()/SIF/CDVD-completion/semaphore-signaling
 * behavior in any way, and R814_CLOSECONFIG_TRACE is unset (zero cost)
 * in every normal/Wii build.
 *
 * Usage: r814_closeconfig_trace <bios> <disc> <ckpt_path> <start|continue> [budget] [save_ckpt_path]
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

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path> <start|continue> [budget] [save_ckpt_path]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    const char *mode = argv[4];
    uint64_t budget = argc > 5 ? strtoull(argv[5], NULL, 10) : 100000000ull;
    const char *save_ckpt_path = argc > 6 ? argv[6] : NULL;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }

    if (strcmp(mode, "start") == 0) {
        if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
        if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
        iop_cdvd_set_disc_present(0x12 /* CDVD_TYPE_PS2CD, Round 170's cited constant, Round 750 fix */);
    } else {
        if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }
    }

    ee_state_t *ee = ee_core_get_state();
    fprintf(stderr, "[R814-TRACE] start total_instr=%llu pc=0x%08x ncmd=%llu scmd=%llu\n",
            (unsigned long long)ee->instructions_executed, ee->pc,
            (unsigned long long)iop_cdvd_get_ncmd_call_count(),
            (unsigned long long)iop_cdvd_get_scmd_call_count());

    const long SLICE = 1000000;
    uint64_t done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(SLICE);
        done += (uint64_t)SLICE;
        if ((done / (uint64_t)SLICE) % 50 == 0) {
            fprintf(stderr, "[R814-TRACE] progress done=%llu total_instr=%llu pc=0x%08x scmd=%llu\n",
                    (unsigned long long)done, (unsigned long long)ee->instructions_executed,
                    ee->pc, (unsigned long long)iop_cdvd_get_scmd_call_count());
        }
    }

    fprintf(stderr, "[R814-TRACE] FINAL total_instr=%llu pc=0x%08x halted=%u ncmd=%llu scmd=%llu last_scmd=%u\n",
            (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
            (unsigned long long)iop_cdvd_get_ncmd_call_count(),
            (unsigned long long)iop_cdvd_get_scmd_call_count(),
            iop_cdvd_get_last_scmd_issued());

    if (ee->halted) {
        fprintf(stderr, "[R814-TRACE] EE halted: %s\n", ee->halt_reason);
        return 0;
    }

    if (save_ckpt_path) {
        if (checkpoint_save(save_ckpt_path) != 0) {
            fprintf(stderr, "[R814-TRACE] checkpoint_save(%s) FAILED\n", save_ckpt_path);
        } else {
            fprintf(stderr, "[R814-TRACE] checkpoint_save(%s) OK - resume from here next call\n", save_ckpt_path);
        }
    }
    return 0;
}
