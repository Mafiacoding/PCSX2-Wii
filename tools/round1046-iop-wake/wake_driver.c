/*
 * Round 1046 (task #447/#536/#1009 continuation; user's explicit
 * "versuche sie aufzuwecken" request): decisive-experiment driver,
 * modeled exactly on Round 1037's r1037_signal_loop2.c methodology -
 * poll real thread status every chunk, and only when a target IOP
 * thread is GENUINELY parked in TSW_SLEEP (status==IOP_THS_WAIT &&
 * wait_type==IOP_TSW_SLEEP), force it through the same transition the
 * real WakeupThread() handler performs (via the scratch-only
 * iop_hle_thread_debug_wakeup() added to this driver's private copy
 * of iop_hle_thread.c - never touches the tracked source).
 *
 * Resumes from Round 1045's confirmed 1.74B-instruction steady-state
 * checkpoint (ee_pc=0x0020eee8, R1036REG count=135) to avoid re-paying
 * the cost of getting back to the wall.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_hle_thread.h"

#define N_TARGETS 5
static const int targets[N_TARGETS] = {4,5,6,7,8};

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <ckpt_path> [budget]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *ckpt_path = argv[2];
    uint64_t budget = argc > 3 ? strtoull(argv[3], NULL, 10) : 60000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    uint32_t start_ee_pc = ee->pc;
    uint64_t start_instr = ee->instructions_executed;

    printf("[R1046-WAKE] loaded checkpoint: ee_pc=0x%08x instr=%llu\n", ee->pc, (unsigned long long)ee->instructions_executed);
    {
        int cur = iop_hle_thread_get_current_thread_id();
        printf("[R1046-WAKE] current_thread_id=%d status=0x%x priority=%u entry=0x%08x pc=0x%08x\n",
               cur, iop_hle_thread_get_status(cur), iop_hle_thread_get_priority(cur),
               iop_hle_thread_get_entry(cur), iop_hle_thread_get_pc(cur));
    }
    for (int i = 0; i < N_TARGETS; i++) {
        int t = targets[i];
        printf("[R1046-WAKE] pre  tid=%d status=0x%x wait_type=%d wait_id=%d entry=0x%08x pc=0x%08x\n",
               t, iop_hle_thread_get_status(t), iop_hle_thread_get_wait_type(t), iop_hle_thread_get_wait_id(t),
               iop_hle_thread_get_entry(t), iop_hle_thread_get_pc(t));
    }

    uint64_t chunk = 2000000ull, done = 0;
    uint32_t wake_count = 0;
    uint32_t last_ee_pc = ee->pc;
    uint64_t last_move_at = 0;

    while (done < budget && !ee->halted) {
        /* force-wake pass: only threads GENUINELY TSW_SLEEP-parked right now */
        for (int i = 0; i < N_TARGETS; i++) {
            int t = targets[i];
            uint32_t st = iop_hle_thread_get_status(t);
            int wt = iop_hle_thread_get_wait_type(t);
            if (st == IOP_THS_WAIT && wt == IOP_TSW_SLEEP) {
                if (iop_hle_thread_debug_wakeup(t)) {
                    wake_count++;
                    printf("[R1046-WAKE] FORCED wakeup tid=%d at instr=%llu\n", t, (unsigned long long)ee->instructions_executed);
                }
            }
        }
        system_run_interleaved(chunk);
        done += chunk;
        if (ee->pc != last_ee_pc) {
            printf("[R1046-WAKE] ee_pc MOVED 0x%08x -> 0x%08x at instr=%llu (delta_from_start=%llu)\n",
                   last_ee_pc, ee->pc, (unsigned long long)ee->instructions_executed,
                   (unsigned long long)(ee->instructions_executed - start_instr));
            last_ee_pc = ee->pc;
            last_move_at = ee->instructions_executed;
        }
    }

    printf("[R1046-WAKE] DONE total_instr=%llu ee_pc=0x%08x halted=%u wake_count=%u last_move_at=%llu\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, wake_count, (unsigned long long)last_move_at);
    printf("[R1046-WAKE] start_ee_pc=0x%08x final_ee_pc=0x%08x net_moved=%d\n",
           start_ee_pc, ee->pc, ee->pc != start_ee_pc);

    for (int i = 0; i < N_TARGETS; i++) {
        int t = targets[i];
        printf("[R1046-WAKE] post tid=%d status=0x%x wait_type=%d wait_id=%d entry=0x%08x pc=0x%08x\n",
               t, iop_hle_thread_get_status(t), iop_hle_thread_get_wait_type(t), iop_hle_thread_get_wait_id(t),
               iop_hle_thread_get_entry(t), iop_hle_thread_get_pc(t));
    }
    return 0;
}
