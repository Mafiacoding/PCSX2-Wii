#ifndef PCSX2WII_BIOS_LOADER_H
#define PCSX2WII_BIOS_LOADER_H

#include <stdint.h>
#include <stddef.h>

/* PS2 BIOS images are 4 MiB ROM dumps. We keep it in MEM2 (the Wii's
 * 64MB GDDR3 pool) since MEM1 (24MB, fast) is needed for EE RAM (32MB
 * emulated - already more than MEM1 alone can hold, a first-class
 * problem for any full emulation attempt, see docs/STATUS.md). */
#define BIOS_MAX_SIZE   (4 * 1024 * 1024)
#define BIOS_RESET_VECTOR 0xBFC00000u

typedef struct {
    uint8_t *data;             /* BIOS ROM bytes, BIOS_MAX_SIZE aligned buffer */
    uint32_t size;              /* actual dumped size, usually 4194304 */
    char name[64];
    char version_string[32];    /* parsed out of the BIOS "ROMVER" area if found */
    uint8_t loaded;
} bios_image_t;

/* Loads a raw PS2 BIOS dump (.bin) from the given libfat path (e.g.
 * "sd:/pcsx2/bios/bios.bin") into 'out'. Returns 0 on success, -1 on
 * failure (file missing, wrong size, or ROMVER magic not found). */
int bios_load(const char *path, bios_image_t *out);

void bios_free(bios_image_t *bios);

#endif
