/*
 * Round 1058 (task #1004/#1027/#447/#536 continuation, per user's
 * "finde den cd read request und checke gt3"):
 *
 *   1) "Find the CD read request": this project's real, working CD-read
 *      mechanism is NOT dispatch_ncmd()/dispatch_scmd() (the real MMIO
 *      N-command/S-command register path, confirmed by Round 732 to be
 *      never called by any of GT3/Tekken/KOF/MS3's own game code) - it
 *      is iop_cdvd_disc_find_file()/iso_read_sector() (source/hw/
 *      iop_cdvd.c, source/core/iso_loader.c), the direct FILEIO
 *      fast-path wired in Round 367/554 that cdrom0:/cdrom1: FIO_F_OPEN
 *      goes through. This driver instruments and reports BOTH real
 *      counters side by side to give direct, current, disc-mounted
 *      evidence of which path is actually exercised.
 *   2) "Check GT3": fresh cold boot of GT3 on the CURRENT tree (post
 *      Round-1057 fixes), reporting EE/IOP pc, thread state, CDVD
 *      counters and GS/draw counters every slice, to see whether
 *      anything has changed since the last GT3-specific round (1013:
 *      EE pc=0x8000fde8, IOP pc=0x00155c00).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_hle_thread.h"
#include "core/hw/gs.h"
#include "core/hw/gif.h"
#include "core/hw/vu.h"
#include "core/hw/iop_cdvd.h"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *bios_path = argc > 1 ? argv[1] : "/tmp/r1058/bios.bin";
    const char *disc_path = argc > 2 ? argv[2] : "/tmp/r1058/gt3.iso";
    uint64_t budget = argc > 3 ? strtoull(argv[3], NULL, 10) : 300000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { printf("[FAIL] could not load BIOS %s\n", bios_path); return 1; }
    if (system_init(&bios, &bios) != 0) { printf("[FAIL] system_init failed\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { printf("[FAIL] could not mount disc %s\n", disc_path); return 1; }
    printf("[R1058] real GT3 disc mounted, BIOS=%s, budget=%llu\n", bios_path, (unsigned long long)budget);

    ee_state_t  *ee  = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();
    gs_state_t  *gs  = gs_get_state();
    gif_state_t *gif = gif_get_state();
    vu1_state_t *vu1 = vu1_get_state();
    (void)vu1;

    const uint64_t SLICE = 10000000ull;
    int num_slices = (int)(budget / SLICE);
    if (num_slices < 1) num_slices = 1;

    uint64_t prev_path1 = 0, prev_qw = 0;
    uint64_t prev_ncmd = 0, prev_scmd = 0;

    for (int i = 0; i < num_slices; i++) {
        system_run_interleaved(SLICE);

        uint64_t path1 = gif ? gif->gif_path1_transfers : 0;
        uint64_t qw = gif ? gif->quadwords_seen : 0;
        uint64_t ncmd = iop_cdvd_get_ncmd_call_count();
        uint64_t scmd = iop_cdvd_get_scmd_call_count();
        int ee_tid = ee_hle_thread_get_current_thread_id();

        printf("[slice %2d] ee_instr=%llu ee_pc=0x%08x ee_halted=%u ee_tid=%d "
               "iop_pc=0x%08x iop_tid=%d "
               "ncmd=%llu(+%llu) scmd=%llu(+%llu) last_ncmd=0x%02x "
               "gif_path1=%llu(+%llu) qw=%llu(+%llu) pmode=0x%02x dispfb1=0x%08x dispfb2=0x%08x\n",
               i, (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, ee_tid,
               iop->pc, iop_hle_thread_get_current_thread_id(),
               (unsigned long long)ncmd, (unsigned long long)(ncmd - prev_ncmd),
               (unsigned long long)scmd, (unsigned long long)(scmd - prev_scmd),
               (unsigned)iop_cdvd_get_last_ncommand(),
               (unsigned long long)path1, (unsigned long long)(path1 - prev_path1),
               (unsigned long long)qw, (unsigned long long)(qw - prev_qw),
               (unsigned)gs->pmode, (unsigned)gs->dispfb1, (unsigned)gs->dispfb2);
        fflush(stdout);

        prev_path1 = path1; prev_qw = qw; prev_ncmd = ncmd; prev_scmd = scmd;

        if (ee->halted) {
            printf("[INFO] EE halted: %s\n", ee->halt_reason);
            break;
        }
    }

    printf("[SUMMARY] final ee_instr=%llu ee_pc=0x%08x ee_halted=%u iop_pc=0x%08x "
           "total_ncmd=%llu total_scmd=%llu total_gif_path1=%llu total_qw=%llu\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, iop->pc,
           (unsigned long long)iop_cdvd_get_ncmd_call_count(),
           (unsigned long long)iop_cdvd_get_scmd_call_count(),
           (unsigned long long)(gif ? gif->gif_path1_transfers : 0),
           (unsigned long long)(gif ? gif->quadwords_seen : 0));

    /* IOP thread table dump (status/entry/wait_type for tids 1-9) */
    printf("[IOP-THREADS]\n");
    for (int t = 1; t <= 9; t++) {
        uint32_t status = iop_hle_thread_get_status(t);
        if (status == 0) continue;
        printf("  tid=%d status=0x%x entry=0x%08x wait_type=%d\n",
               t, status, iop_hle_thread_get_entry(t), iop_hle_thread_get_wait_type(t));
    }

    return 0;
}
