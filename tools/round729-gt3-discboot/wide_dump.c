/*
 * Round 766 (task #761): scratch wide-window dumper, follow-up to
 * ckpt_inspect.c. Dumps a much larger IOP RAM window (0x00000000-
 * 0x00020000, covering the whole early fixed-address kernel-module
 * range from Round 760/762's table: SYSMEM/EXCEPMAN/SSBUSC/DMACMAN/
 * TIMEMANP-I/EECONF/VBLANK/IOMAN/STDIO/SIFMAN/SIFCMD) to a flat file,
 * so it can be statically scanned for the raw encoding of a real
 * `jal 0x00000c3c` instruction (0x0C000000 | (0xC3C>>2) = 0x0C00030F,
 * little-endian bytes 0F 03 00 0C) - i.e. the real caller of the
 * self-looping subroutine identified in Round 765 - and so the
 * region around 0x00001358 (the subroutine's own internal jal target)
 * can be disassembled. Never committed - scratch diagnostic only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> <ckpt_path>\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(argv[3], &bios, &bios, argv[2]) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    iop_state_t *iop = iop_core_get_state();

    FILE *f = fopen("/tmp/iop_wide_dump.bin", "wb");
    fwrite(iop->ram, 1, 0x20000, f);
    fclose(f);
    printf("wrote /tmp/iop_wide_dump.bin (IOP RAM 0x00000000-0x00020000)\n");

    return 0;
}
