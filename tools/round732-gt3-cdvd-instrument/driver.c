/*
 * Round 732 (task #447, fresh GT3-in-game-code context): per user
 * request "fix 447 we need to keep this going". Extends Round 729's
 * checkpoint-chained GT3 disc-boot survey (chain_driver.c) with the
 * new iop_cdvd_get_ncmd_call_count()/get_scmd_call_count()/
 * get_last_ncmd_issued()/get_last_scmd_issued() diagnostic getters
 * added this round.
 *
 * Purpose: GT3's own real game code produced exactly one wireframe
 * frame + a handful of XGKICKs (Round 729/730/731), then stalled -
 * EE PC cycling in the same 0x0061bbe0-0x0061bbf8 VBLANK-idle range
 * both this survey and the historical diskless-BIOS-boot task #447
 * investigation (Rounds 480-608) converged on. This driver answers a
 * concrete, previously-untested empirical question: during that
 * stall, does GT3's own IOP-side code ever issue a FURTHER CDVD
 * N-command/S-command (e.g. a next-asset ReadCd), or has it stopped
 * calling into this subsystem entirely? Snapshotting the new counters
 * before and after a chained run distinguishes "GT3 is waiting on a
 * disc-command reply that never arrives" (counts increase, no visible
 * progress) from "GT3 isn't even trying to read the disc right now"
 * (counts stay flat - the stall is upstream of CDVD dispatch).
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
    uint64_t budget = argc > 5 ? strtoull(argv[5], NULL, 10) : 100000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }

    if (strcmp(mode, "start") == 0) {
        if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
        if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    } else {
        if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }
    }

    ee_state_t  *ee  = ee_core_get_state();
    gs_state_t  *gs  = gs_get_state();
    gif_state_t *gif = gif_get_state();
    vu1_state_t *vu1 = vu1_get_state();

    uint64_t ncmd_before = iop_cdvd_get_ncmd_call_count();
    uint64_t scmd_before = iop_cdvd_get_scmd_call_count();
    uint8_t  last_ncmd_before = iop_cdvd_get_last_ncmd_issued();
    uint8_t  last_scmd_before = iop_cdvd_get_last_scmd_issued();
    uint64_t xgkick_before = gif ? gif->gif_path1_transfers : 0;

    uint64_t chunk = 10000000ull, done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
    }

    uint64_t ncmd_after = iop_cdvd_get_ncmd_call_count();
    uint64_t scmd_after = iop_cdvd_get_scmd_call_count();
    uint8_t  last_ncmd_after = iop_cdvd_get_last_ncmd_issued();
    uint8_t  last_scmd_after = iop_cdvd_get_last_scmd_issued();
    uint64_t xgkick_after = gif ? gif->gif_path1_transfers : 0;

    int tid = ee_hle_thread_get_current_thread_id();
    printf("[R732-CDVD] ran %llu more, total_instr=%llu pc=0x%08x halted=%u tid=%d "
           "vu1_instr=%llu gif_path1=%llu qw_seen=%llu pmode=0x%02x "
           "dispfb1=0x%08x dispfb2=0x%08x\n",
           (unsigned long long)done, (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, tid,
           (unsigned long long)(vu1 ? vu1->instructions_executed : 0),
           (unsigned long long)(gif ? gif->gif_path1_transfers : 0),
           (unsigned long long)(gif ? gif->quadwords_seen : 0),
           (unsigned)gs->pmode, (unsigned)gs->dispfb1, (unsigned)gs->dispfb2);

    printf("[R732-CDVD] ncmd_calls: before=%llu after=%llu delta=%llu last_issued=0x%02x->0x%02x\n",
           (unsigned long long)ncmd_before, (unsigned long long)ncmd_after,
           (unsigned long long)(ncmd_after - ncmd_before), last_ncmd_before, last_ncmd_after);
    printf("[R732-CDVD] scmd_calls: before=%llu after=%llu delta=%llu last_issued=0x%02x->0x%02x\n",
           (unsigned long long)scmd_before, (unsigned long long)scmd_after,
           (unsigned long long)(scmd_after - scmd_before), last_scmd_before, last_scmd_after);
    printf("[R732-CDVD] xgkick (gif_path1_transfers): before=%llu after=%llu delta=%llu\n",
           (unsigned long long)xgkick_before, (unsigned long long)xgkick_after,
           (unsigned long long)(xgkick_after - xgkick_before));

    if (ee->halted) {
        printf("[R732-CDVD] EE halted: %s\n", ee->halt_reason);
        return 0;
    }

    if (checkpoint_save(ckpt_path) != 0) { fprintf(stderr, "checkpoint_save fail\n"); return 1; }
    printf("[R732-CDVD] checkpoint saved to %s\n", ckpt_path);
    return 0;
}
