/* Round 942 (task #929/930, GT3 fresh-boot investigation): the fresh
 * GT3 checkpoint chain (chain_driver.c, start mode) hit a hard freeze
 * at EE pc=0x80000200 (the real EE interrupt vector), instr=38,865,330
 * - identical to what the user's own real Dolphin test showed for the
 * diskless BIOS case. Two independent chain re-runs (100M then 50M
 * more slices) produced byte-for-byte identical instr counts, meaning
 * zero net progress. This driver loads that exact checkpoint and
 * single-steps the EE core directly, printing pc/instructions_executed
 * every step, to determine whether the EE genuinely never advances at
 * all (a true spin/storm) or advances-and-returns (a periodic
 * re-entry loop invisible at coarse 10M-slice granularity). */
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
    if (argc < 3) { fprintf(stderr, "usage: %s <bios_path> <ckpt_path>\n", argv[0]); return 1; }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(argv[2], &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    printf("[R942] initial: ee_pc=0x%08x ee_instr=%llu ee_cop0_cause=0x%08x ee_cop0_status=0x%08x iop_pc=0x%08x ee_idle=%d\n",
           ee->pc, (unsigned long long)ee->instructions_executed, ee->cop0[13], ee->cop0[12], iop->pc, ee->idle);

    uint32_t last_pc = ee->pc;
    uint64_t last_instr = ee->instructions_executed;
    for (int i = 0; i < 2000; i++) {
        ee_core_step();
        if (ee->pc != last_pc || i < 40 || i % 100 == 0) {
            printf("[R942] step=%d pc=0x%08x instr=%llu cause=0x%08x status=0x%08x halted=%d\n",
                   i, ee->pc, (unsigned long long)ee->instructions_executed, ee->cop0[13], ee->cop0[12], ee->halted);
        }
        last_pc = ee->pc;
        last_instr = ee->instructions_executed;
        if (ee->halted) { printf("[R942] EE halted: %s\n", ee->halt_reason); break; }
    }
    printf("[R942] final after 2000 steps: pc=0x%08x instr=%llu (delta=%llu)\n",
           ee->pc, (unsigned long long)ee->instructions_executed,
           (unsigned long long)(ee->instructions_executed - last_instr));
    return 0;
}
