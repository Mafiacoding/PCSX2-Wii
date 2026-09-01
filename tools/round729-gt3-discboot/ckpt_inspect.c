/*
 * Round 764 follow-up (task #760): scratch inspector for the GT3
 * checkpoint's current stall - loads a checkpoint, dumps the IOP RAM
 * window around the stalled pc plus the EE RAM window around its own
 * stalled pc to flat files for disassembly, and prints the exact
 * register/hardware state (GPRs, SIF mailbox/flag registers, IOP
 * INTC state) needed to determine what condition the loop is actually
 * waiting on, rather than continuing to blindly grow the chain.
 * Never committed - scratch diagnostic only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/sif.h"
#include "core/hw/iop_intc.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path>\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(argv[3], &bios, &bios, argv[2]) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    printf("=== EE state ===\n");
    printf("pc=0x%08x next_pc=0x%08x halted=%u\n", ee->pc, ee->next_pc, ee->halted);
    for (int i = 0; i < 32; i++) printf("  gpr[%2d]=0x%08x%s", i, (uint32_t)ee->gpr[i].ud0, (i%4==3)?"\n":" ");
    printf("\n");

    printf("=== IOP state ===\n");
    printf("pc=0x%08x next_pc=0x%08x halted=%u\n", iop->pc, iop->next_pc, iop->halted);
    for (int i = 0; i < 32; i++) printf("  gpr[%2d]=0x%08x%s", i, iop->gpr[i], (i%4==3)?"\n":" ");
    printf("\n");

    printf("=== SIF registers (real hw 0x1000F200/F210/F230) ===\n");
    uint32_t mscom, smcom, smflag;
    sif_mmio_read32(0x1000F200u, &mscom);
    sif_mmio_read32(0x1000F210u, &smcom);
    sif_mmio_read32(0x1000F230u, &smflag);
    printf("SIF_MSCOM=0x%08x SIF_SMCOM=0x%08x SIF_SMFLAG=0x%08x\n", mscom, smcom, smflag);

    iop_intc_state_t *intc = iop_intc_get_state();
    printf("=== IOP INTC state ===\n");
    printf("istat=0x%08x imask=0x%08x ictrl=0x%08x\n", intc->istat, intc->imask, intc->ictrl);

    /* Dump IOP RAM window around the stalled pc for disassembly. */
    FILE *f = fopen("/tmp/iop_dump.bin", "wb");
    fwrite(iop->ram + 0x00000B00u, 1, 0x300, f);
    fclose(f);
    printf("wrote /tmp/iop_dump.bin (IOP RAM 0x00000B00-0x00000E00)\n");

    /* Dump EE RAM window around the stalled pc for disassembly
     * (0x80005D00-0x80006400, physical offset = virtual & 0x1FFFFFFF). */
    f = fopen("/tmp/ee_dump.bin", "wb");
    fwrite(ee->ram + (0x80005D00u & 0x1FFFFFFFu), 1, 0x700, f);
    fclose(f);
    printf("wrote /tmp/ee_dump.bin (EE RAM 0x80005D00-0x80006400)\n");

    /* Round 771 follow-up: dump a wider window covering the new
     * post-SIFX-fix EE resting pc (0x8000e548) and its $ra
     * (0x8000dbcc), to disassemble and identify the code region. */
    f = fopen("/tmp/ee_dump_r771.bin", "wb");
    fwrite(ee->ram + (0x8000d800u & 0x1FFFFFFFu), 1, 0x1000, f);
    fclose(f);
    printf("wrote /tmp/ee_dump_r771.bin (EE RAM 0x8000d800-0x8000e800)\n");

    /* Also dump the words the IOP loop's own pc range reads, in case
     * it's a data-poll rather than pure control flow: read out
     * 0x000-0x100 (kernel exception-vector/low-RAM area) and print
     * key EE-visible words that a debounce loop might be checking. */
    printf("=== IOP low-RAM 0x00000000-0x00000100 (first 16 words) ===\n");
    for (int i = 0; i < 16; i++) {
        printf("[0x%03x]=0x%08x ", i*4, iop_mem_read32(iop, i*4));
        if (i % 4 == 3) printf("\n");
    }

    return 0;
}
