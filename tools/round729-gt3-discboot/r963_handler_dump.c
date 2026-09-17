/* Round 963 (task #887/937/938, user-directed: "geh diese module an und
 * disassemblier sie vielleicht steckt dort die loesung" - go after
 * these modules and disassemble them, maybe the solution is in there):
 * dumps raw IOP RAM windows around the 6 real interrupt-handler
 * addresses Round 962 empirically confirmed are registered during a
 * genuine fresh cold boot (system_init(), SCPH-50004 BIOS + GT3, NO
 * checkpoint) - irq=0(VBLANK_START)=0x00012274, irq=2=0x00130cc0,
 * irq=11(VBLANK_END)=0x0001232c, irq=16=0x0011a4d0, irq=42=0x00016c64,
 * irq=43=0x00018290 - so tools/round655-ee-disasm/disasm.c (existing
 * R5900/MIPS-I decoder, reused for IOP/R3000A per Round 944/961
 * precedent) can turn them into real mnemonics for module attribution.
 *
 * Method: same fresh-boot polling loop as r962_fresh_intr_trace.c,
 * stopping as soon as all 6 target irqs have a nonzero handler (or at
 * a safety budget), then dumping a generous window (-16 bytes before,
 * +1024 bytes after) around each address to its own raw binary file
 * under /tmp for offline disassembly this round. Read-only, no
 * tracked-source changes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_hle_intr.h"
#include "core/hw/iop_cdvd.h"

static const int TARGET_IRQ[6] = {0, 2, 11, 16, 42, 43};
static const char *TARGET_NAME[6] = {
    "irq00_VBLANK_START", "irq02", "irq11_VBLANK_END", "irq16", "irq42", "irq43"
};

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios> <disc> [budget_slices] [outdir]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    uint64_t budget = (argc >= 4) ? strtoull(argv[3], NULL, 10) : 150000000ull;
    const char *outdir = (argc >= 5) ? argv[4] : "/tmp";

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);

    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    uint64_t done = 0;
    const uint64_t CHUNK = 10000000ull;
    int all_found = 0;
    while (done < budget && !ee->halted && !iop->halted) {
        system_run_interleaved(CHUNK);
        done += CHUNK;
        int cnt = 0;
        for (int i = 0; i < 6; i++)
            if (iop_hle_intr_get_intr_handler(TARGET_IRQ[i]) != 0) cnt++;
        fprintf(stderr, "[R963] slices=%llu ee_instr=%llu found=%d/6\n",
                (unsigned long long)done, (unsigned long long)ee->instructions_executed, cnt);
        if (cnt == 6) { all_found = 1; break; }
    }

    fprintf(stderr, "\n[R963] all_found=%d after slices=%llu ee_instr=%llu\n",
            all_found, (unsigned long long)done, (unsigned long long)ee->instructions_executed);

    for (int i = 0; i < 6; i++) {
        uint32_t h = iop_hle_intr_get_intr_handler(TARGET_IRQ[i]);
        fprintf(stderr, "[R963] irq=%d (%s) handler=0x%08x\n", TARGET_IRQ[i], TARGET_NAME[i], h);
        if (h == 0) { fprintf(stderr, "[R963]   -> skip dump, handler still 0\n"); continue; }
        uint32_t start = (h >= 16) ? (h - 16) : 0;
        uint32_t len = 1040; /* -16 .. +1024 */
        char path[512];
        snprintf(path, sizeof(path), "%s/r963_%s_0x%08x.bin", outdir, TARGET_NAME[i], h);
        FILE *f = fopen(path, "wb");
        if (!f) { fprintf(stderr, "[R963]   open fail: %s\n", path); continue; }
        for (uint32_t a = start; a < start + len; a += 4) {
            uint32_t w = iop_mem_read32(iop, a);
            fwrite(&w, 4, 1, f);
        }
        fclose(f);
        fprintf(stderr, "[R963]   dumped 0x%08x..0x%08x (%u bytes) -> %s\n",
                start, start + len, len, path);
    }
    return 0;
}
