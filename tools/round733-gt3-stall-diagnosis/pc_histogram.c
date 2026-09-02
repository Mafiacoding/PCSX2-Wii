/*
 * Round 733 continuation: the tight loop at 0x0061bbe0-0x0061bbf8
 * (disassembled this round) is a real, textbook EE VBLANK_START-wait
 * idiom - write 4 (ack) to I_STAT (0x1000F000), then busy-poll I_STAT
 * until bit 2 (VBLANK_START, real ps2tek EE INTC cause table) sets
 * again. This is CORRECT real PS2 code, not a bug - confirmed by a
 * fresh checkpoint snapshot showing the CURRENT live pc (0x0060dc08,
 * a fixed-point multiply/shift/clamp routine, not the idle loop) is
 * NOT stuck there either. So thread 4 alternates between real
 * computation and the VBLANK wait, exactly as real game code would.
 *
 * This tool answers the next real question: over a modest further
 * instruction budget, what is the FULL distribution of addresses
 * thread 4 (and the whole EE) actually executes - is real per-frame
 * work happening that just never produces a new XGKICK, or is
 * thread 4 doing near-nothing but the idle loop with only rare,
 * unproductive excursions?
 *
 * Samples ee->pc every N instructions (small step chunks) into a
 * hash-free simple top-K tracking array, and watches
 * gif_path1_transfers/triangles_drawn/lines_drawn/sprites_drawn/
 * points_drawn for ANY change across the whole run.
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
#include "core/hw/vu.h"

#define MAX_BUCKETS 4096
typedef struct { uint32_t pc; uint64_t count; } bucket_t;
static bucket_t g_buckets[MAX_BUCKETS];
static int g_nbuckets = 0;

static void record(uint32_t pc)
{
    /* bucket by (pc & ~0xF) to group nearby addresses within the same loop */
    uint32_t key = pc & ~0xFu;
    for (int i = 0; i < g_nbuckets; i++) {
        if (g_buckets[i].pc == key) { g_buckets[i].count++; return; }
    }
    if (g_nbuckets < MAX_BUCKETS) {
        g_buckets[g_nbuckets].pc = key;
        g_buckets[g_nbuckets].count = 1;
        g_nbuckets++;
    }
}

static int cmp_desc(const void *a, const void *b)
{
    const bucket_t *ba = (const bucket_t *)a, *bb = (const bucket_t *)b;
    if (ba->count > bb->count) return -1;
    if (ba->count < bb->count) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path> <total_budget> [sample_stride]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    uint64_t budget = strtoull(argv[4], NULL, 10);
    uint64_t stride = argc > 5 ? strtoull(argv[5], NULL, 10) : 100ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t  *ee  = ee_core_get_state();
    gif_state_t *gif = gif_get_state();
    gs_state_t  *gs  = gs_get_state();

    uint64_t xgkick0 = gif ? gif->gif_path1_transfers : 0;
    uint64_t tri0 = gif ? gif->triangles_drawn : 0;
    uint64_t lin0 = gif ? gif->lines_drawn : 0;
    uint64_t spr0 = gif ? gif->sprites_drawn : 0;
    uint64_t pts0 = gif ? gif->points_drawn : 0;
    uint32_t dispfb2_0 = gs->dispfb2;

    uint64_t done = 0;
    uint64_t samples = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(stride);
        done += stride;
        record(ee->pc);
        samples++;

        /* Report immediately if any drawing counter changes mid-run. */
        uint64_t xg = gif ? gif->gif_path1_transfers : 0;
        if (xg != xgkick0) {
            printf("[R733-HIST] *** gif_path1_transfers CHANGED at done=%llu: %llu -> %llu (pc=0x%08x) ***\n",
                   (unsigned long long)done, (unsigned long long)xgkick0, (unsigned long long)xg, ee->pc);
            xgkick0 = xg;
        }
    }

    int tid = ee_hle_thread_get_current_thread_id();
    printf("[R733-HIST] ran %llu instr (%llu samples), pc=0x%08x halted=%u tid=%d\n",
           (unsigned long long)done, (unsigned long long)samples, ee->pc, ee->halted, tid);
    printf("[R733-HIST] xgkick: %llu->%llu  tri: %llu->%llu  lines: %llu->%llu  sprites: %llu->%llu  points: %llu->%llu\n",
           (unsigned long long)xgkick0, (unsigned long long)(gif ? gif->gif_path1_transfers : 0),
           (unsigned long long)tri0, (unsigned long long)(gif ? gif->triangles_drawn : 0),
           (unsigned long long)lin0, (unsigned long long)(gif ? gif->lines_drawn : 0),
           (unsigned long long)spr0, (unsigned long long)(gif ? gif->sprites_drawn : 0),
           (unsigned long long)pts0, (unsigned long long)(gif ? gif->points_drawn : 0));
    printf("[R733-HIST] dispfb2: 0x%08x -> 0x%08x\n", dispfb2_0, gs->dispfb2);

    printf("[R733-HIST] WakeupThread call counts (target tid -> count), nonzero only:\n");
    for (int t = 0; t <= 32; t++) {
        uint64_t c = ee_hle_thread_get_wakeup_calls(t);
        if (c > 0) printf("[R733-HIST]   WakeupThread(%d) called %llu times\n", t, (unsigned long long)c);
    }
    printf("[R733-HIST] SignalSema call counts (semid -> count), nonzero only:\n");
    for (int s = 0; s <= 64; s++) {
        uint64_t c = ee_hle_thread_get_signal_calls(s);
        if (c > 0) printf("[R733-HIST]   SignalSema(%d) called %llu times\n", s, (unsigned long long)c);
    }
    printf("[R733-HIST] RotateThreadReadyQueue call count: %llu\n",
           (unsigned long long)ee_hle_thread_get_rotate_calls());

    qsort(g_buckets, (size_t)g_nbuckets, sizeof(bucket_t), cmp_desc);
    printf("[R733-HIST] top %d PC buckets (of %d distinct, %llu samples):\n",
           g_nbuckets < 30 ? g_nbuckets : 30, g_nbuckets, (unsigned long long)samples);
    for (int i = 0; i < g_nbuckets && i < 30; i++) {
        printf("[R733-HIST]   0x%08x: %llu samples (%.2f%%)\n",
               g_buckets[i].pc, (unsigned long long)g_buckets[i].count,
               100.0 * (double)g_buckets[i].count / (double)samples);
    }

    if (ee->halted) {
        printf("[R733-HIST] EE halted: %s\n", ee->halt_reason);
        return 0;
    }
    if (checkpoint_save(ckpt_path) != 0) { fprintf(stderr, "checkpoint_save fail\n"); return 1; }
    printf("[R733-HIST] checkpoint saved to %s\n", ckpt_path);
    return 0;
}
