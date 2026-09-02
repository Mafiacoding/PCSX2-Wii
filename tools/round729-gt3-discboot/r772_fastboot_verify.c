/*
 * Round 772 verification driver (task #447, real fix) - v2, coarse
 * chunking (matches chain_driver.c's established 10,000,000-instr
 * slice convention; the first version's 1000-instr slices triggered
 * system_run_interleaved()'s own per-slice-cap debug print millions
 * of times and made the run I/O-bound instead of CPU-bound).
 *
 * Fresh cold boot against a real, user-provided disc image. Reports
 * pc/instr count after every 10,000,000-instruction chunk so the
 * run's trajectory can be read off directly: baseline (pre-fix)
 * behavior is a repeating ~29.5M-instruction OSDSYS self-relaunch
 * cycle (Round 771 finding) forever; the fix should either break that
 * cycle (pc advancing into real game-code territory, e.g. Tekken's
 * real epc=0x003572A0 per Round 554) or, if the BOOT2 patch/scan
 * didn't apply for some reason, leave the cycle unchanged (honest
 * negative). Scratch only - never committed/rsynced.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/hw/iop_cdvd.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> [budget]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    uint64_t budget = argc > 3 ? strtoull(argv[3], NULL, 10) : 300000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12 /* CDVD_TYPE_PS2CD */);

    ee_state_t *ee = ee_core_get_state();

    uint64_t chunk = 10000000ull, done = 0;
    uint32_t last_pc = 0;
    int same_pc_streak = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        fprintf(stderr, "[R772] instr=%llu pc=0x%08x halted=%u\n",
                (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);
        fflush(stderr);
        if (ee->pc == last_pc) same_pc_streak++; else same_pc_streak = 0;
        last_pc = ee->pc;
        if (same_pc_streak >= 4) {
            fprintf(stderr, "[R772] pc unchanged for 4+ consecutive chunks - likely parked, stopping early\n");
            break;
        }
    }

    printf("[R772-FINAL] instr=%llu pc=0x%08x halted=%u reason=%s\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
           ee->halt_reason[0] ? ee->halt_reason : "(none)");
    return 0;
}
