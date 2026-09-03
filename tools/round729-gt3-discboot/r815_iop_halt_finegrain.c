/*
 * Round 815 follow-up #2: fine-grained (single-IOP-step) trace of the
 * final instructions leading up to the "PC escaped to unfetchable
 * addr" halt discovered by r815_iop_halt_probe.c. Scratch-only, not
 * part of the tracked-repo mandatory-workflow diff - just reads
 * already-public iop_state_t fields every single-step call.
 *
 * Usage: r815_iop_halt_finegrain <bios> <disc> <ckpt_path> <fastfwd_budget>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"

#define RING 64

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <bios> <disc> <ckpt_path> <fastfwd_budget>\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    uint64_t fastfwd = strtoull(argv[4], NULL, 10);

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    /* fast-forward in big slices first */
    uint64_t done = 0;
    while (done < fastfwd && !iop->halted && !ee->halted) {
        system_run_interleaved(100000);
        done += 100000;
    }
    fprintf(stderr, "[FG] after fastfwd=%llu: iop_pc=0x%08x iop_halted=%u\n",
            (unsigned long long)done, iop->pc, iop->halted);
    if (iop->halted) {
        fprintf(stderr, "[FG] already halted before fine-grain phase - reduce fastfwd budget\n");
        return 0;
    }

    /* now single-step, keep a ring buffer of pc/ra/gpr for the last RING steps */
    uint32_t ring_pc[RING], ring_ra[RING], ring_sp[RING], ring_v0[RING], ring_v1[RING], ring_at[RING];
    int idx = 0, filled = 0;
    uint64_t steps = 0;
    const uint64_t MAX_FINE_STEPS = 2000000; /* safety cap */
    while (!iop->halted && !ee->halted && steps < MAX_FINE_STEPS) {
        ring_pc[idx] = iop->pc;
        ring_ra[idx] = iop->gpr[31];
        ring_sp[idx] = iop->gpr[29];
        ring_v0[idx] = iop->gpr[2];
        ring_v1[idx] = iop->gpr[3];
        ring_at[idx] = iop->gpr[1];
        idx = (idx + 1) % RING;
        if (idx == 0) filled = 1;
        system_run_interleaved(1);
        steps++;
    }
    fprintf(stderr, "[FG] stopped after %llu fine steps: iop_halted=%u iop_pc=0x%08x reason=\"%s\"\n",
            (unsigned long long)steps, iop->halted, iop->pc, iop->halt_reason);
    fprintf(stderr, "[FG] --- ring buffer (oldest to newest) ---\n");
    int count = filled ? RING : idx;
    int start = filled ? idx : 0;
    for (int i = 0; i < count; i++) {
        int j = (start + i) % RING;
        fprintf(stderr, "[FG] pc=0x%08x ra=0x%08x sp=0x%08x v0=0x%08x v1=0x%08x at=0x%08x\n",
                ring_pc[j], ring_ra[j], ring_sp[j], ring_v0[j], ring_v1[j], ring_at[j]);
    }
    return 0;
}
