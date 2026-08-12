/*
 * Round 577 (task #551) Tekken disc-boot XGKICK survey driver.
 * Host-native only, not part of the Wii build (see tools/README
 * convention - every roundNNN-* driver here is a scratch test
 * harness for the emulator core, compiled standalone).
 *
 * Direct follow-up to Round 576's real MSCNT fix (VIF1 MSCNT now
 * resumes VU1's microprogram from its current TPC instead of
 * restarting it from IMM, matching the user-supplied ps2sdk
 * packet2_utils.h ground truth). Round 574 found xgkick_calls=0
 * across the entire diskless JP-BIOS boot window despite VU1
 * executing 66,376 real instructions there; this driver re-runs the
 * equivalent survey on the *disc-boot* path (real Tekken Tag
 * Tournament (Demo) game code, not just the BIOS animation) to check
 * whether the MSCNT fix lets VU1 reach a real XGKICK when running the
 * game's own microprogram(s).
 *
 * Uses the exact same disc-mount + BIOS-boot setup precedent as
 * Round 567's driver (iop_cdvd_mount_iso() with the reconstructed
 * /tmp/round238_diag/disc.iso fixture, JP BIOS at
 * /tmp/round238_diag/bios.bin) and the same halt-reason/register-dump
 * technique. Round 567/568/569 all found the disc-boot path parks at
 * a real WaitSema(semid=0) at pc=0x00400324, instr=41868665 - this
 * driver runs well past that point (per the "even if it crashes i
 * dont care atleast it boots" tolerance) to see whether VU1/GIF
 * activity happens before or after that park.
 */
#include <stdio.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/hw/gs.h"
#include "core/hw/gif.h"
#include "core/hw/vu.h"
#include "core/hw/iop_cdvd.h"

int main(void)
{
    bios_image_t bios;
    if (bios_load("/tmp/round238_diag/bios.bin", &bios) != 0) {
        printf("[FAIL] could not load BIOS\n");
        return 1;
    }
    if (system_init(&bios, &bios) != 0) {
        printf("[FAIL] system_init failed\n");
        return 1;
    }
    if (iop_cdvd_mount_iso("/tmp/round238_diag/disc.iso") != 0) {
        printf("[FAIL] could not mount disc.iso\n");
        return 1;
    }
    printf("[INFO] disc mounted, BIOS loaded, starting survey\n");

    ee_state_t  *ee  = ee_core_get_state();
    gs_state_t  *gs  = gs_get_state();
    gif_state_t *gif = gif_get_state();
    vu1_state_t *vu1 = vu1_get_state();

    /* Round 567 found the terminal WaitSema(0) park at instr=41868665.
     * Survey in slices, reporting VU1/GIF activity at each checkpoint,
     * well past that point to see if anything happens either side of
     * it. 20 slices of 10M = 200M instructions total. */
    const uint64_t SLICE = 10000000ull;
    const int NUM_SLICES = 45;

    uint64_t prev_vu1_instr = 0;
    uint64_t prev_path1 = 0;

    for (int i = 0; i < NUM_SLICES; i++) {
        system_run_interleaved(SLICE);

        uint64_t vu1_instr = vu1->instructions_executed;
        uint64_t path1 = gif->gif_path1_transfers;

        printf("[slice %2d] instr=%llu pc=0x%08x halted=%u vu1_instr=%llu(+%llu) vu1_tpc=0x%04x "
               "gif_path1_transfers=%llu(+%llu) pmode=0x%02x dispfb2=0x%08x\n",
               i, (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
               (unsigned long long)vu1_instr, (unsigned long long)(vu1_instr - prev_vu1_instr),
               vu1->tpc,
               (unsigned long long)path1, (unsigned long long)(path1 - prev_path1),
               (unsigned)gs->pmode, (unsigned)gs->dispfb2);
        fflush(stdout);

        prev_vu1_instr = vu1_instr;
        prev_path1 = path1;

        if (ee->halted) {
            printf("[INFO] EE halted: %s\n", ee->halt_reason);
            break;
        }
    }

    printf("[SUMMARY] final instr=%llu pc=0x%08x total_vu1_instr=%llu total_gif_path1_transfers=%llu\n",
           (unsigned long long)ee->instructions_executed, ee->pc,
           (unsigned long long)vu1->instructions_executed,
           (unsigned long long)gif->gif_path1_transfers);

    return 0;
}
