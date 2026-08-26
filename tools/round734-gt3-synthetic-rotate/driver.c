/*
 * Round 734 (task #447 continuation, user: "maybe its time we write our
 * self some code RotateThreadReadyQueue"). Diagnostic-only experiment,
 * same precedent class as Round 568's synthetic WaitSema(0) signal test:
 * periodically injects ee_hle_thread_debug_force_rotate(st, 5) (Round 734's
 * new diagnostic function) against GT3's live checkpoint, forcing threads
 * 3/5 to actually get real CPU time despite nothing in GT3's own code ever
 * calling RotateThreadReadyQueue or WakeupThread on them (Round 733's
 * finding). Watches:
 *   - which thread ends up CURRENT after each rotate (does the switch
 *     actually take, or does thread 4 immediately win right back)
 *   - CDVD dispatch_ncmd/dispatch_scmd call counts (Round 732's counters)
 *   - triangle/line/sprite/xgkick draw counters
 *   - thread 3/5's own status/wait_type/saved_pc after running, to see
 *     whether they do real work then re-park (SleepThread again) or run
 *     substantially.
 *
 * This is explicitly NOT proposing that the real scheduler auto-rotate -
 * see ee_hle_thread.c's own Round 734 comment. This driver's injection is
 * scratch-only and never touches organic emulation behavior.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/hw/gs.h"
#include "core/hw/gif.h"
#include "core/hw/iop_cdvd.h"

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <bios> <disc> <ckpt> <total_budget> [rotate_interval]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    uint64_t budget = strtoull(argv[4], NULL, 10);
    uint64_t interval = argc > 5 ? strtoull(argv[5], NULL, 10) : 500000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t  *ee  = ee_core_get_state();
    gif_state_t *gif = gif_get_state();

    uint64_t ncmd0 = iop_cdvd_get_ncmd_call_count();
    uint64_t scmd0 = iop_cdvd_get_scmd_call_count();
    uint64_t tri0 = gif ? gif->triangles_drawn : 0;
    uint64_t lin0 = gif ? gif->lines_drawn : 0;
    uint64_t spr0 = gif ? gif->sprites_drawn : 0;
    uint64_t xg0  = gif ? gif->gif_path1_transfers : 0;

    printf("[R734] START ncmd=%llu scmd=%llu tri=%llu lines=%llu sprites=%llu xg=%llu\n",
           (unsigned long long)ncmd0, (unsigned long long)scmd0,
           (unsigned long long)tri0, (unsigned long long)lin0,
           (unsigned long long)spr0, (unsigned long long)xg0);

    uint64_t done = 0;
    int rotate_num = 0;
    int last_seen_tid[16] = {0};
    while (done < budget && !ee->halted) {
        system_run_interleaved(interval);
        done += interval;

        /* Inject the synthetic wake+rotate. Round 734 correction: a bare
         * rotate does nothing for threads 3/5 because RotateThreadReadyQueue
         * only reorders threads that are ALREADY RUN/READY - it does not
         * wake WAIT/SLEEP threads (confirmed: first version of this driver,
         * rotate-only, never once switched away from thread 4, because
         * threads 3/5 stayed WAIT the whole time and were never candidates).
         * Real WakeupThread + RotateThreadReadyQueue are two separate real
         * kernel primitives - combine them here, exactly as real calling
         * code would if it wanted to both wake a sibling AND ensure it
         * actually gets a turn against an already-running same-priority
         * thread. */
        if (ee_hle_thread_get_status(3) == 0x4 /* EE_THS_WAIT */) ee_hle_thread_debug_force_wakeup(3);
        if (ee_hle_thread_get_status(5) == 0x4 /* EE_THS_WAIT */) ee_hle_thread_debug_force_wakeup(5);
        ee_hle_thread_debug_force_rotate(ee, 5);
        rotate_num++;
        int cur = ee_hle_thread_get_current_thread_id();
        if (cur >= 0 && cur < 16) last_seen_tid[cur] = 1;

        if (rotate_num <= 40 || rotate_num % 20 == 0) {
            printf("[R734] rotate#%-4d done=%-10llu current_tid=%d pc=0x%08x\n",
                   rotate_num, (unsigned long long)done, cur, ee->pc);
        }

        /* Run a short slice post-rotate so the newly-current thread gets
         * real time before the NEXT rotate potentially displaces it. */
        system_run_interleaved(interval / 4 > 0 ? interval / 4 : 1000);
        done += interval / 4 > 0 ? interval / 4 : 1000;
    }

    printf("[R734] ran total %llu instr (%d rotates), halted=%u\n",
           (unsigned long long)done, rotate_num, ee->halted);
    printf("[R734] threads ever seen CURRENT after a rotate: ");
    for (int i = 0; i < 16; i++) if (last_seen_tid[i]) printf("%d ", i);
    printf("\n");

    uint64_t ncmd1 = iop_cdvd_get_ncmd_call_count();
    uint64_t scmd1 = iop_cdvd_get_scmd_call_count();
    printf("[R734] END ncmd=%llu->%llu  scmd=%llu->%llu\n",
           (unsigned long long)ncmd0, (unsigned long long)ncmd1,
           (unsigned long long)scmd0, (unsigned long long)scmd1);
    printf("[R734] tri: %llu->%llu  lines: %llu->%llu  sprites: %llu->%llu  xgkick: %llu->%llu\n",
           (unsigned long long)tri0, (unsigned long long)(gif ? gif->triangles_drawn : 0),
           (unsigned long long)lin0, (unsigned long long)(gif ? gif->lines_drawn : 0),
           (unsigned long long)spr0, (unsigned long long)(gif ? gif->sprites_drawn : 0),
           (unsigned long long)xg0,  (unsigned long long)(gif ? gif->gif_path1_transfers : 0));

    for (int t = 3; t <= 5; t += 2) {
        printf("[R734] thread %d final: status=0x%x wtype=%u wid=%u saved_pc=0x%08x wakeups=%u\n",
               t, ee_hle_thread_get_status(t), ee_hle_thread_get_wait_type(t),
               ee_hle_thread_get_wait_id(t), ee_hle_thread_get_saved_pc(t),
               ee_hle_thread_get_wakeup_count(t));
    }

    /* Deliberately NOT saving the checkpoint - this is a throwaway
     * synthetic-injection branch, not real organic emulation state we
     * want to persist as GT3's ongoing boot checkpoint. */
    return 0;
}
