/*
 * Round 836 (task #811/#854, per user's "1 dann 2 dann 3" step 2):
 * fresh cold-boot, single-slice-granularity trace that stops the
 * INSTANT g_ee_null_jalr_guard_hits (Round 630/782's pre-existing
 * counter) first transitions away from zero, and dumps the full
 * register file plus which of the guard's two sub-branches fired
 * (bounce-to-$ra vs both-zero-exit) at that exact moment - the
 * earliest possible ground truth for how/why GT3's thread 3 first
 * hit pc==0, rather than the already-dead replay state every prior
 * checkpoint (Round 826's 58.5M-instr save included) captures.
 *
 * Silences system_run_interleaved's own per-call diagnostic printf
 * during the stepping loop (same technique as r830_dmac_trace.c),
 * restored before this driver's own output.
 *
 * Usage: r836_first_null_jalr <bios_path> <disc_path> [max_slices]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/hw/iop_cdvd.h"

extern long g_ee_null_jalr_guard_hits;

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> [max_slices]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    uint64_t max_slices = argc > 3 ? strtoull(argv[3], NULL, 10) : 10000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);

    ee_state_t *ee = ee_core_get_state();

    int saved_stdout = dup(1);
    int devnull = open("/dev/null", O_WRONLY);

    uint32_t prev_pc = ee->pc;
    long prev_guard = g_ee_null_jalr_guard_hits;
    uint64_t slice;
    int found = 0;
    for (slice = 0; slice < max_slices && !ee->halted; slice++) {
        uint32_t pc_before = ee->pc;

        if (devnull >= 0) dup2(devnull, 1);
        system_run_interleaved(1);
        if (saved_stdout >= 0) dup2(saved_stdout, 1);

        long g_now = g_ee_null_jalr_guard_hits;
        if (g_now != prev_guard) {
            found = 1;
            printf("[R836] FIRST GUARD HIT at slice=%llu total_instr=%llu\n",
                   (unsigned long long)slice, (unsigned long long)ee->instructions_executed);
            printf("[R836] pc_before_slice=0x%08x pc_after_slice=0x%08x tid=%d\n",
                   pc_before, ee->pc, ee_hle_thread_get_current_thread_id());
            for (int r = 0; r < 32; r++) {
                printf("  gpr[%2d]=0x%08x", r, (uint32_t)ee->gpr[r].ud0);
                if ((r % 4) == 3) printf("\n");
            }
            printf("[R836] cop0_status=0x%08x cop0_cause=0x%08x cop0_epc=0x%08x\n",
                   ee->cop0[12], ee->cop0[13], ee->cop0[14]);
            break;
        }
        prev_guard = g_now;
        (void)prev_pc;
        prev_pc = pc_before;
    }
    if (devnull >= 0) close(devnull);

    if (!found) {
        printf("[R836] guard never fired within %llu slices; total_instr=%llu pc=0x%08x halted=%u\n",
               (unsigned long long)max_slices, (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);
    }
    return 0;
}
