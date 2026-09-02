/*
 * Round 775 checkpoint-chained survey driver - generalized version of
 * this project's established Round 715/382/383/729 checkpoint-
 * chaining pattern (see include/core/checkpoint.h's citation trail),
 * used here to accumulate multi-billion-instruction boot budgets for
 * Tekken Tag Tournament (full), Klonoa 2, and King of Fighters
 * 2000-2001 across many sandbox tool-call invocations (each capped at
 * ~175s wall-clock, well under what's needed for deep real disc-boot
 * crt0/kernel-init sequences). Each invocation runs a bounded
 * instruction budget then saves a checkpoint (never committed/rsynced
 * - contains BIOS/disc-derived RAM content per checkpoint.h's
 * leak-prevention note), so the next invocation resumes exactly where
 * the last left off. Prints full real GS display state (PMODE/
 * DISPFB1/DISPLAY1/DISPFB2/DISPLAY2) every invocation per the user's
 * standing "GS/Display is Display2" reminder and task #447's
 * "reach booting screens past OSDSYS" goal.
 *
 * Round 779 units-clarity fix (task #796): the `budget`/`chunk` values
 * below are passed straight to system_run_interleaved(), whose
 * parameter is a *slice* count, not a raw EE instruction count - each
 * slice executes up to EE_IOP_STEP_RATIO=8 real EE instructions plus 1
 * IOP instruction (see source/core/system.c's own citation on the real
 * ~8:1 EE:IOP clock ratio - deliberate and correct, already documented
 * there; it was never a bug). This driver's own printed "ran %llu more"
 * previously just echoed the requested slice count (`done`) labeled as
 * if it were the real EE instruction delta, which is why "budget=
 * 50000000" was consistently observed (STATUS.md, Rounds 777-778) to
 * actually advance ee->instructions_executed by ~400,000,000 - an
 * exact 8x match, not a mystery overshoot. No emulator-core bug exists
 * here. Fixed *only* in this diagnostic tool: now samples
 * ee->instructions_executed before/after and prints the real delta
 * alongside the requested slice count, so future rounds can size chain
 * calls by real EE-instruction target instead of an unlabeled 8x fudge
 * factor.
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
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path> <start|continue> [budget]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    const char *mode = argv[4];
    uint64_t budget = argc > 5 ? strtoull(argv[5], NULL, 10) : 1500000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }

    if (strcmp(mode, "start") == 0) {
        if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
        if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
        iop_cdvd_set_disc_present(0x12);
    } else {
        if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }
    }

    ee_state_t  *ee  = ee_core_get_state();
    gs_state_t  *gs  = gs_get_state();
    gif_state_t *gif = gif_get_state();
    vu1_state_t *vu1 = vu1_get_state();

    uint64_t chunk = 10000000ull, done = 0;
    uint64_t ee_instr_before = ee->instructions_executed;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
    }
    uint64_t ee_instr_delta = ee->instructions_executed - ee_instr_before;

    int tid = ee_hle_thread_get_current_thread_id();
    printf("[R775-CHAIN] ran %llu slice(s) (real EE delta=%llu, ~%.1fx), total_instr=%llu pc=0x%08x halted=%u tid=%d "
           "vu1_instr=%llu vu1_tpc=0x%04x gif_path1=%llu qw_seen=%llu\n",
           (unsigned long long)done, (unsigned long long)ee_instr_delta,
           done ? (double)ee_instr_delta / (double)done : 0.0,
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, tid,
           (unsigned long long)(vu1 ? vu1->instructions_executed : 0), vu1 ? vu1->tpc : 0,
           (unsigned long long)(gif ? gif->gif_path1_transfers : 0),
           (unsigned long long)(gif ? gif->quadwords_seen : 0));
    printf("[R775-CHAIN-GS] PMODE=0x%016llx DISPFB1=0x%016llx DISPLAY1=0x%016llx DISPFB2=0x%016llx DISPLAY2=0x%016llx\n",
           (unsigned long long)gs->pmode, (unsigned long long)gs->dispfb1, (unsigned long long)gs->display1,
           (unsigned long long)gs->dispfb2, (unsigned long long)gs->display2);

    if (ee->halted) {
        printf("[R775-CHAIN] EE halted: %s\n", ee->halt_reason);
        return 0;
    }

    if (checkpoint_save(ckpt_path) != 0) { fprintf(stderr, "checkpoint_save fail\n"); return 1; }
    printf("[R775-CHAIN] checkpoint saved to %s\n", ckpt_path);
    return 0;
}
