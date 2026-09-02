/*
 * Round 715 (task #693/#694): dump a fixed EE-RAM code region to a raw
 * file so it can be run through tools/round655-ee-disasm/disasm.c with
 * base=0x80000000 (KSEG0 virtual == physical offset 0 for RAM). Used
 * to re-examine the real TIMER3 Alarm dispatcher at 0x80002650
 * (Round 622/628/629's own already-decoded function) for a possible
 * missing/misdecoded conditional branch around the unconditional
 * pending_count-- at 0x80002708 - Round 629 Finding 3 proved the
 * empty-table condition itself is real/expected (live PCSX2 shows the
 * same state deep into real gameplay without crashing), so if real
 * hardware doesn't crash on pending_count==0, the real ROM code must
 * guard the decrement/dispatch somewhere our own decode may have
 * missed.
 */
#include <stdio.h>
#include <stdlib.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"

int main(int argc, char **argv)
{
    const char *bios_path = argc > 1 ? argv[1] : "/tmp/r708/bios.bin";
    uint64_t budget = argc > 2 ? strtoull(argv[2], NULL, 10) : 60000000ull;
    uint32_t dump_start = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 16) : 0x80002600u;
    uint32_t dump_len = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 16) : 0x300u;
    const char *out_path = argc > 5 ? argv[5] : "/tmp/r715_ramdump.bin";

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }

    uint64_t chunk = 2000000ull, done = 0;
    while (done < budget) {
        system_run_interleaved(chunk);
        done += chunk;
    }

    ee_state_t *st = ee_core_get_state();
    FILE *out = fopen(out_path, "wb");
    if (!out) { fprintf(stderr, "cannot open %s\n", out_path); return 1; }
    uint32_t phys = dump_start - 0x80000000u;
    fwrite(st->ram + phys, 1, dump_len, out);
    fclose(out);
    printf("[R715-RAMDUMP] dumped %u bytes from 0x%08X to %s after %llu instructions\n",
           dump_len, dump_start, out_path, (unsigned long long)done);
    return 0;
}
