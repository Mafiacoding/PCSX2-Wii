/* Round 1006 part 2 (task #984): analyze.c's own fine-grained
 * 2000-raw-step check already PROVED the IOP's pc=0x00155C00 freeze
 * is a genuine park, not a coarse-sampling artifact (0 pc/instr
 * changes across 2000 individual iop_core_step() calls). That run
 * also printed iop_idle=1.
 *
 * Re-reading source/hw/iop_hle_thread.c's reschedule() (lines
 * 257-289) confirms this project has a REAL IOP-side HLE thread/
 * semaphore scheduler (the direct IOP analog of Round 569's EE
 * scheduler in ee_hle_thread.c), and that `st->idle = 1` there is set
 * ONLY when pick_next_ready() finds nothing ready (every thread WAIT/
 * DORMANT/SUSPEND) - i.e. iop_idle=1 is not a bug flag, it is this
 * project's own already-established "genuinely nothing to run" real
 * scheduler state (task #179, same idle-not-halt design as the EE
 * side). This tool applies the EXACT same correction Round 1005 made
 * on the EE side (use the real scheduler's own public accessors,
 * not raw disassembly of a frozen fetch address) to the IOP side:
 * dump iop_hle_thread.h's real per-thread state + global stats to
 * find which IOP thread(s) exist and what they're genuinely blocked
 * on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_hle_thread.h"

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
    iop_state_t *iop = iop_core_get_state();
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
    }
    printf("\n[R1006-2] final ee_instr=%llu ee_pc=0x%08x iop_instr=%llu iop_pc=0x%08x iop_idle=%d\n",
           (unsigned long long)ee->instructions_executed, ee->pc,
           (unsigned long long)iop->instructions_executed, iop->pc, iop->idle);

    int nthreads = iop_hle_thread_get_thread_count();
    printf("\n[R1006-2] IOP thread_count=%d current_tid=%d\n", nthreads, iop_hle_thread_get_current_thread_id());
    for (int t = 0; t <= nthreads + 1; t++) {
        uint32_t status = iop_hle_thread_get_status(t);
        int wtype = iop_hle_thread_get_wait_type(t);
        int wid = iop_hle_thread_get_wait_id(t);
        uint32_t entry = iop_hle_thread_get_entry(t);
        uint32_t tpc = iop_hle_thread_get_pc(t);
        uint32_t prio = iop_hle_thread_get_priority(t);
        printf("  tid=%d status=%u wait_type=%d wait_id=%d entry=0x%08x pc=0x%08x prio=%u\n",
               t, status, wtype, wid, entry, tpc, prio);
    }

    printf("\n[R1006-2] sema_count=%d evf_count=%d alarm_count=%d\n",
           iop_hle_thread_get_sema_count(), iop_hle_thread_get_evf_count(), iop_hle_thread_get_alarm_count());

    const iop_hle_thread_stats_t *st = iop_hle_thread_get_stats();
    printf("\n[R1006-2] stats: threads_created=%u threads_started=%u threads_exited=%u threads_deleted=%u context_switches=%u\n",
           st->threads_created, st->threads_started, st->threads_exited, st->threads_deleted, st->context_switches);
    printf("[R1006-2] stats: semas_created=%u semas_deleted=%u wait_sema_blocked=%u wait_sema_immediate=%u\n",
           st->semas_created, st->semas_deleted, st->wait_sema_blocked, st->wait_sema_immediate);
    printf("[R1006-2] stats: sleep_thread_blocked=%u delay_thread_blocked=%u\n",
           st->sleep_thread_blocked, st->delay_thread_blocked);
    printf("[R1006-2] stats: evflags_created=%u evflags_deleted=%u wait_evf_blocked=%u wait_evf_immediate=%u\n",
           st->evflags_created, st->evflags_deleted, st->wait_evf_blocked, st->wait_evf_immediate);
    printf("[R1006-2] stats: alarms_set=%u alarms_cancelled=%u alarms_fired=%u\n",
           st->alarms_set, st->alarms_cancelled, st->alarms_fired);

    return 0;
}
