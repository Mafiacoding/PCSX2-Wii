/*
 * Round 776 (task #536/#777, user request: "screenshot for me that we
 * have text from the bios"): checkpoint-chained diskless (no disc
 * mounted) JP-BIOS boot survey that dumps the live GS framebuffer to a
 * PPM after every invocation, so the survey can be pushed well past
 * the ~130M-instruction ceiling a single sandbox tool-call's wall-
 * clock budget allows for fb_dump.c's plain cold-start driver.
 * Modeled directly on tools/round729-gt3-discboot/r775_chain_driver.c
 * (start/continue + checkpoint_load/checkpoint_save), but with NO
 * disc mount at all (task #536's diskless scenario, not task #447's
 * disc-boot scenario) and with fb_dump.c's exact PPM-dump logic
 * (fbp/fbw from the real, live DISPFB2-derived gif_state_t fields,
 * gs_mem_read_psmct32() pixel reads) appended at the end of every
 * invocation instead of only at driver exit.
 *
 * Checkpoint file is scratch-only, confined to /tmp/, never
 * committed/rsynced, per this project's established leak-prevention
 * convention (include/core/checkpoint.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/hw/gs.h"
#include "core/hw/gs_mem.h"
#include "core/hw/gif.h"

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <bios_path> <ckpt_path> <start|continue> <budget> <out_ppm>\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *ckpt_path = argv[2];
    const char *mode = argv[3];
    uint64_t budget = strtoull(argv[4], NULL, 10);
    const char *out_ppm = argv[5];

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }

    if (strcmp(mode, "start") == 0) {
        if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
        /* deliberately NO iop_cdvd_mount_iso() call - diskless boot, per task #536 */
    } else {
        if (checkpoint_load(ckpt_path, &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }
    }

    ee_state_t *ee = ee_core_get_state();
    gif_state_t *gif = gif_get_state();
    gs_state_t *gs = gs_get_state();

    uint64_t chunk = 4000000ull, done = 0;
    while (done < budget) { system_run_interleaved(chunk); done += chunk; }

    uint32_t fbp = gif->fbp, fbw = gif->fbw ? gif->fbw : 640u;
    printf("[R776-CHAIN] ran %llu, total_instr=%llu pc=0x%08x fbp=%u fbw=%u quadwords_seen=%lu triangles_drawn=%lu\n",
           (unsigned long long)done, (unsigned long long)ee->instructions_executed, ee->pc, fbp, fbw,
           gif->quadwords_seen, gif->triangles_drawn);
    printf("[R776-CHAIN-GS] PMODE=0x%llx DISPFB1=0x%llx DISPFB2=0x%llx DISPLAY1=0x%llx DISPLAY2=0x%llx\n",
           (unsigned long long)gs->pmode, (unsigned long long)gs->dispfb1, (unsigned long long)gs->dispfb2,
           (unsigned long long)gs->display1, (unsigned long long)gs->display2);

    int W = 640, H = 224;
    FILE *f = fopen(out_ppm, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    long non_bg = 0;
    for (int yy = 0; yy < H; yy++) {
        for (int xx = 0; xx < W; xx++) {
            uint32_t px = gs_mem_read_psmct32(fbp, fbw, (uint32_t)xx, (uint32_t)yy);
            uint8_t r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF;
            if (px != 0) non_bg++;
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    }
    fclose(f);
    printf("[R776-CHAIN] non_zero_pixels=%ld / %d, dumped to %s\n", non_bg, W * H, out_ppm);

    if (checkpoint_save(ckpt_path) != 0) { fprintf(stderr, "checkpoint_save fail\n"); return 1; }
    printf("[R776-CHAIN] checkpoint saved to %s\n", ckpt_path);
    return 0;
}
