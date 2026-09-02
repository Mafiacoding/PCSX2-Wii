/*
 * Round 777 (task #785): KOF thread census. Loads the live KOF
 * checkpoint (no further execution) and dumps every EE HLE thread's
 * status/priority/wait_type/wait_id/entry/saved_pc/wakeup_count via
 * the existing ee_hle_thread_get_* introspection API (Round 612/613,
 * reused unmodified - see include/core/ee/ee_hle_thread.h), following
 * the same pattern already used for GT3 in Round 733/740. Answers
 * "what is KOF's own thread doing right now" as a companion to the
 * r777_kof_gsdbg_driver.c GS-write/SetGsCrt-call survey (task #784).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"

static const char *status_name(uint32_t s) {
    switch (s) {
        case 0: return "NONE";
        case 1: return "RUN";
        case 2: return "READY";
        case 4: return "WAIT";
        case 8: return "SUSPEND";
        case 0x10: return "WAIT_SUSPEND";
        case 0x40: return "DORMANT";
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

    /* Disc path unused for pure state inspection - pass NULL, checkpoint
     * format doesn't require re-mounting the ISO to read TCB state. */
    if (checkpoint_load(ckpt_path, &bios, &bios, NULL) != 0) {
        fprintf(stderr, "checkpoint_load fail\n");
        return 1;
    }

    ee_state_t *ee = ee_core_get_state();
    int count = ee_hle_thread_get_thread_count();
    int cur = ee_hle_thread_get_current_thread_id();

    printf("[R777-CENSUS] total_instr=%llu pc=0x%08x halted=%u thread_count=%d current_tid=%d\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, count, cur);

    for (int tid = 1; tid <= count + 4; tid++) {
        uint32_t st_ = ee_hle_thread_get_status(tid);
        if (st_ == 0 && tid > count) continue;
        uint32_t prio = ee_hle_thread_get_priority(tid);
        uint32_t wt = ee_hle_thread_get_wait_type(tid);
        uint32_t wid = ee_hle_thread_get_wait_id(tid);
        uint32_t entry = ee_hle_thread_get_entry(tid);
        uint32_t saved_pc = ee_hle_thread_get_saved_pc(tid);
        uint32_t wakeups = ee_hle_thread_get_wakeup_count(tid);
        uint64_t wcalls = ee_hle_thread_get_wakeup_calls(tid);
        printf("[R777-CENSUS] tid=%d status=%s(0x%x) prio=%u wait_type=%u wait_id=%u "
               "entry=0x%08x saved_pc=0x%08x wakeup_count=%u wakeup_calls=%llu%s\n",
               tid, status_name(st_), st_, prio, wt, wid, entry, saved_pc, wakeups,
               (unsigned long long)wcalls, (tid == cur) ? "  <-- CURRENT" : "");
    }

    return 0;
}
