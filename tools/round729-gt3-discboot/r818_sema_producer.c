/*
 * Round 818 (task #823/#824/#825, per the user's own next-step
 * instruction after Round 817's sema-0/sema-5 correction): Round 817
 * established that semaphore 0 (not 5) is GT3's real, cleanly-testable
 * loader-adjacent block (thread 2, saved pc=0x0101bc24), and that
 * releasing it is a clean negative - the woken thread runs real code
 * but never reaches a CDVD import. The next step the user specified is
 * to trace semaphore 0's real CreateSema call site, its expected
 * producer thread/function, and why that producer never signals it,
 * following the exact chain: CreateSema(initial_count) -> semid=0 ->
 * WaitSema(0) -> expected producer -> SignalSema(0) -> GT3 loader
 * proceeds -> first CDVD import.
 *
 * Cold-boot-only (no checkpoint save/load anywhere in this driver's
 * own path), same pattern as r816/r817's own drivers. Linked against
 * ee_hle_thread.c compiled with -DR818_SEMA_TRACE (this round's new,
 * independent-of-R812_EVENTLOG instrumentation - logs ONLY CreateSema
 * and SignalSema/iSignalSema calls with their semid/caller-$ra/pc, and
 * deliberately never logs WaitSema, so the log stays bounded even once
 * a thread starts busy-parking - unlike R812_EVENTLOG, which would log
 * a WaitSema-entry line on every single scheduler tick once a thread
 * blocks, producing unbounded output over a multi-ten-million-
 * instruction window).
 *
 * Usage: r818_sema_producer <bios_path> <disc_path> [budget]
 * (budget in the same driver-internal "slice tick" units as every
 * other r81N tool - system_run_interleaved(SLICE) with SLICE=1,000,000
 * per loop; raw total_instr grows at roughly 7-8x the budget-unit
 * count per Round 815/816's own calibration.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/hw/iop_cdvd.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> [budget]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    uint64_t budget = argc > 3 ? strtoull(argv[3], NULL, 10) : 8000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12 /* CDVD_TYPE_PS2CD */);

    ee_state_t *ee = ee_core_get_state();
    fprintf(stderr, "[R818-TRACE] cold-boot start (no checkpoint anywhere) budget=%llu\n",
            (unsigned long long)budget);

    const long SLICE = 1000000;
    uint64_t done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(SLICE);
        done += (uint64_t)SLICE;
        if ((done / (uint64_t)SLICE) % 10 == 0) {
            fprintf(stderr, "[R818-TRACE] progress done=%llu total_instr=%llu pc=0x%08x ncmd=%llu scmd=%llu\n",
                    (unsigned long long)done, (unsigned long long)ee->instructions_executed,
                    ee->pc, (unsigned long long)iop_cdvd_get_ncmd_call_count(),
                    (unsigned long long)iop_cdvd_get_scmd_call_count());
        }
    }

    fprintf(stderr, "[R818-TRACE] FINAL done=%llu total_instr=%llu pc=0x%08x halted=%d ncmd=%llu scmd=%llu\n",
            (unsigned long long)done, (unsigned long long)ee->instructions_executed,
            ee->pc, ee->halted,
            (unsigned long long)iop_cdvd_get_ncmd_call_count(),
            (unsigned long long)iop_cdvd_get_scmd_call_count());

    /* Dump full thread table at the end, same convention as
     * r817_sem5_probe.c, so we can cross-reference which tid ends up
     * parked on which semid at this point in the run. */
    int count = ee_hle_thread_get_thread_count();
    for (int t = 1; t <= count; t++) {
        fprintf(stderr, "[R818-TRACE] final tid=%d status=0x%x wait_type=%u wait_id=%u saved_pc=0x%08x prio=%u\n",
                t, ee_hle_thread_get_status(t), ee_hle_thread_get_wait_type(t),
                ee_hle_thread_get_wait_id(t), ee_hle_thread_get_saved_pc(t), ee_hle_thread_get_priority(t));
    }

    return 0;
}
