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
    if (argc < 6) { fprintf(stderr, "usage: %s <bios> <disc> <ckpt> <start_addr_hex> <len_bytes>\n", argv[0]); return 1; }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios fail\n"); return 1; }
    if (checkpoint_load(argv[3], &bios, &bios, argv[2]) != 0) { fprintf(stderr, "ckpt fail\n"); return 1; }
    iop_state_t *iop = iop_core_get_state();
    uint32_t start = (uint32_t)strtoul(argv[4], NULL, 16);
    uint32_t len = (uint32_t)strtoul(argv[5], NULL, 10);
    FILE *f = fopen("/tmp/r815run/iop_ramdump.bin", "wb");
    for (uint32_t a = start; a < start + len; a += 4) {
        uint32_t w = iop_mem_read32(iop, a);
        fwrite(&w, 4, 1, f);
    }
    fclose(f);
    fprintf(stderr, "dumped 0x%08x..0x%08x (%u bytes) to /tmp/r815run/iop_ramdump.bin\n", start, start+len, len);
    return 0;
}
