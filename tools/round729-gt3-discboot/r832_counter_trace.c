/*
 * Round 832 (task #811): read out the compiled-in per-instruction hit
 * counters added to ee_core.c's ee_step() this round, to settle -
 * with zero sampling-gap risk - whether GT3's registered SIF0 DMAC
 * completion handler (0x0101D878) is ever actually reached during a
 * real cold boot.
 *
 * Round 831 proved the external per-slice PC-sampling method used by
 * r830_dmac_trace.c has a real methodology flaw: system_run_interleaved(1)
 * executes up to EE_IOP_STEP_RATIO (8) real EE instructions per call, so
 * a short function that both enters and returns inside one such batch is
 * invisible to exact-PC-equality polling from outside, even though it
 * demonstrably ran. This driver instead reads g_r832_* globals that are
 * incremented directly inside ee_step() - the real per-instruction step
 * function - so there is no sampling gap of any kind.
 *
 * Because these counters do not depend on external polling granularity,
 * this driver does NOT need to call system_run_interleaved(1) in a loop
 * (which required silencing its own diagnostic printf flood in
 * r830_dmac_trace.c) - a single system_run_interleaved(budget) call
 * suffices and is dramatically faster.
 *
 * Usage: r832_counter_trace <bios_path> <disc_path> [budget_slices]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/hw/iop_cdvd.h"
#include "core/hw/dma.h"

/* Declared non-static in ee_core.c (Round 832), not in any header -
 * this driver declares its own extern references, per the established
 * g_ee_null_jalr_guard_hits pattern. */
extern uint64_t g_r832_handler_hits;
extern uint64_t g_r832_guardfn_hits;
extern uint64_t g_r832_addcall_hits;
extern uint64_t g_r832_vector_hits;
extern uint64_t g_r832_bev_vector_hits;

#define GUARD_FLAG_ADDR 0x0103D3B8u

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> [budget_slices]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    uint64_t budget = argc > 3 ? strtoull(argv[3], NULL, 10) : 100000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12 /* CDVD_TYPE_PS2CD */);

    ee_state_t *ee = ee_core_get_state();
    dma_state_t *dma = dma_get_state();

    system_run_interleaved(budget);

    printf("[R832-COUNTER] total_instr=%llu pc=0x%08x halted=%u tid=%d\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
           ee_hle_thread_get_current_thread_id());
    printf("[R832-COUNTER] g_r832_handler_hits  (pc==0x0101D878, real SIF0 completion handler body) = %llu\n",
           (unsigned long long)g_r832_handler_hits);
    printf("[R832-COUNTER] g_r832_guardfn_hits  (pc==0x0101DA50, init-once guard fn)                 = %llu\n",
           (unsigned long long)g_r832_guardfn_hits);
    printf("[R832-COUNTER] g_r832_addcall_hits  (pc==0x0101D508, AddDmacHandler call site)            = %llu\n",
           (unsigned long long)g_r832_addcall_hits);
    printf("[R832-COUNTER] g_r832_vector_hits   (pc==0x80000200, real EE interrupt vector)            = %llu\n",
           (unsigned long long)g_r832_vector_hits);
    printf("[R832-COUNTER] g_r832_bev_vector_hits (pc==0xBFC00400, BEV boot-ROM vector)               = %llu\n",
           (unsigned long long)g_r832_bev_vector_hits);
    printf("[R832-COUNTER] once-flag global 0x%08x = 0x%08x (post-run)\n",
           GUARD_FLAG_ADDR, ee_mem_read32(ee, GUARD_FLAG_ADDR));
    printf("[R832-COUNTER] D_STAT ch5 status bit final=%d enable bit final=%d (d_stat=0x%08x)\n",
           (dma->d_stat & (1u << 5)) ? 1 : 0, (dma->d_stat & (1u << (16 + 5))) ? 1 : 0, dma->d_stat);
    printf("[R832-COUNTER] semaphore-5 signal count=%llu\n",
           (unsigned long long)ee_hle_thread_get_signal_calls(5));

    if (ee->halted) {
        printf("[R832-COUNTER] EE halted: %s\n", ee->halt_reason);
    }
    return 0;
}
