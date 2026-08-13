/*
 * Round 511: organic BIOS boot survey after implementing the real
 * SIF2 (IOP DMA channel 2) inbound IOP-RAM-to-EE-RAM transfer path
 * (source/hw/iop_dma.c: iop_dma_sif2_try_transfer()). Purely
 * empirical - no trampoline, just a long organic boot - to check:
 * (1) does real guest IOP code ever actually issue a SIF2 kick
 *     (iop_dma_get_sif2_transfer_count() > 0)?
 * (2) does DMAC_STAT bit 0x80 (SIF2 completion, the exact condition
 *     the OSDSYS 0x8000F768 wait loop checks per Rounds 265-271) ever
 *     get set?
 * (3) does the EE's PC ever leave the well-documented shared kernel
 *     idle-dispatch loop family?
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/dma.h"
#include "core/hw/iop_dma.h"
#include "core/hw/gs.h"

static bios_image_t bios;

int main(int argc, char **argv)
{
    const char *bios_path = argc > 1 ? argv[1] : "/tmp/round238_diag/bios_fresh.bin";
    uint64_t total_slices = argc > 2 ? strtoull(argv[2], NULL, 10) : 400000000ull;

    const char *err = NULL;
    if (bios_load(bios_path, &bios) != 0) {
        fprintf(stderr, "bios_load failed for %s\n", bios_path);
        return 1;
    }
    if (system_init(&bios, &bios) != 0) {
        fprintf(stderr, "system_init failed\n");
        return 1;
    }

    ee_state_t *ee = ee_core_get_state();
    dma_state_t *dma = dma_get_state();

    const int NUM_CHUNKS = 20;
    uint64_t chunk = total_slices / (uint64_t)NUM_CHUNKS;
    int halted = 0;

    for (int i = 0; i < NUM_CHUNKS && !halted; i++) {
        halted = system_run_interleaved(chunk);
        printf("[R511] chunk %2d: ee_pc=0x%08x halted=%d d_stat=0x%08x sif2_xfer_count=%u\n",
               i, ee->pc, halted, dma->d_stat, iop_dma_get_sif2_transfer_count());
        fflush(stdout);
        if (dma->d_stat & 0x80u) {
            printf("[R511]   *** SIF2 completion bit (DMAC_STAT bit 0x80) is SET ***\n");
        }
    }

    gs_state_t *gs = gs_get_state();
    printf("========== ROUND 511 SIF2 SURVEY RESULT ==========\n");
    printf("EE: halted=%d pc=0x%08x\n", halted, ee->pc);
    printf("DMAC_STAT (d_stat) = 0x%08x  (bit 0x80 SIF2 %s)\n",
           dma->d_stat, (dma->d_stat & 0x80u) ? "SET" : "clear");
    printf("iop_dma_get_sif2_transfer_count() = %u\n", iop_dma_get_sif2_transfer_count());
    printf("GS: PMODE=0x%016llx DISPFB1=0x%016llx DISPLAY1=0x%016llx DISPFB2=0x%016llx DISPLAY2=0x%016llx\n",
           (unsigned long long)gs->pmode, (unsigned long long)gs->dispfb1,
           (unsigned long long)gs->display1, (unsigned long long)gs->dispfb2,
           (unsigned long long)gs->display2);
    printf("===================================================\n");
    return 0;
}
