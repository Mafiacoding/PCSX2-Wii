/*
 * Round 818 (task #824 continuation): scratch RAM-dump helper, same
 * cold-boot pattern as r818_sema_producer.c. Runs to the same
 * instruction budget, then dumps a chosen EE virtual-address range to
 * a flat file so it can be disassembled offline with the existing
 * Round 655 disassembler (tools/round655-ee-disasm/disasm.c) - purely
 * to identify the real caller/context around the CreateSema/SignalSema
 * call sites this round's R818_SEMA_TRACE instrumentation captured.
 * Never committed as part of the tracked build; disposable diagnostic.
 *
 * Usage: r818_ramdump <bios_path> <disc_path> <budget> <start_addr_hex> <len_hex> <out_file>
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
    if (argc < 7) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <budget> <start_addr_hex> <len_hex> <out_file>\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    uint64_t budget = strtoull(argv[3], NULL, 10);
    uint32_t start_addr = (uint32_t)strtoul(argv[4], NULL, 16);
    uint32_t len = (uint32_t)strtoul(argv[5], NULL, 16);
    const char *out_file = argv[6];

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);

    ee_state_t *ee = ee_core_get_state();
    const long SLICE = 1000000;
    uint64_t done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(SLICE);
        done += (uint64_t)SLICE;
    }
    fprintf(stderr, "[R818-RAMDUMP] total_instr=%llu pc=0x%08x dumping 0x%08x len=0x%x to %s\n",
            (unsigned long long)ee->instructions_executed, ee->pc, start_addr, len, out_file);

    FILE *f = fopen(out_file, "wb");
    if (!f) { fprintf(stderr, "open fail\n"); return 1; }
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = ee_mem_read8(ee, start_addr + i);
        fputc(b, f);
    }
    fclose(f);
    return 0;
}
