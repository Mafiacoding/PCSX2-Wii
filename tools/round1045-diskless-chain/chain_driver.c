/*
 * Round 1045 (task #447/#536/#1009 continuation): checkpoint-chained
 * SCPH-50004 diskless boot survey, extending the R933_RPCCALL_TRACE/
 * R1036_REG_TRACE correlated methodology (Rounds 1038-1044) past the
 * single-tool-call ~90-120M-instruction wall documented in Round
 * 1044. Directly modeled on tools/round939-diskless-pmode/
 * chain_driver.c's established start/continue checkpoint pattern
 * (itself following Round 715/729/382/383/936's precedent) - each
 * invocation runs a bounded chunk budget and checkpoints, so a shell
 * loop of many short-lived process invocations can accumulate a much
 * larger total instruction count than any single process's wall-clock
 * budget allows.
 *
 * This driver is diskless (system_init() only, no iop_cdvd_mount_iso()
 * call), matching main.c's own no-disc fallback and every prior round
 * in this SCPH-50004 diskless-boot investigation arc.
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

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <ckpt_path> <start|continue> [budget]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *ckpt_path = argv[2];
    const char *mode = argc > 3 ? argv[3] : "start";
    uint64_t budget = argc > 4 ? strtoull(argv[4], NULL, 10) : 60000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }

    if (strcmp(mode, "start") == 0) {
        if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    } else {
        if (checkpoint_load(ckpt_path, &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }
    }

    ee_state_t *ee = ee_core_get_state();

    uint64_t chunk = 5000000ull, done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
    }

    int tid = ee_hle_thread_get_current_thread_id();
    iop_state_t *iop = iop_core_get_state();
    printf("[R1045-CHAIN] ran %llu more, total_instr=%llu ee_pc=0x%08x halted=%u tid=%d iop_pc=0x%08x\n",
           (unsigned long long)done, (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, tid,
           iop->pc);

    if (ee->halted) {
        printf("[R1045-CHAIN] EE halted: %s\n", ee->halt_reason);
        return 0;
    }

    if (checkpoint_save(ckpt_path) != 0) { fprintf(stderr, "checkpoint_save fail\n"); return 1; }
    printf("[R1045-CHAIN] checkpoint saved to %s\n", ckpt_path);
    return 0;
}
