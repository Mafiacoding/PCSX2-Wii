/*
 * Round 717 (task #700-703), continuing directly from Round 716's finding
 * that threads 6/7/9 wake correctly every VBLANK cycle: this driver asks
 * the next question - what do those threads actually DO once running?
 * Samples ee_hle_thread_get_current_thread_id() + ee_core_get_state()->pc
 * every 20,000 EE instructions (fine enough to catch a thread's brief
 * active window between a wake and its next SleepThread), plus GS PMODE/
 * DISPFB1/DISPFB2 and gif_get_state()->quadwords_seen (a real proxy for
 * "did any GS-relevant data actually move" - unaffected either way by
 * whether the boot ever reaches real triangle/sprite drawing).
 *
 * Purely diagnostic - no tracked-source changes needed, uses only
 * already-public accessors (ee_hle_thread_get_current_thread_id(),
 * ee_core_get_state(), gs_get_state(), gif_get_state()).
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

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *bios_path = argc > 1 ? argv[1] : "/tmp/r708/bios.bin";
    uint64_t budget = argc > 2 ? strtoull(argv[2], NULL, 10) : 100000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { printf("[FAIL] bios load\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { printf("[FAIL] system_init\n"); return 1; }
    printf("[R717] diskless boot, BIOS=%s, budget=%llu\n", bios_path, (unsigned long long)budget);

    uint64_t chunk = 20000ull, done = 0;
    int last_tid = -99;
    uint64_t last_qw = 0;
    uint64_t last_pmode = 0, last_dispfb1 = 0, last_dispfb2 = 0;

    while (done < budget) {
        system_run_interleaved(chunk);
        done += chunk;

        int tid = ee_hle_thread_get_current_thread_id();
        ee_state_t *st = ee_core_get_state();
        uint32_t pc = st ? st->pc : 0;
        gif_state_t *gif = gif_get_state();
        uint64_t qw = gif ? gif->quadwords_seen : 0;
        gs_state_t *gs = gs_get_state();
        uint64_t pmode = gs ? gs->pmode : 0;
        uint64_t dispfb1 = gs ? gs->dispfb1 : 0;
        uint64_t dispfb2 = gs ? gs->dispfb2 : 0;

        int changed = (tid != last_tid) || (qw != last_qw) ||
                      (pmode != last_pmode) || (dispfb1 != last_dispfb1) || (dispfb2 != last_dispfb2);
        if (changed) {
            printf("[R717] instr=%llu tid=%d pc=0x%08X qw_seen=%llu pmode=0x%llx dispfb1=0x%llx dispfb2=0x%llx\n",
                   (unsigned long long)done, tid, pc, (unsigned long long)qw,
                   (unsigned long long)pmode, (unsigned long long)dispfb1, (unsigned long long)dispfb2);
            last_tid = tid; last_qw = qw;
            last_pmode = pmode; last_dispfb1 = dispfb1; last_dispfb2 = dispfb2;
        }
    }

    printf("[R717] FINAL after %llu instructions: tid=%d qw_seen=%llu\n",
           (unsigned long long)done, ee_hle_thread_get_current_thread_id(),
           (unsigned long long)(gif_get_state() ? gif_get_state()->quadwords_seen : 0));
    return 0;
}
