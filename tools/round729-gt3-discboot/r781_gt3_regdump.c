/*
 * Round 781 (task #800/#801, GT3 0x0101bc24 stall investigation):
 * generic EE GPR + INTC-register dump for the live GT3 checkpoint.
 * Companion to r778_ms3_regdump.c (same pattern, generalized) - this
 * round's disassembly (tools/round655-ee-disasm) showed pc=0x0101bc24
 * sitting on a `syscall` instruction immediately preceded by
 * `addiu v1, zero, 68` (WaitSema), so this tool's job is to read the
 * real $a0 (semaphore id) WaitSema was called with, and the EE
 * interrupt-controller register state, to determine whether this is a
 * genuine, currently-never-signaled wait (real gap) or a transient one
 * this project's own interrupt-park mechanism (see ee_core.c's
 * sysnum==68 handler) should already be resolving.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/hw/ee_intc.h"

static const char *rname[32] = {
    "zr","at","v0","v1","a0","a1","a2","a3",
    "t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7",
    "t8","t9","k0","k1","gp","sp","fp","ra"
};

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <ckpt_path>\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *ckpt_path = argv[2];

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(ckpt_path, &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    printf("[R781-REGDUMP] total_instr=%llu pc=0x%08x halted=%u cop0.Status=0x%08x cop0.Cause=0x%08x\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
           ee->cop0[12], ee->cop0[13]);
    for (int r = 0; r < 32; r++) {
        printf("[R781-REGDUMP] $%-3s = 0x%016llx\n", rname[r], (unsigned long long)ee->gpr[r].ud0);
    }
    ee_intc_state_t *intc = ee_intc_get_state();
    printf("[R781-REGDUMP] EE_INTC stat=0x%08x mask=0x%08x pending=%d\n",
           intc->stat, intc->mask, ee_intc_pending());
    return 0;
}
