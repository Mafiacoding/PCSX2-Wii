/*
 * Round 729 checkpoint-chained variant - mirrors this project's own
 * established Round 715/382/383 checkpoint-chaining pattern (see
 * include/core/checkpoint.h's module comment for the full citation
 * trail), needed here because the plain driver.c's first run already
 * showed real Gran Turismo 3 game code running past 959,994,964
 * instructions without halting - far beyond a single sandbox
 * tool-call's wall-clock budget can cover in one shot. Each
 * invocation runs a bounded instruction budget then saves a
 * checkpoint (never committed/rsynced - contains BIOS/disc-derived
 * RAM content per checkpoint.h's leak-prevention note), so the next
 * invocation can pick up exactly where the last one left off.
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
    uint64_t budget = argc > 5 ? strtoull(argv[5], NULL, 10) : 500000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }

    if (strcmp(mode, "start") == 0) {
        if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
        if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
        /* Round 750 (task #730) fix: iop_cdvd_mount_iso() only sets
         * g_disc_mounted - it does NOT set the CDVD disc-TYPE register,
         * so iop_cdvd_get_disc_type() kept reading back IOP_CDVD_TYPE_
         * NODISC for this driver's entire run, exactly the same class of
         * bug main.c's own Round 610 fix already documents ("a disc-
         * mounted boot trace and a genuinely diskless boot trace were
         * found to be byte-identical because iop_cdvd_get_disc_type()
         * read back NODISC in both cases"). That silently fooled the
         * "diskless-only" guards in ee_check_browser_idle_carousel() and
         * ee_check_browser_menu_escalation_heuristic() (both gate on
         * iop_cdvd_get_disc_type() != IOP_CDVD_TYPE_NODISC) into firing
         * on this genuinely disc-mounted GT3 boot, raw-dereferencing
         * KUSEG addresses 0x1C0444/0x1C0450/0x1C0454 that real disc-boot
         * code never establishes TLB coverage for (Round 607: real
         * disc-auto-boot uses the EELOAD chain, never the Browser
         * struct) - producing the permanent TLB Refill exception loop
         * root-caused this round. Mirrors main.c's own already-correct
         * mount+set_disc_present pairing (source/main.c line ~451). */
        iop_cdvd_set_disc_present(0x12 /* CDVD_TYPE_PS2CD, Round 170's cited constant */);
    } else {
        if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }
    }

    ee_state_t  *ee  = ee_core_get_state();
    gs_state_t  *gs  = gs_get_state();
    gif_state_t *gif = gif_get_state();
    vu1_state_t *vu1 = vu1_get_state();

    uint64_t chunk = 10000000ull, done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
    }

    int tid = ee_hle_thread_get_current_thread_id();
    printf("[R729-CHAIN] ran %llu more, total_instr=%llu pc=0x%08x halted=%u tid=%d "
           "vu1_instr=%llu vu1_tpc=0x%04x gif_path1=%llu qw_seen=%llu pmode=0x%02x "
           "dispfb1=0x%08x dispfb2=0x%08x\n",
           (unsigned long long)done, (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, tid,
           (unsigned long long)(vu1 ? vu1->instructions_executed : 0), vu1 ? vu1->tpc : 0,
           (unsigned long long)(gif ? gif->gif_path1_transfers : 0),
           (unsigned long long)(gif ? gif->quadwords_seen : 0),
           (unsigned)gs->pmode, (unsigned)gs->dispfb1, (unsigned)gs->dispfb2);

    if (ee->halted) {
        printf("[R729-CHAIN] EE halted: %s\n", ee->halt_reason);
        return 0;
    }

    if (checkpoint_save(ckpt_path) != 0) { fprintf(stderr, "checkpoint_save fail\n"); return 1; }
    printf("[R729-CHAIN] checkpoint saved to %s\n", ckpt_path);
    return 0;
}
