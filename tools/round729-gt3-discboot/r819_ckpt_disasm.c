/*
 * Round 819 (task #828, per user's 5-point CD-command-dispatch-trace
 * directive): fast checkpoint-load RAM/register dump tool. Instead of
 * re-running a slow ~288M-instruction cold boot to inspect GT3's
 * resting state, this loads the Round 818 persisted checkpoint
 * (checkpoints/gt3_round818_steady_state_288332636instr.ckpt) directly
 * via checkpoint_load() and dumps: (1) the full EE HLE thread table,
 * (2) current EE pc/gpr, (3) a chosen EE RAM range to a flat file for
 * offline disassembly with tools/round655-ee-disasm/disasm.c.
 *
 * Never touches tracked source; purely a read-only diagnostic driver,
 * matching the r818_ramdump.c precedent. Checkpoint path is a
 * command-line argument so nothing BIOS/disc/checkpoint-derived is
 * hardcoded into this file itself (leak-check discipline).
 *
 * Usage: r819_ckpt_disasm <bios_path> <disc_path> <ckpt_path> <start_addr_hex> <len_hex> <out_file>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/hw/iop_cdvd.h"
#include "core/checkpoint.h"

int main(int argc, char **argv)
{
    if (argc < 7) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path> <start_addr_hex> <len_hex> <out_file>\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    uint32_t start_addr = (uint32_t)strtoul(argv[4], NULL, 16);
    uint32_t len = (uint32_t)strtoul(argv[5], NULL, 16);
    const char *out_file = argv[6];

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);

    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) {
        fprintf(stderr, "checkpoint_load FAILED for %s\n", ckpt_path);
        return 1;
    }

    ee_state_t *ee = ee_core_get_state();
    fprintf(stderr, "[R819-CKPT] loaded %s: total_instr=%llu pc=0x%08x halted=%d ncmd=%llu scmd=%llu\n",
            ckpt_path, (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
            (unsigned long long)iop_cdvd_get_ncmd_call_count(),
            (unsigned long long)iop_cdvd_get_scmd_call_count());

    for (int r = 0; r < 32; r++) {
        fprintf(stderr, "[R819-CKPT] gpr[%d]=0x%08x", r, (uint32_t)ee->gpr[r].ud0);
        if ((r % 4) == 3) fprintf(stderr, "\n"); else fprintf(stderr, "  ");
    }

    fprintf(stderr, "[R819-CKPT] current_thread_id=%d thread_count=%d\n",
            ee_hle_thread_get_current_thread_id(), ee_hle_thread_get_thread_count());
    int count = ee_hle_thread_get_thread_count();
    for (int t = 1; t <= count; t++) {
        fprintf(stderr, "[R819-CKPT] tid=%d status=0x%x wait_type=%u wait_id=%u saved_pc=0x%08x prio=%u\n",
                t, ee_hle_thread_get_status(t), ee_hle_thread_get_wait_type(t),
                ee_hle_thread_get_wait_id(t), ee_hle_thread_get_saved_pc(t), ee_hle_thread_get_priority(t));
    }

    fprintf(stderr, "[R819-CKPT] dumping 0x%08x len=0x%x to %s\n", start_addr, len, out_file);
    FILE *f = fopen(out_file, "wb");
    if (!f) { fprintf(stderr, "open fail\n"); return 1; }
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = ee_mem_read8(ee, start_addr + i);
        fputc(b, f);
    }
    fclose(f);
    return 0;
}
