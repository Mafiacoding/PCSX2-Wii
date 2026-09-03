/*
 * Round 816 (task #811/#820 continuation, per the user's own explicit
 * next-step instruction after Round 815): the real BIOS-to-GT3
 * handoff is now confirmed clean (Round 815: handoff_pc=0x80002fbc,
 * target_pc=0x01000008, discname="SCES_502.94;1"). The user's request
 * this round is an EXHAUSTIVE CONTINUOUS trace from that real entry
 * point, watching specifically for GT3's first game-side CDVD RPC
 * attempt, while explicitly avoiding the checkpoint save/load
 * fidelity artifact Round 815 flagged around ~24M instructions.
 *
 * Cold boot only ("start" mode) - NO checkpoint save/load anywhere in
 * this driver's own path, satisfying the user's "run continuously
 * through that region" instruction directly (as opposed to "take
 * checkpoints only at points proven safe by a save/restore
 * equivalence test", which was not attempted this round - continuous
 * is simpler and sufficient given this sandbox's per-call throughput).
 *
 * Linked against ee_core.c compiled with -DR815_HANDOFF_TRACE (reused
 * from last round, for HANDOFF/BLOCK/SYSCALL/FIRST-CDVD-BIND
 * observability) and -DR813_CDVDTRACE (reused from Round 813, now
 * widened this round to also log $ra/tid - see ee_core.c's own Round
 * 816 comments at the two SIF_SID_CDVD_NCMD/SCMD branches) for the
 * actual EE-issued CDVD RPC *call* trace, which is the more direct
 * "game-side CDVD import" signal the user asked for (bind is the
 * setup step; call is the actual sceCdXxx()-equivalent invocation).
 * Deliberately NOT linked with -DR812_EVENTLOG this round: that
 * logger's per-event stderr volume, while fine for Round 815's
 * ~19.5M-instruction sample, would be unmanageably large over the
 * ~1B+ raw-instruction continuous window this round targets - the
 * R815_HANDOFF_TRACE code's own call to
 * ee_hle_thread_eventlog_set_enabled(1) at handoff remains harmless
 * either way, since R812_EVENTLOG's own EVT() macro compiles to
 * nothing when its guarding -D isn't defined at compile time.
 *
 * Usage: r816_continuous_cdvd <bios_path> <disc_path> [budget]
 * (budget is in the same driver-internal "slice tick" units as every
 * other r81N tool in this directory - system_run_interleaved(SLICE)
 * with SLICE=1,000,000 per loop; raw total_instr empirically grows at
 * roughly 8x the budget-unit count, per Round 815's own calibration.)
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
    uint64_t budget = argc > 3 ? strtoull(argv[3], NULL, 10) : 150000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12 /* CDVD_TYPE_PS2CD */);

    ee_state_t *ee = ee_core_get_state();
    fprintf(stderr, "[R816-TRACE] cold-boot start (no checkpoint anywhere) budget=%llu\n",
            (unsigned long long)budget);

    const long SLICE = 1000000;
    uint64_t done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(SLICE);
        done += (uint64_t)SLICE;
        if ((done / (uint64_t)SLICE) % 20 == 0) {
            fprintf(stderr, "[R816-TRACE] progress done=%llu total_instr=%llu pc=0x%08x ncmd=%llu scmd=%llu\n",
                    (unsigned long long)done, (unsigned long long)ee->instructions_executed,
                    ee->pc, (unsigned long long)iop_cdvd_get_ncmd_call_count(),
                    (unsigned long long)iop_cdvd_get_scmd_call_count());
        }
    }

    fprintf(stderr, "[R816-TRACE] FINAL total_instr=%llu pc=0x%08x halted=%u ncmd=%llu scmd=%llu\n",
            (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
            (unsigned long long)iop_cdvd_get_ncmd_call_count(),
            (unsigned long long)iop_cdvd_get_scmd_call_count());
    if (ee->halted) {
        fprintf(stderr, "[R816-TRACE] EE halted: %s\n", ee->halt_reason);
    }
    return 0;
}
