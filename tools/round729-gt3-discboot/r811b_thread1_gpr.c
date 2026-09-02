/*
 * Round 811 continuation (task #811, per user's explicit backward-trace
 * request): dump thread 1's full saved register context (using the new
 * ee_hle_thread_get_gpr() accessor added this round) plus its
 * status/wait_type/wait_id/saved_pc/entry, for the earliest available
 * GT3 checkpoint. Purpose: get ground truth on the EXACT pc thread 1 is
 * parked at (not assumed from an old summary) and its $ra/$sp/$gp/
 * $a0-$a3/$v0/$v1 at that point, to backward-disassemble the containing
 * function per the user's request.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"

static const char *regname(int r)
{
    static const char *names[32] = {
        "zero","at","v0","v1","a0","a1","a2","a3",
        "t0","t1","t2","t3","t4","t5","t6","t7",
        "s0","s1","s2","s3","s4","s5","s6","s7",
        "t8","t9","k0","k1","gp","sp","s8","ra"
    };
    return (r >= 0 && r < 32) ? names[r] : "?";
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <ckpt_path> [tid]\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(argv[2], &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    int tid = argc > 3 ? atoi(argv[3]) : 1;

    ee_state_t *ee = ee_core_get_state();
    printf("[R811B] total_instr=%llu pc=0x%08x\n", (unsigned long long)ee->instructions_executed, ee->pc);

    int count = ee_hle_thread_get_thread_count();
    int cur = ee_hle_thread_get_current_thread_id();
    printf("[R811B] thread_count=%d current_thread_id=%d\n", count, cur);

    uint32_t status = ee_hle_thread_get_status(tid);
    uint32_t wtype = ee_hle_thread_get_wait_type(tid);
    uint32_t wid = ee_hle_thread_get_wait_id(tid);
    uint32_t entry = ee_hle_thread_get_entry(tid);
    uint32_t saved_pc = ee_hle_thread_get_saved_pc(tid);
    uint32_t wcount = ee_hle_thread_get_wakeup_count(tid);

    printf("[R811B] tid=%d status=0x%x wait_type=%u wait_id=%u entry=0x%08x saved_pc=0x%08x wakeup_count=%u\n",
           tid, status, wtype, wid, entry, saved_pc, wcount);

    printf("[R811B] --- saved GPR context for tid=%d ---\n", tid);
    for (int r = 0; r < 32; r++) {
        uint64_t v = ee_hle_thread_get_gpr(tid, r);
        printf("[R811B]   $%-4s (r%02d) = 0x%08x%08x\n", regname(r), r,
               (unsigned)(v >> 32), (unsigned)(v & 0xFFFFFFFFu));
    }

    return 0;
}
