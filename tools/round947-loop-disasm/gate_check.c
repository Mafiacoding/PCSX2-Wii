#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"

int main(int argc, char **argv)
{
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(argv[2], &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }
    ee_state_t *ee = ee_core_get_state();
    printf("ee_pc=0x%08x\n", ee->pc);
    for (uint32_t a = 0x80020CE0u; a <= 0x80020D10u; a += 4)
        printf("  [0x%08x] = 0x%08x\n", a, ee_mem_read32(ee, a));
    printf("device table 0x80020B60-0x80020BA0:\n");
    for (uint32_t a = 0x80020B60u; a <= 0x80020BA0u; a += 4)
        printf("  [0x%08x] = 0x%08x\n", a, ee_mem_read32(ee, a));
    return 0;
}
