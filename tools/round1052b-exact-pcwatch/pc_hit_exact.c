/*
 * Round 1052b (task #447/#536/#1051 continuation): exact, non-sampled
 * execution-based confirmation of whether the real Sony Reschedule()/
 * GetHighestReadyPriority()/enclosing-dispatcher region found in Round
 * 1051 (IOP addresses 0x00115800-0x00115E00) is EVER actually fetched
 * during real boot execution.
 *
 * Round 1052's first attempt (tools/round1052-live-exec-check/
 * pc_hit_check.c) sampled iop->pc only once per 100000-instruction
 * chunk boundary, which cannot reliably detect a brief visit to a
 * narrow (~1536-byte) code region entirely contained within one
 * chunk. That run reported "no hits" but was flagged as inconclusive
 * due to this sampling gap.
 *
 * This driver instead links against a scratch copy of system.c
 * (system_r1052b.c) that calls r1052b_note_pc(iop->pc) after EVERY
 * single iop_core_step() call inside system_run_interleaved() - i.e.
 * every real IOP instruction, with zero sampling gap. r1052b_note_pc()
 * (defined here) increments a per-address hit counter whenever the pc
 * falls in the target range. This gives an exact, not probabilistic,
 * answer.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"

#define RANGE_LO 0x00115800u
#define RANGE_HI 0x00115E00u

static uint32_t hitcount[(RANGE_HI - RANGE_LO) / 4];
static uint64_t total_checked = 0;
static uint64_t total_hits = 0;

void r1052b_note_pc(uint32_t pc)
{
    total_checked++;
    if (pc >= RANGE_LO && pc < RANGE_HI) {
        hitcount[(pc - RANGE_LO) / 4]++;
        total_hits++;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <ckpt_path> [budget]\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(argv[2], &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    uint64_t budget = argc > 3 ? strtoull(argv[3], NULL, 10) : 30000000ull;

    printf("[R1052B] starting exact per-instruction pc-watch, budget=%llu, range=0x%08x-0x%08x\n",
           (unsigned long long)budget, RANGE_LO, RANGE_HI);

    /* run in modest chunks so we still get periodic status output,
     * but the per-instruction check happens inside system.c regardless
     * of chunk size - chunk size here only affects print frequency. */
    uint64_t chunk = 5000000ull, done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        printf("[R1052B] progress: instr_done=%llu total_checked=%llu total_hits=%llu ee_pc=0x%08x\n",
               (unsigned long long)done, (unsigned long long)total_checked,
               (unsigned long long)total_hits, ee->pc);
    }

    int any = 0;
    for (unsigned i = 0; i < (RANGE_HI - RANGE_LO) / 4; i++) {
        if (hitcount[i]) { printf("[R1052B-HIT] pc=0x%08x hits=%u\n", RANGE_LO + i * 4, hitcount[i]); any = 1; }
    }
    if (!any) {
        printf("[R1052B-RESULT] EXACT (non-sampled) check: ZERO hits in 0x%08x-0x%08x across %llu real IOP instructions checked one-by-one.\n",
               RANGE_LO, RANGE_HI, (unsigned long long)total_checked);
    } else {
        printf("[R1052B-RESULT] EXACT (non-sampled) check: %llu total hits across %llu real IOP instructions checked one-by-one.\n",
               (unsigned long long)total_hits, (unsigned long long)total_checked);
    }
    return 0;
}
