/*
 * Round 775 scratch: extended boot survey that periodically polls real
 * GS display state (gs_get_state(): PMODE/DISPFB1/DISPLAY1/DISPFB2/
 * DISPLAY2) across a long run, to find the first point (if any) where
 * real game code configures circuit 2 - per the user's standing
 * "GS/Display is Display2" reminder and the explicit task #447
 * follow-up request to push toward booting screens past OSDSYS.
 *
 * Also tracks the WaitSema/SignalSema real syscall traffic (via the
 * same public ee_core_get_state() struct - no source-level hooks
 * needed this round, so this compiles directly against the TRACKED
 * tree, not a scratch copy) so a genuine future park is distinguished
 * from continued real progress.
 *
 * Scratch only - never commits BIOS/disc/checkpoint binary content.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/hw/iop_cdvd.h"
#include "core/hw/gs.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> [budget] [chunk]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    uint64_t budget = argc > 3 ? strtoull(argv[3], NULL, 10) : 200000000ull;
    uint64_t chunk  = argc > 4 ? strtoull(argv[4], NULL, 10) : 10000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);

    ee_state_t *ee = ee_core_get_state();
    gs_state_t *gsst = gs_get_state();

    uint64_t done = 0;
    uint32_t last_pc = 0;
    int same_pc_streak = 0;
    uint64_t last_pmode = 0, last_dispfb1 = 0, last_display1 = 0, last_dispfb2 = 0, last_display2 = 0;
    int first_nonzero_reported = 0;

    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;

        uint64_t pmode = gsst->pmode, dispfb1 = gsst->dispfb1, display1 = gsst->display1,
                 dispfb2 = gsst->dispfb2, display2 = gsst->display2;

        int changed = (pmode != last_pmode) || (dispfb1 != last_dispfb1) ||
                      (display1 != last_display1) || (dispfb2 != last_dispfb2) ||
                      (display2 != last_display2);

        if (changed) {
            fprintf(stderr, "[R775-GS-CHANGE] instr=%llu pc=0x%08x PMODE=0x%llx DISPFB1=0x%llx DISPLAY1=0x%llx DISPFB2=0x%llx DISPLAY2=0x%llx\n",
                    (unsigned long long)ee->instructions_executed, ee->pc,
                    (unsigned long long)pmode, (unsigned long long)dispfb1, (unsigned long long)display1,
                    (unsigned long long)dispfb2, (unsigned long long)display2);
            fflush(stderr);
            if ((dispfb2 != 0 || display2 != 0 || pmode != 0) && !first_nonzero_reported) {
                printf("[R775-FIRST-NONZERO-GS] instr=%llu pc=0x%08x\n",
                       (unsigned long long)ee->instructions_executed, ee->pc);
                first_nonzero_reported = 1;
            }
        }
        last_pmode = pmode; last_dispfb1 = dispfb1; last_display1 = display1;
        last_dispfb2 = dispfb2; last_display2 = display2;

        fprintf(stderr, "[R775] instr=%llu pc=0x%08x halted=%u\n",
                (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);
        fflush(stderr);

        if (ee->pc == last_pc) same_pc_streak++; else same_pc_streak = 0;
        last_pc = ee->pc;
        if (same_pc_streak >= 6) {
            fprintf(stderr, "[R775] pc unchanged for 6+ consecutive chunks - stopping early (may be a real busy-loop OR a coincidental periodic revisit - verify with instrumentation before concluding a park)\n");
            break;
        }
    }

    printf("[R775-FINAL] instr=%llu pc=0x%08x halted=%u reason=%s\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
           ee->halt_reason[0] ? ee->halt_reason : "(none)");
    printf("[R775-GS-FINAL] PMODE=0x%016llx DISPFB1=0x%016llx DISPLAY1=0x%016llx DISPFB2=0x%016llx DISPLAY2=0x%016llx\n",
           (unsigned long long)gsst->pmode, (unsigned long long)gsst->dispfb1, (unsigned long long)gsst->display1,
           (unsigned long long)gsst->dispfb2, (unsigned long long)gsst->display2);
    return 0;
}
