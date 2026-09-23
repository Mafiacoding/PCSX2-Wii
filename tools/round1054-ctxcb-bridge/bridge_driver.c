/*
 * Round 1054 (task #447/#536/#1051/#1052/#1053 continuation): SCRATCH
 * experiment driver. Links against /tmp/r1054/iop_hle_intr_r1054.c (a
 * scratch copy of source/hw/iop_hle_intr.c with the real INTRMAN
 * ShouldPreemptCb(0x00115E14)/NewCtxCb(0x00115B90) callback pair - see
 * Round 1053's disassembly/citation - wired into the real interrupt-
 * return path via a genuine synthetic subroutine-call bridge, same
 * "$ra rigged to a private return-gate sentinel" pattern already
 * proven for Alarm/interrupt dispatch in this project's own tracked
 * source). Tracked source is NOT modified by this experiment - this is
 * purely a scratch-tree test of the Round 1053 hypothesis per the
 * standing backup-before-experimenting rule.
 *
 * Resumes the Round 1045 confirmed steady-state checkpoint and runs
 * forward, reporting: how many times the bridge fired, how many times
 * ShouldPreemptCb said "yes", how many times NewCtxCb actually ran to
 * completion, and the full tid 1-9 status table before/after - the
 * direct test of whether this real callback pair, once genuinely
 * invoked, changes anything about the parked IOP threads 4-8.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_hle_thread.h"

extern void r1054_get_bridge_stats(uint32_t *fire_count, uint32_t *true_count, uint32_t *completed_count);

static void dump_threads(const char *label)
{
    printf("[R1054] --- thread table (%s) ---\n", label);
    int cur = iop_hle_thread_get_current_thread_id();
    printf("[R1054] current_thread_id=%d\n", cur);
    for (int t = 1; t <= 9; t++) {
        printf("[R1054] tid=%d status=0x%x wait_type=%d wait_id=%d entry=0x%08x pc=0x%08x\n",
               t, iop_hle_thread_get_status(t), iop_hle_thread_get_wait_type(t),
               iop_hle_thread_get_wait_id(t), iop_hle_thread_get_entry(t), iop_hle_thread_get_pc(t));
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <ckpt_path> [budget]\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(argv[2], &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    uint64_t budget = argc > 3 ? strtoull(argv[3], NULL, 10) : 60000000ull;

    printf("[R1054] loaded checkpoint: ee_pc=0x%08x instr=%llu\n", ee->pc, (unsigned long long)ee->instructions_executed);
    dump_threads("BEFORE");

    uint64_t chunk = 2000000ull, done = 0;
    uint32_t last_ee_pc = ee->pc;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        if (ee->pc != last_ee_pc) {
            printf("[R1054] ee_pc MOVED 0x%08x -> 0x%08x at instr=%llu\n", last_ee_pc, ee->pc, (unsigned long long)ee->instructions_executed);
            last_ee_pc = ee->pc;
        }
    }

    uint32_t fire=0, truec=0, comp=0;
    r1054_get_bridge_stats(&fire, &truec, &comp);
    printf("[R1054] bridge_fire_count=%u shouldpreempt_true_count=%u newctx_completed_count=%u\n", fire, truec, comp);
    printf("[R1054] DONE total_instr=%llu ee_pc=0x%08x halted=%u\n", (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);
    dump_threads("AFTER");
    return 0;
}
