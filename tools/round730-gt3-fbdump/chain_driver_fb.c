/*
 * Round 730 (task #536/#447 continuation): per user request "keep going
 * and see if we get any picture output". Extends Round 729's
 * checkpoint-chained GT3 disc-boot survey (chain_driver.c) with actual
 * framebuffer dumping, mirroring Round 450/588/717's own PPM-dump
 * convention (gs_mem_read_psmct32 over the real GS-memory bp/bw the
 * emulator is tracking, written to a raw P6 PPM).
 *
 * Dumps THREE candidate buffers each time it's asked to snapshot:
 *   1. gif->fbp/fbw      - the current real draw target (FRAME reg)
 *   2. GS circuit 1 (DISPFB1, decoded via gs_decode_dispfb())
 *   3. GS circuit 2 (DISPFB2, decoded via gs_decode_dispfb())
 * because Round 321 found real OSDSYS uses circuit 2, but this is
 * GT3 in-game, not OSDSYS, and DISPFB2 was observed oscillating
 * 0x1400/0x1446 in Round 729 (a real double-buffer flip candidate).
 *
 * Same checkpoint-chaining pattern as Round 729's chain_driver.c
 * (10,000,000-instruction budget per invocation to fit the sandbox's
 * ~178s per-call ceiling). Checkpoint file itself never committed
 * (BIOS/disc-derived RAM content, per checkpoint.h). PPM dumps are
 * OUR OWN emulator's rendered pixel output (not raw copyrighted
 * asset bytes) but are still written to /tmp scratch only, per this
 * round's own caution - only summary pixel statistics are reported
 * back, and any image shown to the user is converted/relayed
 * out-of-band rather than committed to the tracked repo.
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
#include "core/hw/gs_mem.h"
#include "core/hw/gs_wii_output.h"
#include "core/hw/gif.h"
#include "core/hw/vu.h"
#include "core/hw/iop_cdvd.h"

static long dump_ppm(const char *path, uint32_t bp, uint32_t bw, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    long non_zero = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t px = gs_mem_read_psmct32(bp, bw ? bw : (uint32_t)w, (uint32_t)x, (uint32_t)y);
            uint8_t r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF;
            if (px != 0) non_zero++;
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    }
    fclose(f);
    return non_zero;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path> <start|continue> [budget] [tag]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    const char *mode = argv[4];
    uint64_t budget = argc > 5 ? strtoull(argv[5], NULL, 10) : 10000000ull;
    const char *tag = argc > 6 ? argv[6] : "snap";

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }

    if (strcmp(mode, "start") == 0) {
        if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
        if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    } else {
        if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }
    }

    ee_state_t  *ee  = ee_core_get_state();
    gs_state_t  *gs  = gs_get_state();
    gif_state_t *gif = gif_get_state();
    vu1_state_t *vu1 = vu1_get_state();

    uint64_t chunk = 10000000ull, done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
    }

    int tid = ee_hle_thread_get_current_thread_id();
    printf("[R730-FB] ran %llu, total_instr=%llu pc=0x%08x halted=%u tid=%d "
           "vu1_instr=%llu gif_path1=%llu qw_seen=%llu pmode=0x%02x "
           "dispfb1=0x%08x dispfb2=0x%08x draw_fbp=%u draw_fbw=%u "
           "triangles=%llu lines=%llu sprites=%llu points=%llu\n",
           (unsigned long long)done, (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, tid,
           (unsigned long long)(vu1 ? vu1->instructions_executed : 0),
           (unsigned long long)(gif ? gif->gif_path1_transfers : 0),
           (unsigned long long)(gif ? gif->quadwords_seen : 0),
           (unsigned)gs->pmode, (unsigned)gs->dispfb1, (unsigned)gs->dispfb2,
           gif ? gif->fbp : 0, gif ? gif->fbw : 0,
           (unsigned long long)(gif ? gif->triangles_drawn : 0),
           (unsigned long long)(gif ? gif->lines_drawn : 0),
           (unsigned long long)(gif ? gif->sprites_drawn : 0),
           (unsigned long long)(gif ? gif->points_drawn : 0));

    char path[512];
    uint32_t bp, bw;

    if (gif) {
        snprintf(path, sizeof(path), "/tmp/round729_gt3/%s_draw.ppm", tag);
        long nz = dump_ppm(path, gif->fbp, gif->fbw, 640, 224);
        printf("[R730-FB] draw-target dump: fbp=%u fbw=%u non_zero=%ld/%d -> %s\n",
               gif->fbp, gif->fbw, nz, 640 * 224, path);
    }

    gs_decode_dispfb(gs->dispfb1, &bp, &bw);
    snprintf(path, sizeof(path), "/tmp/round729_gt3/%s_disp1.ppm", tag);
    { long nz = dump_ppm(path, bp, bw, 640, 224);
      printf("[R730-FB] DISPFB1 dump: bp=%u bw=%u non_zero=%ld/%d -> %s\n", bp, bw, nz, 640*224, path); }

    gs_decode_dispfb(gs->dispfb2, &bp, &bw);
    snprintf(path, sizeof(path), "/tmp/round729_gt3/%s_disp2.ppm", tag);
    { long nz = dump_ppm(path, bp, bw, 640, 224);
      printf("[R730-FB] DISPFB2 dump: bp=%u bw=%u non_zero=%ld/%d -> %s\n", bp, bw, nz, 640*224, path); }

    if (ee->halted) {
        printf("[R730-FB] EE halted: %s\n", ee->halt_reason);
        return 0;
    }

    if (checkpoint_save(ckpt_path) != 0) { fprintf(stderr, "checkpoint_save fail\n"); return 1; }
    printf("[R730-FB] checkpoint saved to %s\n", ckpt_path);
    return 0;
}
