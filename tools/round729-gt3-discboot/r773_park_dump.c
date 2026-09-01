/*
 * Round 773 scratch: same as r772_fastboot_verify.c but on park-detection,
 * dumps a window of EE RAM around the resting PC to a flat file so it can
 * be fed to tools/round655-ee-disasm/disasm for real classification (is
 * this a genuine WaitSema/sync primitive vs. an error loop vs. something
 * else). Also dumps GS PMODE/DISPFB1/DISPLAY1/DISPFB2/DISPLAY2 per the
 * user's standing "GS/Display is Display2" reminder. Scratch only, never
 * committed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/hw/iop_cdvd.h"
#include "core/hw/gs.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <dump_out_path> [budget]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *dump_path = argv[3];
    uint64_t budget = argc > 4 ? strtoull(argv[4], NULL, 10) : 400000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);

    ee_state_t *ee = ee_core_get_state();

    uint64_t chunk = 10000000ull, done = 0;
    uint32_t last_pc = 0;
    int same_pc_streak = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        fprintf(stderr, "[R773] instr=%llu pc=0x%08x halted=%u\n",
                (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);
        fflush(stderr);
        if (ee->pc == last_pc) same_pc_streak++; else same_pc_streak = 0;
        last_pc = ee->pc;
        if (same_pc_streak >= 4) break;
    }

    uint32_t park_pc = ee->pc;
    uint32_t win_start = (park_pc >= 0x40) ? park_pc - 0x40 : 0;
    uint32_t win_len = 0x80;

    FILE *f = fopen(dump_path, "wb");
    if (f) {
        for (uint32_t off = 0; off < win_len; off++) {
            uint8_t b = ee_mem_read8(ee, win_start + off);
            fwrite(&b, 1, 1, f);
        }
        fclose(f);
        printf("[R773-DUMP] wrote %u bytes from 0x%08x to %s\n", win_len, win_start, dump_path);
    }
    printf("[R773-WINBASE] 0x%08x\n", win_start);
    printf("[R773-PARKPC] 0x%08x\n", park_pc);

    /* GS display state - explicit circuit 2 per user's standing reminder */
    gs_state_t *gsst = gs_get_state();
    printf("[R773-GS] PMODE=0x%016llx\n", (unsigned long long)gsst->pmode);
    printf("[R773-GS] DISPFB1=0x%016llx DISPLAY1=0x%016llx\n",
           (unsigned long long)gsst->dispfb1, (unsigned long long)gsst->display1);
    printf("[R773-GS] DISPFB2=0x%016llx DISPLAY2=0x%016llx\n",
           (unsigned long long)gsst->dispfb2, (unsigned long long)gsst->display2);

    printf("[R773-FINAL] instr=%llu pc=0x%08x halted=%u reason=%s\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
           ee->halt_reason[0] ? ee->halt_reason : "(none)");
    return 0;
}
