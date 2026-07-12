/*
 * system.c - interleaved EE/IOP scheduler. See system.h for the
 * rationale and current known simplifications (EE_IOP_STEP_RATIO
 * instructions per slice on the EE side per 1 on the IOP side - a
 * ratio-aware, but still not cycle-accurate, approximation of real
 * hardware's ~8:1 clock difference; see system.h).
 */
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include <stdio.h>

/* Real EE clock (~294.912 MHz) vs real IOP clock (~36.864 MHz) is
 * roughly 8:1 - already documented as this project's target ratio in
 * system.h/docs/ROADMAP.md before this was implemented. This does NOT
 * make the scheduler cycle-accurate (different MIPS instructions take
 * different real cycle counts on both cores, none of which is
 * modeled) - it just steps the EE 8 real instructions for every 1 IOP
 * instruction per slice, instead of the previous 1:1, so a given wall-
 * clock-equivalent slice count gives each core roughly the right
 * SHARE of total instructions executed. An honest, noted
 * approximation, not a claim of real timing fidelity. */
#define EE_IOP_STEP_RATIO 8

/* Task #172 continued: adapts iop_mem_write8()'s real signature to
 * the generic (void *ctx, addr, val) shape ee_core.c's optional SIF
 * DMA-copy bridge expects - see ee_core.h's ee_core_set_iop_write8_
 * bridge() comment for why this indirection exists (keeping ee_core.c
 * free of a hard link-time dependency on iop_core.c). */
static void system_iop_write8_adapter(void *ctx, uint32_t addr, uint8_t val)
{
    iop_mem_write8((iop_state_t *)ctx, addr, val);
}

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
    ee_core_set_iop_write8_bridge(iop_core_get_state(), system_iop_write8_adapter);
    return 0;
}

int system_run_interleaved(uint64_t max_slices)
{
    ee_state_t  *ee  = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    uint64_t slice = 0;
    for (;;) {
        for (int i = 0; i < EE_IOP_STEP_RATIO; i++) {
            if (!ee->halted)
                ee_core_step();
        }
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
