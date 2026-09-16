/*
 * Round 939 (task #924, continuation of task #887 per Round 925's
 * finding): diskless BIOS boot's PMODE stayed 0x00 across a
 * 1,055,999,045-instruction survey (Round 925). Real-hardware
 * precedent (Round 742-745, live PCSX2 DebugServer on a genuine
 * disc-free boot) found continuing activity (periodic XGKICKs) out
 * past 6.96 BILLION cycles before this project stopped watching -
 * so "still zero at ~1B instructions" may just mean "needs several
 * billion more", not a bug. This driver checkpoint-chains the
 * diskless path (system_init(), deliberately no iop_cdvd_mount_iso()
 * call - matches source/main.c's own diskless fallback exactly) out
 * to a multi-billion-instruction budget, sampling PMODE/DISPFB1/
 * DISPFB2/DISPLAY1/DISPLAY2 periodically, mirroring the established
 * checkpoint-chaining pattern (Round 715/729/382/383/936).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_intc.h"
#include "core/hw/gs.h"
#include "core/hw/gif.h"
#include "core/hw/vu.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <ckpt_path> <start|continue> [budget]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *ckpt_path = argv[2];
    const char *mode = argc > 3 ? argv[3] : "start";
    uint64_t budget = argc > 4 ? strtoull(argv[4], NULL, 10) : 1000000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }

    if (strcmp(mode, "start") == 0) {
        if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
        /* Deliberately NOT calling iop_cdvd_mount_iso() - this is the
         * diskless path, matching main.c's own fallback when no
         * sd:/pcsx2/games/game.{bin,iso} is found. */
    } else {
        if (checkpoint_load(ckpt_path, &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }
    }

    ee_state_t  *ee  = ee_core_get_state();
    gs_state_t  *gs  = gs_get_state();
    gif_state_t *gif = gif_get_state();
    vu1_state_t *vu1 = vu1_get_state();

    uint64_t chunk = 5000000ull, done = 0;
    uint64_t last_report = 0;
    uint8_t last_pmode = gs->pmode;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;

        if (gs->pmode != last_pmode || done - last_report >= 100000000ull) {
            fprintf(stderr, "[R939PC] instr=%llu ee_pc=0x%08x pmode=0x%02x dispfb1=0x%08x dispfb2=0x%08x "
                    "display1=0x%016llx display2=0x%016llx\n",
                    (unsigned long long)done, ee->pc, (unsigned)gs->pmode,
                    (unsigned)gs->dispfb1, (unsigned)gs->dispfb2,
                    (unsigned long long)gs->display1, (unsigned long long)gs->display2);
            last_pmode = gs->pmode;
            last_report = done;
        }
        if (gs->pmode != 0) {
            fprintf(stderr, "[R939-MILESTONE] PMODE went nonzero at instr=%llu! pmode=0x%02x\n",
                    (unsigned long long)done, (unsigned)gs->pmode);
            break;
        }
    }

    int tid = ee_hle_thread_get_current_thread_id();
    iop_state_t *iop = iop_core_get_state();
    printf("[R939-CHAIN] ran %llu more, total_instr=%llu ee_pc=0x%08x halted=%u tid=%d "
           "iop_pc=0x%08x vu1_instr=%llu gif_path1=%llu pmode=0x%02x dispfb1=0x%08x dispfb2=0x%08x\n",
           (unsigned long long)done, (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, tid,
           iop->pc,
           (unsigned long long)(vu1 ? vu1->instructions_executed : 0),
           (unsigned long long)(gif ? gif->gif_path1_transfers : 0),
           (unsigned)gs->pmode, (unsigned)gs->dispfb1, (unsigned)gs->dispfb2);

    if (ee->halted) {
        printf("[R939-CHAIN] EE halted: %s\n", ee->halt_reason);
        return 0;
    }

    if (checkpoint_save(ckpt_path) != 0) { fprintf(stderr, "checkpoint_save fail\n"); return 1; }
    printf("[R939-CHAIN] checkpoint saved to %s\n", ckpt_path);
    return 0;
}
