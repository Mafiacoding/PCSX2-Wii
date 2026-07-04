/*
 * system.c - interleaved EE/IOP scheduler. See system.h for the
 * rationale and current known simplifications (1:1 instruction
 * stepping, not clock-rate-accurate).
 */
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include <stdio.h>

int system_init(const bios_image_t *ee_bios, const bios_image_t *iop_bios)
{
    if (ee_core_init(ee_bios) != 0) {
        printf("[!] system_init: EE core init failed\n");
        return -1;
    }
    if (iop_core_init(iop_bios) != 0) {
        printf("[!] system_init: IOP core init failed\n");
        return -1;
    }
    return 0;
}

int system_run_interleaved(uint64_t max_slices)
{
    ee_state_t  *ee  = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    uint64_t slice = 0;
    for (;;) {
        if (!ee->halted)
            ee_core_step();
        if (!iop->halted)
            iop_core_step();

        if (ee->halted && iop->halted) {
            printf("\n[+] system_run_interleaved: both cores halted after %llu slice(s)\n",
                   (unsigned long long)slice);
            printf("    EE  halted at pc=0x%08lX after %llu instructions: %s\n",
                   (unsigned long)ee->pc, (unsigned long long)ee->instructions_executed,
                   ee->halt_reason[0] ? ee->halt_reason : "(unknown)");
            printf("    IOP halted at pc=0x%08lX after %llu instructions: %s\n",
                   (unsigned long)iop->pc, (unsigned long long)iop->instructions_executed,
                   iop->halt_reason[0] ? iop->halt_reason : "(unknown)");
            return 1;
        }

        slice++;
        if (max_slices != 0 && slice >= max_slices) {
            printf("\n[!] system_run_interleaved: hit slice cap (%llu) before both cores halted\n",
                   (unsigned long long)max_slices);
            printf("    EE  halted=%d pc=0x%08lX\n", ee->halted, (unsigned long)ee->pc);
            printf("    IOP halted=%d pc=0x%08lX\n", iop->halted, (unsigned long)iop->pc);
            return 0;
        }
    }
}
