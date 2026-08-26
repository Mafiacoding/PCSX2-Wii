/*
 * Round 729 (task #536/#447 continuation), directly per the user's
 * request: "should i just upload gran turismo here and you try
 * everything with our emulator and not pcsx2". User uploaded the real
 * "Gran Turismo 3 - A-Spec (Europe, Australia) (En,Fr,De,Es,It) (v1.00)"
 * ISO (3.5GB, verified real ISO9660 PVD "CD001" at LBA16 despite `file`
 * reporting UDF as the primary descriptor - PS2 discs are commonly
 * hybrid ISO9660/UDF, and this project's own iso_loader.c (Round 139/
 * 170/367) only needs the ISO9660 side, exactly like real cdrom0:
 * FILEIO access does).
 *
 * This is this project's OWN tracked disc-boot path - the same one
 * Round 577 used for Tekken Tag Tournament (Demo), extended to a much
 * larger, more complex real commercial title. Mirrors Round 577's
 * structure exactly (mount real disc -> load real BIOS -> system_init
 * -> system_run_interleaved in slices -> report EE/VU1/GIF/GS state
 * each slice) so results are directly comparable to that prior
 * baseline.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/hw/gs.h"
#include "core/hw/gif.h"
#include "core/hw/vu.h"
#include "core/hw/iop_cdvd.h"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *bios_path = argc > 1 ? argv[1] : "/tmp/round729_gt3/bios.bin";
    const char *disc_path = argc > 2 ? argv[2] : "/tmp/round729_gt3/disc.iso";
    uint64_t budget = argc > 3 ? strtoull(argv[3], NULL, 10) : 200000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { printf("[FAIL] could not load BIOS %s\n", bios_path); return 1; }
    if (system_init(&bios, &bios) != 0) { printf("[FAIL] system_init failed\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { printf("[FAIL] could not mount disc %s\n", disc_path); return 1; }
    printf("[R729] real Gran Turismo 3 disc mounted, BIOS=%s, budget=%llu\n", bios_path, (unsigned long long)budget);

    ee_state_t  *ee  = ee_core_get_state();
    gs_state_t  *gs  = gs_get_state();
    gif_state_t *gif = gif_get_state();
    vu1_state_t *vu1 = vu1_get_state();

    const uint64_t SLICE = 10000000ull;
    int num_slices = (int)(budget / SLICE);
    if (num_slices < 1) num_slices = 1;

    uint64_t prev_vu1_instr = 0;
    uint64_t prev_path1 = 0;
    uint64_t prev_qw = 0;

    for (int i = 0; i < num_slices; i++) {
        system_run_interleaved(SLICE);

        uint64_t vu1_instr = vu1 ? vu1->instructions_executed : 0;
        uint64_t path1 = gif ? gif->gif_path1_transfers : 0;
        uint64_t qw = gif ? gif->quadwords_seen : 0;
        int tid = ee_hle_thread_get_current_thread_id();

        printf("[slice %2d] instr=%llu pc=0x%08x halted=%u tid=%d vu1_instr=%llu(+%llu) vu1_tpc=0x%04x "
               "gif_path1=%llu(+%llu) qw_seen=%llu(+%llu) pmode=0x%02x dispfb1=0x%08x dispfb2=0x%08x\n",
               i, (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, tid,
               (unsigned long long)vu1_instr, (unsigned long long)(vu1_instr - prev_vu1_instr),
               vu1 ? vu1->tpc : 0,
               (unsigned long long)path1, (unsigned long long)(path1 - prev_path1),
               (unsigned long long)qw, (unsigned long long)(qw - prev_qw),
               (unsigned)gs->pmode, (unsigned)gs->dispfb1, (unsigned)gs->dispfb2);
        fflush(stdout);

        prev_vu1_instr = vu1_instr;
        prev_path1 = path1;
        prev_qw = qw;

        if (ee->halted) {
            printf("[INFO] EE halted: %s\n", ee->halt_reason);
            break;
        }
    }

    printf("[SUMMARY] final instr=%llu pc=0x%08x halted=%u total_vu1_instr=%llu total_gif_path1=%llu total_qw=%llu\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
           (unsigned long long)(vu1 ? vu1->instructions_executed : 0),
           (unsigned long long)(gif ? gif->gif_path1_transfers : 0),
           (unsigned long long)(gif ? gif->quadwords_seen : 0));

    return 0;
}
