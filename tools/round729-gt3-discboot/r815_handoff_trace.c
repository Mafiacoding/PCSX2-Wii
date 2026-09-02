/*
 * Round 815 (task #811/#820, direct continuation of Round 814 per the
 * user's own next-step instruction): checkpoint-chained cold-boot
 * driver, identical in structure to r812_eventlog.c/r813_eecdvd_trace.c/
 * r814_closeconfig_trace.c, but linked against an ee_core.c/
 * ee_hle_thread.c compiled WITH -DR815_HANDOFF_TRACE (and, for the
 * detailed thread/sema event log once past the handoff, -DR812_EVENTLOG
 * too) so:
 *   - the real LF_F_ELF_LOAD RPC reply for GT3's own disc ELF logs its
 *     epc/gp ("[R815EVT] ELF-LOAD-REPLY ...")
 *   - the exact instant EE PC first reaches that epc logs the full
 *     handoff-boundary register/thread-state snapshot the user asked
 *     for ("[R815EVT] HANDOFF ...")
 *   - the following (bounded) window logs newly-seen PCs ("[R815EVT]
 *     BLOCK ..."), every syscall number ("[R815EVT] SYSCALL ..."), and
 *     the first post-handoff CDVD-service RPC bind ("[R815EVT]
 *     FIRST-CDVD-BIND ...")
 *   - R812_EVENTLOG's own existing thread create/start/wakeup/
 *     sema event log (unchanged since Round 812) is scoped to start
 *     exactly at the handoff via the new Round 815
 *     ee_hle_thread_eventlog_set_enabled() runtime switch, so its
 *     output isn't drowned in pre-handoff BIOS/EELOAD boot noise.
 *
 * WHY COLD BOOT ("start" mode) IS REQUIRED: same rationale as every
 * prior r81N tool in this directory - the real LF_F_ELF_LOAD RPC for
 * GT3's own ELF happens early in boot (Round 554/558's own citation),
 * long before any multi-billion-instruction checkpoint's resting
 * point, so resuming from a late checkpoint would already be past the
 * moment of interest.
 *
 * Purely observational - does NOT change dispatch_ncmd()/SIF/CDVD-
 * completion/semaphore-signaling/ELF-load behavior in any way. Both
 * R815_HANDOFF_TRACE and R812_EVENTLOG are unset (zero cost) in every
 * normal/Wii build.
 *
 * Usage: r815_handoff_trace <bios> <disc> <ckpt_path> <start|continue> [budget] [save_ckpt_path]
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
    fprintf(stderr, "[R815-TRACE] start total_instr=%llu pc=0x%08x ncmd=%llu scmd=%llu\n",
            (unsigned long long)ee->instructions_executed, ee->pc,
            (unsigned long long)iop_cdvd_get_ncmd_call_count(),
            (unsigned long long)iop_cdvd_get_scmd_call_count());

    const long SLICE = 1000000;
    uint64_t done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(SLICE);
        done += (uint64_t)SLICE;
        if ((done / (uint64_t)SLICE) % 50 == 0) {
            fprintf(stderr, "[R815-TRACE] progress done=%llu total_instr=%llu pc=0x%08x scmd=%llu\n",
                    (unsigned long long)done, (unsigned long long)ee->instructions_executed,
                    ee->pc, (unsigned long long)iop_cdvd_get_scmd_call_count());
        }
    }

    fprintf(stderr, "[R815-TRACE] FINAL total_instr=%llu pc=0x%08x halted=%u ncmd=%llu scmd=%llu\n",
            (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
            (unsigned long long)iop_cdvd_get_ncmd_call_count(),
            (unsigned long long)iop_cdvd_get_scmd_call_count());

    if (ee->halted) {
        fprintf(stderr, "[R815-TRACE] EE halted: %s\n", ee->halt_reason);
        return 0;
    }

    if (save_ckpt_path) {
        if (checkpoint_save(save_ckpt_path) != 0) {
            fprintf(stderr, "[R815-TRACE] checkpoint_save(%s) FAILED\n", save_ckpt_path);
        } else {
            fprintf(stderr, "[R815-TRACE] checkpoint_save(%s) OK - resume from here next call\n", save_ckpt_path);
        }
    }
    return 0;
}
