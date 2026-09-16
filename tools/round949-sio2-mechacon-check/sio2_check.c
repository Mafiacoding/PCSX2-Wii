/* Round 949 (task #221/#447/#536): checks whether the IOP-side real
 * SIO2 CTRL register (pad/mc polling trigger) has been written at all
 * by the time the resting-loop checkpoint state is reached, and dumps
 * the real CDVD status/ready bytes. This is a read-only diagnostic -
 * no synthetic/forced state is injected. See STATUS.md Round 949 entry
 * for the full citation trail (Woon Yung's real OSD-init writeup vs
 * this project's proposed-but-unevidenced periodic-IP3/MECHACON-flag
 * patch).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/hw/iop_sio2.h"

extern uint8_t  iop_cdvd_get_status(void);
extern uint8_t  iop_cdvd_get_ready(void);
extern uint64_t iop_cdvd_get_scmd_call_count(void);

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <bios> <ckpt>\n", argv[0]); return 1; }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(argv[2], &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    printf("[R949] iop_sio2_get_pad_command_count() = %u\n", iop_sio2_get_pad_command_count());
    printf("[R949] iop_cdvd_get_status()  = 0x%02x\n", iop_cdvd_get_status());
    printf("[R949] iop_cdvd_get_ready()   = 0x%02x\n", iop_cdvd_get_ready());
    printf("[R949] iop_cdvd_get_scmd_call_count() = %llu\n",
           (unsigned long long)iop_cdvd_get_scmd_call_count());
    return 0;
}
