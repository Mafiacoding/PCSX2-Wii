/*
 * Round 752 (task #735/#737): resume the GT3 disc-boot checkpoint chain
 * with the new real IOP Breakpoint-exception (ExcCode 9) fix in place,
 * to confirm the IOP - which Round 751 found permanently halted via an
 * unhandled BREAK at pc=0x00119704 - now survives and the EE's SBUS
 * mailbox-flag wait loop (which depends on the IOP staying alive to
 * ever write those registers) can make forward progress again.
 *
 * Important methodology note: `ckpt_gt3_r750.bin` was itself SAVED
 * *after* the IOP had already halted under the OLD (pre-fix) code, so
 * `checkpoint_load()` restores `iop->halted=1` verbatim - the fix
 * cannot retroactively revive a checkpoint that already recorded a
 * halted core, since `iop_core_run()`/`iop_core_step()` both check
 * `halted` first and no-op if set. Re-running the entire multi-billion-
 * instruction chain from a fresh cold boot to reach this exact point
 * again is far outside a single tool call's budget. Instead, this
 * driver directly reproduces the real, exact halted state (same pc,
 * same halt_reason) by clearing just the `halted` flag after load -
 * everything else (cop0, gpr, RAM, all peripheral state) is the real,
 * checkpoint-preserved state the IOP was actually in at the moment it
 * hit the real BREAK - then single-steps to observe whether the fix's
 * real Breakpoint-exception delivery now fires instead of a halt, and
 * continues running to see how far real execution now proceeds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/gs.h"
#include "core/hw/gif.h"
#include "core/hw/iop_cdvd.h"

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <bios> <disc> <ckpt> <total_budget> [slice]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    uint64_t budget = strtoull(argv[4], NULL, 10);
    uint64_t slice = argc > 5 ? strtoull(argv[5], NULL, 10) : 50000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t  *ee  = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();
    gif_state_t *gif = gif_get_state();

    printf("[R752] LOADED (real, checkpoint-preserved state) ee_pc=0x%08X ee_instr=%llu "
           "iop_halted=%d iop_pc=0x%08X iop_halt_reason=\"%s\" iop_cause=0x%08X iop_exception_pending=%d\n",
           ee->pc, (unsigned long long)ee->instructions_executed,
           iop->halted, iop->pc, iop->halted ? iop->halt_reason : "(running)",
           iop->cop0[13], iop->exception_pending);

    if (!iop->halted) {
        printf("[R752] NOTE: IOP was not halted in this checkpoint (unexpected vs Round 751's finding) - proceeding anyway.\n");
    } else {
        /* Reproduce the real moment the BREAK was hit: clear only the
         * halted flag (leave pc/cop0/gpr/RAM exactly as the real
         * checkpoint recorded them - this is the true, unmodified
         * state the IOP was actually in). */
        printf("[R752] Clearing iop->halted to resume the IOP from its real, exact pre-halt state "
               "(pc=0x%08X, everything else untouched) and single-stepping to observe the fix.\n", iop->pc);
        iop->halted = 0;
        uint32_t pre_pc = iop->pc;
        uint32_t pre_cause = iop->cop0[13];
        int genuinely_halted = iop_core_step();
        printf("[R752] After 1 step from the real BREAK instruction: iop_step_returned_halt=%d "
               "iop->halted=%d iop->pc=0x%08X (was 0x%08X) Cause=0x%08X (was 0x%08X) "
               "ExcCode=%u EPC=0x%08X exception_pending=%d\n",
               genuinely_halted, iop->halted, iop->pc, pre_pc, iop->cop0[13], pre_cause,
               (iop->cop0[13] >> 2) & 0x1Fu, iop->cop0[14], iop->exception_pending);
    }

    uint64_t done = 0;
    while (done < budget && !ee->halted) {
        uint64_t step = (budget - done) < slice ? (budget - done) : slice;
        system_run_interleaved(step);
        done += step;
        printf("[R752] +%llu ee_pc=0x%08X ee_instr=%llu iop_halted=%d iop_pc=0x%08X iop_instr=%llu "
               "gif_tri=%llu gif_xg=%llu ncmd=%llu\n",
               (unsigned long long)done, ee->pc, (unsigned long long)ee->instructions_executed,
               iop->halted, iop->pc, (unsigned long long)iop->instructions_executed,
               (unsigned long long)(gif ? gif->triangles_drawn : 0),
               (unsigned long long)(gif ? gif->gif_path1_transfers : 0),
               (unsigned long long)iop_cdvd_get_ncmd_call_count());
        if (iop->halted) {
            printf("[R752] IOP HALTED mid-run: pc=0x%08X reason=\"%s\"\n", iop->pc, iop->halt_reason);
        }
    }

    printf("[R752] DONE ee_halted=%d ee_pc=0x%08X ee_instr=%llu iop_halted=%d iop_pc=0x%08X iop_instr=%llu\n",
           ee->halted, ee->pc, (unsigned long long)ee->instructions_executed,
           iop->halted, iop->pc, (unsigned long long)iop->instructions_executed);

    return 0;
}
