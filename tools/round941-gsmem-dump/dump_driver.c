/*
 * Round 941 follow-up (task #925/#926 continuation): the user ran the
 * Round 941 fixed build (PMODE=0x02 Circuit-2-only) on real Dolphin
 * and reported real progress - sparse colored pixels/"stars" now
 * visible on the previously solid-black screen, while Dolphin's own
 * D3D12 "Draw calls" stat stayed at 0. That stat staying 0 is
 * EXPECTED and not a contradiction: this project's real Wii blit path
 * (gs_blit_psmct32_to_xfb(), called from main.c's run_real_boot_flow())
 * writes pixels directly into the Wii's XFB via CPU/DCFlushRange -
 * it never goes through GX draw calls, so Dolphin's GX statistics
 * overlay legitimately has nothing to count regardless of whether
 * real pixels are produced.
 *
 * This driver reproduces the Round 941 diskless-boot scenario
 * host-natively (system_init(), no disc mount - same as main.c's
 * diskless fallback) out past the 25,000,000-instruction forced-
 * display threshold, then dumps DISPFB2's actual GS-local-memory
 * content (FBP=0, FBW=10*64=640px, 640x448 area) to characterize
 * what real content (if any) is now reaching the blit path - i.e.
 * to find out what the user's "stars" actually are: genuine sparse
 * GS-memory writes vs. some other explanation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/hw/gs.h"
#include "core/hw/gs_mem.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <bios_path> [budget]\n", argv[0]); return 1; }
    const char *bios_path = argv[1];
    uint64_t budget = argc > 2 ? strtoull(argv[2], NULL, 10) : 40000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    gs_init();
    gs_mem_init();

    ee_state_t *ee = ee_core_get_state();
    gs_state_t *gs = gs_get_state();

    uint64_t chunk = 2000000ull, done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        if (gs->pmode != 0) {
            fprintf(stderr, "[R941DUMP] pmode went nonzero at instr=%llu (pmode=0x%02x)\n",
                    (unsigned long long)done, (unsigned)gs->pmode);
            break;
        }
    }

    printf("[R941DUMP] final: instr=%llu halted=%u pmode=0x%02x dispfb2=0x%08x display2=0x%016llx\n",
           (unsigned long long)ee->instructions_executed, ee->halted, (unsigned)gs->pmode,
           (unsigned)gs->dispfb2, (unsigned long long)gs->display2);

    if (gs->pmode == 0) {
        printf("[R941DUMP] pmode never went nonzero within budget - forced hook didn't fire (budget too low?)\n");
        return 0;
    }

    /* Decode DISPFB2 the same way main.c/gs_wii_output.c does. */
    uint32_t fbp_field = (uint32_t)(gs->dispfb2 & 0x1FFu);
    uint32_t fbw_field  = (uint32_t)((gs->dispfb2 >> 9) & 0x3Fu);
    uint32_t bp_words = fbp_field * 2048u;
    uint32_t bw_pixels = fbw_field * 64u;
    printf("[R941DUMP] decoded: bp_words=%u bw_pixels=%u\n", bp_words, bw_pixels);

    uint32_t width = bw_pixels, height = 448;
    uint64_t nonzero = 0, total = (uint64_t)width * height;
    uint32_t sample_count = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t v = gs_mem_read_psmct32(bp_words, bw_pixels, x, y);
            if (v != 0) {
                nonzero++;
                if (sample_count < 20) {
                    printf("[R941DUMP] nonzero pixel #%u at (x=%u,y=%u) = 0x%08x\n",
                           sample_count, x, y, v);
                    sample_count++;
                }
            }
        }
    }
    printf("[R941DUMP] total pixels=%llu nonzero=%llu (%.4f%%)\n",
           (unsigned long long)total, (unsigned long long)nonzero,
           total ? (100.0 * (double)nonzero / (double)total) : 0.0);

    return 0;
}
