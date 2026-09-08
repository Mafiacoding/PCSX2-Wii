/*
 * Round 832 (task #811) checkpoint-resuming variant of
 * r832_counter_trace.c. A fresh cold boot only reaches
 * pc=0x8000e538 (still pure BIOS kernel code) after 800,000,000
 * EE instructions (100,000,000 interleave slices) - nowhere near
 * GT3's own code space (0x0101xxxx), where the SIF0 AddDmacHandler
 * init and its registered completion handler (0x0101D878) would
 * even have a chance to run. Rather than re-spend that budget again
 * (GT3's own established chain already reached far deeper: see
 * checkpoints/gt3_round825_steady_state_1170824645instr.ckpt, saved
 * post-Round-824 scheduler fix), this driver resumes that persisted
 * checkpoint and extends it forward while watching the Round 832
 * compiled-in counters (g_r832_handler_hits and siblings), so the
 * question gets answered from genuinely deep-into-GT3 execution
 * rather than from early BIOS boot.
 *
 * Usage: r832_counter_chain <bios_path> <disc_path> <ckpt_path> [budget_slices]
 * Never overwrites the input checkpoint - if progress is made and not
 * halted, saves to <ckpt_path>.r832next (caller decides whether to keep).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/hw/iop_cdvd.h"
#include "core/hw/dma.h"

extern uint64_t g_r832_handler_hits;
extern uint64_t g_r832_guardfn_hits;
extern uint64_t g_r832_addcall_hits;
extern uint64_t g_r832_vector_hits;
extern uint64_t g_r832_bev_vector_hits;

#define GUARD_FLAG_ADDR 0x0103D3B8u

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path> [budget_slices]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    uint64_t budget = argc > 4 ? strtoull(argv[4], NULL, 10) : 200000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    dma_state_t *dma = dma_get_state();

    uint64_t start_instr = ee->instructions_executed;
    printf("[R832-CHAIN] resumed checkpoint at total_instr=%llu pc=0x%08x\n",
           (unsigned long long)start_instr, ee->pc);
    printf("[R832-CHAIN] counters at resume: handler=%llu guardfn=%llu addcall=%llu vector=%llu bev=%llu\n",
           (unsigned long long)g_r832_handler_hits, (unsigned long long)g_r832_guardfn_hits,
           (unsigned long long)g_r832_addcall_hits, (unsigned long long)g_r832_vector_hits,
           (unsigned long long)g_r832_bev_vector_hits);

    uint64_t chunk = 10000000ull, done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
    }

    int tid = ee_hle_thread_get_current_thread_id();
    printf("[R832-CHAIN] ran %llu more slices, total_instr=%llu (delta=%llu) pc=0x%08x halted=%u tid=%d\n",
           (unsigned long long)done, (unsigned long long)ee->instructions_executed,
           (unsigned long long)(ee->instructions_executed - start_instr), ee->pc, ee->halted, tid);
    printf("[R832-COUNTER] g_r832_handler_hits  (pc==0x0101D878) = %llu\n", (unsigned long long)g_r832_handler_hits);
    printf("[R832-COUNTER] g_r832_guardfn_hits  (pc==0x0101DA50) = %llu\n", (unsigned long long)g_r832_guardfn_hits);
    printf("[R832-COUNTER] g_r832_addcall_hits  (pc==0x0101D508) = %llu\n", (unsigned long long)g_r832_addcall_hits);
    printf("[R832-COUNTER] g_r832_vector_hits   (pc==0x80000200) = %llu\n", (unsigned long long)g_r832_vector_hits);
    printf("[R832-COUNTER] g_r832_bev_vector_hits (pc==0xBFC00400) = %llu\n", (unsigned long long)g_r832_bev_vector_hits);
    printf("[R832-COUNTER] once-flag global 0x%08x = 0x%08x (post-run)\n",
           GUARD_FLAG_ADDR, ee_mem_read32(ee, GUARD_FLAG_ADDR));
    printf("[R832-COUNTER] D_STAT ch5 status bit final=%d enable bit final=%d (d_stat=0x%08x)\n",
           (dma->d_stat & (1u << 5)) ? 1 : 0, (dma->d_stat & (1u << (16 + 5))) ? 1 : 0, dma->d_stat);
    printf("[R832-COUNTER] semaphore-5 signal count=%llu\n", (unsigned long long)ee_hle_thread_get_signal_calls(5));

    if (ee->halted) {
        printf("[R832-CHAIN] EE halted: %s\n", ee->halt_reason);
        return 0;
    }

    char outpath[1024];
    snprintf(outpath, sizeof(outpath), "%s.r832next", ckpt_path);
    if (checkpoint_save(outpath) != 0) { fprintf(stderr, "checkpoint_save fail\n"); return 1; }
    printf("[R832-CHAIN] checkpoint saved to %s\n", outpath);
    return 0;
}
