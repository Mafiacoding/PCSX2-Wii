/*
 * Round 815 follow-up: scratch-only probe (not part of the tracked
 * repo's mandatory-workflow diff) to pin down the exact IOP halt
 * reason/pc discovered while chaining the r815_handoff_trace driver
 * against the real GT3 disc boot. iop_state_t.halt_reason is already
 * a public field (include/core/iop/iop_core.h) - no new source
 * instrumentation needed, just a small driver that resumes a
 * checkpoint a short distance and prints iop_core_get_state()'s
 * halted/halt_reason/pc directly.
 *
 * Usage: r815_iop_halt_probe <bios> <disc> <ckpt_path> <budget>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <bios> <disc> <ckpt_path> <budget>\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    uint64_t budget = strtoull(argv[4], NULL, 10);

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    fprintf(stderr, "[PROBE] start ee_pc=0x%08x iop_pc=0x%08x iop_halted=%u\n",
            ee->pc, iop->pc, iop->halted);

    const long SLICE = 100000;
    uint64_t done = 0;
    while (done < budget && !ee->halted) {
        uint8_t was_halted = iop->halted;
        system_run_interleaved(SLICE);
        done += (uint64_t)SLICE;
        if (!was_halted && iop->halted) {
            fprintf(stderr, "[PROBE] *** IOP HALTED at done=%llu total_instr=%llu ***\n",
                    (unsigned long long)done, (unsigned long long)ee->instructions_executed);
            fprintf(stderr, "[PROBE] iop_pc=0x%08x reason=\"%s\"\n", iop->pc, iop->halt_reason);
            fprintf(stderr, "[PROBE] iop_ra(gpr31)=0x%08x iop_gpr[29 sp]=0x%08x\n",
                    iop->gpr[31], iop->gpr[29]);
            break;
        }
    }
    fprintf(stderr, "[PROBE] FINAL done=%llu total_instr=%llu ee_pc=0x%08x iop_pc=0x%08x iop_halted=%u reason=\"%s\"\n",
            (unsigned long long)done, (unsigned long long)ee->instructions_executed,
            ee->pc, iop->pc, iop->halted, iop->halt_reason);
    return 0;
}
