/*
 * Round 778 (task #791 continuation): dump key EE GPRs from the live
 * Metal Slug 3 checkpoint, to check whether the decompression-loop
 * state discovered at 0x00100B60-0x00100D00 (s0=dst write ptr,
 * s1=outer counter, s2=dst base, s3=ctx2 base) is actually converging
 * toward completion across the checkpoint chain, or spinning without
 * forward progress. Companion to r777_kof_ramdump.c/r777_disasm -
 * answers "is this loop real progress or a stuck/non-terminating spin"
 * by sampling the same registers at different total_instr depths.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"

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
    printf("[R778-REGDUMP] total_instr=%llu pc=0x%08x halted=%u\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);
    for (int r = 16; r <= 19; r++) {
        printf("[R778-REGDUMP] $%s = 0x%016llx\n", rname[r], (unsigned long long)ee->gpr[r].ud0);
    }
    printf("[R778-REGDUMP] $a2=0x%016llx $a3=0x%016llx\n",
           (unsigned long long)ee->gpr[6].ud0, (unsigned long long)ee->gpr[7].ud0);
    /* ctx2 fields relative to s3-0x3698 (0xC948 as signed 16-bit = -14008 = -0x36B8) */
    uint32_t s3 = (uint32_t)ee->gpr[19].ud0;
    uint32_t ctx = s3 - 14008u;
    uint32_t ctx_phys = ctx & 0x1FFFFFFF;
    if (ctx_phys + 24 <= 32*1024*1024) {
        uint32_t *w = (uint32_t *)(ee->ram + ctx_phys);
        printf("[R778-REGDUMP] ctx(s3-14008)=0x%08x: +0=0x%08x +4=0x%08x +8=0x%08x +C=0x%08x +10=0x%08x +14=0x%08x\n",
               ctx, w[0], w[1], w[2], w[3], w[4], w[5]);
    } else {
        printf("[R778-REGDUMP] ctx=0x%08x out of RAM range (phys 0x%08x)\n", ctx, ctx_phys);
    }
    return 0;
}
