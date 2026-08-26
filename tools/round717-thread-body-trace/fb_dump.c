/*
 * Round 717 (task #701): organic (no forced injection) framebuffer
 * dump - runs the real diskless JP-BIOS boot to a given instruction
 * budget and dumps whatever is genuinely in GS local memory at
 * fbp/fbw (DISPFB2's real configured values, per Round 321's own
 * finding that real OSDSYS uses GS circuit 2) to a PPM, mirroring
 * Round 450/588's own dump convention but WITHOUT injecting any
 * synthetic packet - this is meant to show exactly what the current
 * tree's organic boot produces on its own, to answer "is a real
 * picture already forming" now that Round 716 confirmed the wake
 * mechanism works and Round 717's own thread-trace showed GIF
 * quadwords_seen climbing continuously.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/hw/gs.h"
#include "core/hw/gs_mem.h"
#include "core/hw/gif.h"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *bios_path = argc > 1 ? argv[1] : "/tmp/r708/bios.bin";
    uint64_t budget = argc > 2 ? strtoull(argv[2], NULL, 10) : 130000000ull;
    const char *out_path = argc > 3 ? argv[3] : "/tmp/r717_organic.ppm";

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { printf("[FAIL] bios load\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { printf("[FAIL] system_init\n"); return 1; }

    uint64_t chunk = 4000000ull, done = 0;
    while (done < budget) { system_run_interleaved(chunk); done += chunk; }

    gif_state_t *gif = gif_get_state();
    gs_state_t *gs = gs_get_state();
    uint32_t fbp = gif->fbp, fbw = gif->fbw ? gif->fbw : 640u;
    printf("[R717-FB] after %llu instructions: fbp=%u fbw=%u quadwords_seen=%lu triangles_drawn=%lu\n",
           (unsigned long long)done, fbp, fbw, gif->quadwords_seen, gif->triangles_drawn);
    printf("[R717-FB] PMODE=0x%llx DISPFB1=0x%llx DISPFB2=0x%llx DISPLAY1=0x%llx DISPLAY2=0x%llx\n",
           (unsigned long long)gs->pmode, (unsigned long long)gs->dispfb1,
           (unsigned long long)gs->dispfb2, (unsigned long long)gs->display1,
           (unsigned long long)gs->display2);

    int W = 640, H = 224;
    FILE *f = fopen(out_path, "wb");
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
    printf("[R717-FB] non_zero_pixels=%ld / %d, dumped to %s\n", non_bg, W * H, out_path);
    return 0;
}
