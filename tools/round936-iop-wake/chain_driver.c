/*
 * Round 936 (task #921, continuation of task #919/#920's Round 935 IOP
 * spurious-interrupt-ack fix): find out what real event, if any, wakes
 * GT3's IOP a second time after the Round 935 fix stops the storm and
 * lets the IOP go idle. Mirrors the established checkpoint-chaining
 * pattern (Round 715/729/382/383) plus the pre-existing R933_DMA_KICK_
 * TRACE / R933_RPCCALL_TRACE instrumentation (source/hw/dma.c,
 * source/core/ee/ee_core.c) and this round's new R936_IOP_WAKE_TRACE
 * (source/hw/iop_intc.c) to log every single real interrupt-raise call
 * (hardware or soft) with an instruction-counter timestamp, so we can
 * see directly whether ANYTHING fires after the single Round-935 ack.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_intc.h"
#include "core/hw/gs.h"
#include "core/hw/gif.h"
#include "core/hw/vu.h"
#include "core/hw/iop_cdvd.h"

unsigned long long g_r936_instr_counter = 0;

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path> <start|continue> [budget]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    const char *mode = argv[4];
    uint64_t budget = argc > 5 ? strtoull(argv[5], NULL, 10) : 500000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }

    if (strcmp(mode, "start") == 0) {
        if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
        if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
        iop_cdvd_set_disc_present(0x12);
    } else {
        if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }
    }

    ee_state_t  *ee  = ee_core_get_state();
    gs_state_t  *gs  = gs_get_state();
    gif_state_t *gif = gif_get_state();
    vu1_state_t *vu1 = vu1_get_state();

    uint64_t chunk = 1000000ull, done = 0;
    uint32_t last_iop_pc = 0xFFFFFFFFu;
    uint64_t last_report = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        g_r936_instr_counter = ee->instructions_executed;

        iop_state_t *iop = iop_core_get_state();
        if (iop->pc != last_iop_pc || done - last_report >= 10000000ull) {
            fprintf(stderr, "[R936PC] instr=%llu iop_pc=0x%08x (was 0x%08x) ee_pc=0x%08x\n",
                    (unsigned long long)done, iop->pc, last_iop_pc, ee->pc);
            last_iop_pc = iop->pc;
            last_report = done;
        }
    }

    int tid = ee_hle_thread_get_current_thread_id();
    iop_state_t *iop = iop_core_get_state();
    iop_intc_state_t *intc = iop_intc_get_state();
    printf("[R936-CHAIN] ran %llu more, total_instr=%llu ee_pc=0x%08x halted=%u tid=%d "
           "iop_pc=0x%08x istat=0x%08x imask=0x%08x istat_hi=0x%08x imask_hi=0x%08x "
           "vu1_instr=%llu gif_path1=%llu pmode=0x%02x dispfb1=0x%08x dispfb2=0x%08x\n",
           (unsigned long long)done, (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, tid,
           iop->pc, intc->istat, intc->imask, intc->istat_hi, intc->imask_hi,
           (unsigned long long)(vu1 ? vu1->instructions_executed : 0),
           (unsigned long long)(gif ? gif->gif_path1_transfers : 0),
           (unsigned)gs->pmode, (unsigned)gs->dispfb1, (unsigned)gs->dispfb2);

    if (ee->halted) {
        printf("[R936-CHAIN] EE halted: %s\n", ee->halt_reason);
        return 0;
    }

    if (checkpoint_save(ckpt_path) != 0) { fprintf(stderr, "checkpoint_save fail\n"); return 1; }
    printf("[R936-CHAIN] checkpoint saved to %s\n", ckpt_path);
    return 0;
}
