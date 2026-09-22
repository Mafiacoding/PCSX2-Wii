/* Round 1005 (task #983) part 2, CORRECTION to this tool's own first
 * pass (analyze.c/trace_driver.c): those tools instrumented the
 * legacy, OLD software syscall-bypass blocks in ee_core.c
 * (sysnum==64/65/66/68 around line 4177-4553) directly. Re-reading
 * ee_core.c's real SYSCALL handler (line ~3976-3992) found this is
 * DEAD CODE for these specific syscalls on the current boot path:
 * `if (ee_hle_thread_try_handle(st, sysnum, this_pc, in_delay_slot))
 * return 1;` is checked FIRST, and ee_hle_thread.c (Round 569's real
 * clean-room EE thread/semaphore scheduler) already handles sysnum
 * 64/65/66/68 itself (source/core/ee/ee_hle_thread.c lines 894+),
 * returning 1 before ee_core.c's own legacy blocks are ever reached -
 * which is exactly why the Round 1004-style scratch instrumentation
 * of ee_core.c's blocks captured zero hits despite the EE genuinely
 * parking. This tool instead uses ee_hle_thread.h's REAL, already-
 * existing public diagnostic accessors (no instrumentation needed)
 * to answer the same question correctly: which thread is parked,
 * on which semaphore, and has anything ever signaled it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <budget>\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    uint64_t budget = strtoull(argv[2], NULL, 10);
    uint64_t chunk = 1000000ull, done = 0;
    ee_state_t *ee = ee_core_get_state();
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
    }
    printf("\n[R1005-2] final ee_instr=%llu ee_pc=0x%08x halted=%d\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);

    int nthreads = ee_hle_thread_get_thread_count();
    printf("\n[R1005-2] thread_count=%d current_tid=%d\n", nthreads, ee_hle_thread_get_current_thread_id());
    for (int t = 0; t <= nthreads + 1; t++) {
        uint32_t status = ee_hle_thread_get_status(t);
        uint32_t wtype = ee_hle_thread_get_wait_type(t);
        uint32_t wid = ee_hle_thread_get_wait_id(t);
        uint32_t entry = ee_hle_thread_get_entry(t);
        uint32_t saved_pc = ee_hle_thread_get_saved_pc(t);
        uint32_t wakeups = ee_hle_thread_get_wakeup_count(t);
        printf("  tid=%d status=%u wait_type=%u wait_id=%u entry=0x%08x saved_pc=0x%08x wakeup_count=%u prio=%u\n",
               t, status, wtype, wid, entry, saved_pc, wakeups, ee_hle_thread_get_priority(t));
    }

    printf("\n[R1005-2] Signal-call counters (semid 0-7):\n");
    for (int s = 0; s < 8; s++) {
        printf("  semid=%d signal_calls=%llu\n", s, (unsigned long long)ee_hle_thread_get_signal_calls(s));
    }
    printf("\n[R1005-2] Wakeup-call counters (thid 0-7):\n");
    for (int t = 0; t < 8; t++) {
        printf("  thid=%d wakeup_calls=%llu\n", t, (unsigned long long)ee_hle_thread_get_wakeup_calls(t));
    }

    return 0;
}
