/*
 * Round 808 (task #805 fallback, "grab all sources including sony and see
 * if you can write a fix" per user's explicit instruction): full EE HLE
 * thread-table census for the live GT3 checkpoint. Companion to
 * r781_gt3_regdump.c (same load pattern, generalized to loop over every
 * thread instead of dumping GPRs) - this round's chain-push found GT3
 * stuck at pc=0x00000000, vu1_instr=0, with tid=3 reported as "current"
 * at the end of every 10M-slice chunk despite Round 782's null-jalr
 * ra==0 guard calling ee_hle_thread_exit_current() on it. This tool's
 * job is to answer: how many threads exist, what is each one's real
 * status/priority/wait_type/wait_id/entry/saved_pc, and specifically -
 * is thread 3 actually DORMANT (dead, as the fix intends) or is
 * something re-creating/reusing it every cycle.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"

static const char *status_name(uint32_t st)
{
    /* Matches this project's own ee_hle_thread.c status constants
     * (EE_THS_* - RUN/READY/WAIT/SUSPEND/DORMANT), printed numerically
     * as well in case the enum has grown since this tool was written. */
    switch (st) {
        case 0: return "DORMANT";
        case 1: return "READY";
        case 2: return "RUN";
        case 4: return "WAIT";
        case 8: return "SUSPEND";
        case 0x40: return "WAIT_SUSPEND(WAIT|SUSPEND=0xC? check bits)";
        default: return "?";
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <ckpt_path>\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *ckpt_path = argv[2];

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    printf("[R808-CENSUS] total_instr=%llu pc=0x%08x halted=%u\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);

    int count = ee_hle_thread_get_thread_count();
    int cur = ee_hle_thread_get_current_thread_id();
    printf("[R808-CENSUS] thread_count=%d current_thread_id=%d\n", count, cur);
    printf("[R808-CENSUS] rotate_calls=%llu\n", (unsigned long long)ee_hle_thread_get_rotate_calls());
    /* Round 808 continuation (task #810): direct signal-call counters
     * instead of reverse-engineering syscall numbers from raw
     * disassembly - answers "has ANYTHING, ever, in this run, called
     * SignalSema(semid)" for the two semaphores threads 1/2 are parked
     * on, far more reliably than manual stub-address pattern matching. */
    for (int s = 0; s < 8; s++) {
        printf("[R808-CENSUS] signal_calls(semid=%d)=%llu\n", s, (unsigned long long)ee_hle_thread_get_signal_calls(s));
    }

    /* current_thread_id is 1-based per ee_hle_thread.c line 66 ("1-based;
     * 0 = none yet"), and thread_count tracks the highest slot ever used
     * (see line 425: `if (slot > g.thread_count) g.thread_count = slot;`)
     * - so valid tids are 1..thread_count inclusive, NOT 0..count-1. An
     * earlier version of this tool wrongly 0-indexed and silently missed
     * the highest-numbered (and, here, current) thread entirely. */
    for (int t = 1; t <= count; t++) {
        uint32_t status = ee_hle_thread_get_status(t);
        uint32_t prio = ee_hle_thread_get_priority(t);
        uint32_t wtype = ee_hle_thread_get_wait_type(t);
        uint32_t wid = ee_hle_thread_get_wait_id(t);
        uint32_t entry = ee_hle_thread_get_entry(t);
        uint32_t saved_pc = ee_hle_thread_get_saved_pc(t);
        uint32_t wcount = ee_hle_thread_get_wakeup_count(t);
        unsigned long long wcalls = (unsigned long long)ee_hle_thread_get_wakeup_calls(t);
        printf("[R808-CENSUS] tid=%d status=0x%x(%s) prio=%u wait_type=%u wait_id=%u entry=0x%08x saved_pc=0x%08x wakeup_count=%u wakeup_calls=%llu%s\n",
               t, status, status_name(status), prio, wtype, wid, entry, saved_pc, wcount, wcalls,
               (t == cur) ? "  <-- CURRENT" : "");
    }
    return 0;
}
