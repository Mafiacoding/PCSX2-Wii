/*
 * Round 808 continuation (task #810): does ANYTHING, ever, call
 * SignalSema(5) or SignalSema(0) during GT3's thread-3 loading-screen
 * lifetime, or afterward (e.g. from an interrupt handler via
 * ee_core_park_tick(), independent of thread scheduling)? Uses the
 * existing ee_hle_thread_get_signal_calls(semid) live counter (Round
 * 733 citation - NOT checkpointed, only meaningful within one
 * continuous observation window) rather than reverse-engineering
 * syscall numbers from raw disassembly. Resumes from
 * r781_gt3_test2.ckpt (total_instr=678,449,972, thread 3 confirmed
 * alive) and runs forward, printing sig5/sig0 counters periodically
 * across the thread-3 death boundary (~total_instr=1,274,161,542).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path> [extra_chunks]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    int extra_chunks = argc > 4 ? atoi(argv[4]) : 5;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    printf("[R808-SEMWATCH] start total_instr=%llu pc=0x%08x sig5=%llu sig0=%llu\n",
           (unsigned long long)ee->instructions_executed, ee->pc,
           (unsigned long long)ee_hle_thread_get_signal_calls(5),
           (unsigned long long)ee_hle_thread_get_signal_calls(0));

    const long SLICE = 1000000; /* 1M-slice chunks: coarse but fast */
    long chunk = 0;
    int seen_dormant = 0;
    int chunks_after_dormant = 0;
    uint32_t max_tid2_status_seen = 0; /* did tid2 EVER leave WAIT (0x4), even briefly? */
    /* Run until thread 3 goes DORMANT, then `extra_chunks` more chunks
     * past that to see if anything signals afterward, then stop. */
    while (chunks_after_dormant < extra_chunks && chunk < 900) {
        system_run_interleaved(SLICE);
        chunk++;
        uint32_t st3 = ee_hle_thread_get_status(3);
        uint32_t st2 = ee_hle_thread_get_status(2);
        uint32_t wid2 = ee_hle_thread_get_wait_id(2);
        uint32_t wtype2 = ee_hle_thread_get_wait_type(2);
        if (st2 != 0x4u) max_tid2_status_seen = st2; /* capture any non-WAIT sighting */
        unsigned long long s5 = (unsigned long long)ee_hle_thread_get_signal_calls(5);
        unsigned long long s0 = (unsigned long long)ee_hle_thread_get_signal_calls(0);
        /* Fine-grained for the first 30 chunks (where sig0 first climbs
         * from 0), coarse afterward - looking for whether tid2 EVER
         * transitions off WAIT/wait_id=0 in response to a signal. */
        if (chunk <= 30 || chunk % 20 == 0 || st3 == 0x10u) {
            printf("[R808-SEMWATCH] chunk=%ld total_instr=%llu pc=0x%08x tid3_status=0x%x sig5=%llu sig0=%llu tid2_status=0x%x tid2_wait_type=%u tid2_wait_id=%u\n",
                   chunk, (unsigned long long)ee->instructions_executed, ee->pc, st3, s5, s0, st2, wtype2, wid2);
        }
        if (st3 == 0x10u) {
            if (!seen_dormant) {
                printf("[R808-SEMWATCH] *** thread 3 went DORMANT at chunk=%ld total_instr=%llu sig5=%llu sig0=%llu ***\n",
                       chunk, (unsigned long long)ee->instructions_executed, s5, s0);
                seen_dormant = 1;
            }
            chunks_after_dormant++;
        }
    }
    printf("[R808-SEMWATCH] tid2 ever left WAIT status: %s (last non-WAIT status seen=0x%x)\n",
           max_tid2_status_seen != 0 ? "YES" : "NO (never)", max_tid2_status_seen);
    printf("[R808-SEMWATCH] FINAL: total_instr=%llu sig5=%llu sig0=%llu wakeup_calls(tid1)=%llu wakeup_calls(tid2)=%llu\n",
           (unsigned long long)ee->instructions_executed,
           (unsigned long long)ee_hle_thread_get_signal_calls(5),
           (unsigned long long)ee_hle_thread_get_signal_calls(0),
           (unsigned long long)ee_hle_thread_get_wakeup_calls(1),
           (unsigned long long)ee_hle_thread_get_wakeup_calls(2));
    return 0;
}
