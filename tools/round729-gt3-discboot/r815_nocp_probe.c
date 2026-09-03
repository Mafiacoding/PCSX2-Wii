/*
 * Round 815 follow-up #3: CRITICAL control test. r815_iop_halt_probe
 * (via a checkpoint_load()'d resume) found the IOP halting
 * ("PC escaped to unfetchable addr 0x3C04BF80") at ~24.8M total
 * instructions on every GT3 cold boot - but Round 814's own fresh
 * cold-boot run (no checkpoint round-trip at all, single continuous
 * process) reached 320M instructions cleanly with no such halt
 * reported. This driver tests whether checkpoint save/load fidelity
 * (not organic emulation behavior) is the actual cause: identical
 * cold boot (system_init + iop_cdvd_mount_iso), but staying in ONE
 * continuous process the whole way past 25M instructions, with NO
 * checkpoint_save()/checkpoint_load() round-trip at all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_cdvd.h"
#include "core/ee/ee_hle_thread.h"

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s <bios> <disc> <budget>\n", argv[0]); return 1; }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(argv[2]) != 0) { fprintf(stderr, "mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);
    ee_hle_thread_eventlog_set_enabled(0);
    fprintf(stderr, "[DIAG] eventlog disabled call made\n");

    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();
    uint64_t budget = strtoull(argv[3], NULL, 10);

    const long SLICE = 100000;
    uint64_t done = 0;
    while (done < budget && !ee->halted) {
        uint8_t was_halted = iop->halted;
        system_run_interleaved(SLICE);
        done += (uint64_t)SLICE;
        if (!was_halted && iop->halted) {
            fprintf(stderr, "[NOCPPROBE] *** IOP HALTED at done=%llu total_instr=%llu ***\n",
                    (unsigned long long)done, (unsigned long long)ee->instructions_executed);
            fprintf(stderr, "[NOCPPROBE] iop_pc=0x%08x reason=\"%s\"\n", iop->pc, iop->halt_reason);
            break;
        }
        if (done % 5000000 == 0) {
            fprintf(stderr, "[NOCPPROBE] progress done=%llu iop_pc=0x%08x iop_halted=%u scmd=%llu\n",
                    (unsigned long long)done, iop->pc, iop->halted,
                    (unsigned long long)iop_cdvd_get_scmd_call_count());
        }
    }
    fprintf(stderr, "[NOCPPROBE] FINAL done=%llu ee_pc=0x%08x iop_pc=0x%08x iop_halted=%u reason=\"%s\"\n",
            (unsigned long long)done, ee->pc, iop->pc, iop->halted, iop->halt_reason);
    return 0;
}
