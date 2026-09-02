/*
 * Round 733 (task #447 continuation, user: "use all sources available
 * to track down this issue and fix them once and for all"). Resumes
 * GT3's live checkpoint (Round 729-732, now total_instr~5.04B) WITHOUT
 * running it further, and dumps:
 *
 *   1. The full EE HLE thread table (ee_hle_thread.h's Round 612/613
 *      diagnostic accessors) - status/priority/wait_type/wait_id/
 *      entry/saved_pc/wakeup_count for every tracked thread, to see
 *      exactly what each one is doing/blocked on right now.
 *   2. Raw EE RAM words around the three PCs Round 732's chained runs
 *      observed the EE cycling through (0x0061bbe0-0x0061bbf8,
 *      0x00614af0, 0x0060dc08), written to raw dump files so the
 *      existing Round 655 EE disassembler tool can decode them.
 *
 * Goal: determine whether GT3's stall is (a) a genuine, fixable
 * modeling gap - some thread is READY-but-never-scheduled, or WAITing
 * on a signal this project's own code should be producing but isn't -
 * or (b) correct real-hardware idle/VBLANK-wait behavior with the
 * actual blocker sitting somewhere this project hasn't looked yet.
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
#include "core/hw/iop_cdvd.h"

static void dump_words(ee_state_t *st, const char *path, uint32_t start, int count)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return; }
    for (int i = 0; i < count; i++) {
        uint32_t word = ee_mem_read32(st, start + (uint32_t)(i * 4));
        uint8_t b[4];
        b[0] = (uint8_t)(word & 0xFF);
        b[1] = (uint8_t)((word >> 8) & 0xFF);
        b[2] = (uint8_t)((word >> 16) & 0xFF);
        b[3] = (uint8_t)((word >> 24) & 0xFF);
        fwrite(b, 1, 4, f);
    }
    fclose(f);
    printf("[R733] dumped %d words from 0x%08x to %s\n", count, start, path);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path>\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t  *ee  = ee_core_get_state();
    gs_state_t  *gs  = gs_get_state();
    gif_state_t *gif = gif_get_state();
    vu1_state_t *vu1 = vu1_get_state();

    int tid = ee_hle_thread_get_current_thread_id();
    printf("[R733] total_instr=%llu pc=0x%08x halted=%u current_tid=%d\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, tid);
    printf("[R733] vu1_instr=%llu gif_path1=%llu qw_seen=%llu pmode=0x%02x dispfb1=0x%08x dispfb2=0x%08x\n",
           (unsigned long long)(vu1 ? vu1->instructions_executed : 0),
           (unsigned long long)(gif ? gif->gif_path1_transfers : 0),
           (unsigned long long)(gif ? gif->quadwords_seen : 0),
           (unsigned)gs->pmode, (unsigned)gs->dispfb1, (unsigned)gs->dispfb2);

    int n = ee_hle_thread_get_thread_count();
    printf("[R733] ee_hle thread_count=%d\n", n);
    printf("[R733] %-4s %-6s %-8s %-9s %-8s %-10s %-10s %-10s\n",
           "tid", "cur?", "status", "priority", "waittype", "waitid", "entry", "saved_pc");
    for (int i = 1; i <= n + 4 && i <= 64; i++) {
        uint32_t status = ee_hle_thread_get_status(i);
        if (status == 0) continue; /* skip unused/invalid slots */
        uint32_t prio = ee_hle_thread_get_priority(i);
        uint32_t wtype = ee_hle_thread_get_wait_type(i);
        uint32_t wid = ee_hle_thread_get_wait_id(i);
        uint32_t entry = ee_hle_thread_get_entry(i);
        uint32_t saved_pc = ee_hle_thread_get_saved_pc(i);
        uint32_t wakeups = ee_hle_thread_get_wakeup_count(i);
        printf("[R733] %-4d %-6s 0x%06x %-9u %-8u 0x%08x 0x%08x 0x%08x wakeups=%u\n",
               i, (i == tid) ? "CUR" : "", status, prio, wtype, wid, entry, saved_pc, wakeups);
    }

    /* Round 732's chained runs observed the EE cycling through these
     * three PCs. Dump 32 words (128 bytes) around each. */
    dump_words(ee, "/tmp/round729_gt3/r733_0061bbe0.bin", 0x0061bbc0, 32);
    dump_words(ee, "/tmp/round729_gt3/r733_00614af0.bin", 0x00614ad0, 32);
    dump_words(ee, "/tmp/round729_gt3/r733_0060dc08.bin", 0x0060dbe8, 32);

    /* Also dump the current live PC's neighborhood, whatever it is now. */
    uint32_t cur_base = ee->pc & ~0xFu;
    dump_words(ee, "/tmp/round729_gt3/r733_current_pc.bin", cur_base - 0x20, 32);
    printf("[R733] current pc=0x%08x, dumped window base=0x%08x\n", ee->pc, cur_base - 0x20);

    return 0;
}
