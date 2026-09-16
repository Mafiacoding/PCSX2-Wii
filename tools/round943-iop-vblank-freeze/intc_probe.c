/* Round 943b (task #447/#536 continuation): follow-up to r943_iop_freeze_probe.c.
 * That probe showed the IOP genuinely idle (Cause=0, exception_pending=0)
 * static across 200 steps at pc=0x00155910. iop_check_vblank() (iop_core.c
 * ~line 665) proves VBLANK_START/END are raised periodically via
 * iop_intc_raise() purely off instructions_executed (period
 * IOP_CYCLES_PER_FRAME_NTSC=615186), completely independent of CPU/module
 * state - so over 2.8 BILLION instructions, VBLANK must have been raised
 * ~4551 times already. If Cause.IP2 never sets despite that, the IOP INTC's
 * own imask/imask_hi registers must be masking every single source,
 * including VBLANK - which is the real open question this probe answers:
 * dump intc->istat/imask/istat_hi/imask_hi directly, then single-step
 * across more than 2 full VBLANK periods watching for ANY istat/cause
 * change.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_intc.h"
#include "core/hw/iop_timers.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <bios_path> <ckpt_path>\n", argv[0]); return 1; }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(argv[2], &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    iop_state_t *iop = iop_core_get_state();
    iop_intc_state_t *intc = iop_intc_get_state();

    printf("[R943B] initial iop_pc=0x%08x idle=%d instructions_executed=%llu\n",
           iop->pc, iop->idle, (unsigned long long)iop->instructions_executed);
    printf("[R943B] intc: istat=0x%08x imask=0x%08x istat_hi=0x%08x imask_hi=0x%08x\n",
           intc->istat, intc->imask, intc->istat_hi, intc->imask_hi);
    printf("[R943B] cop0: Status=0x%08x Cause=0x%08x\n", iop->cop0[12], iop->cop0[13]);

    iop_timers_state_t *timers = iop_timers_get_state();
    for (int i = 0; i < IOP_TIMERS_COUNT; i++) {
        printf("[R943B] timer[%d]: count=0x%08x mode=0x%08x target=0x%08x stopped=%d intr_en=%d\n",
               i, timers->t[i].count, timers->t[i].mode, timers->t[i].target,
               (timers->t[i].mode & 0x1) ? 1 : 0, 0);
    }

    uint32_t last_istat = intc->istat, last_imask = intc->imask;
    uint32_t last_istat_hi = intc->istat_hi, last_imask_hi = intc->imask_hi;
    uint32_t last_cause = iop->cop0[13];
    uint64_t period = 615186ull;
    uint64_t total_steps = period * 3ull; /* just over 3 full VBLANK periods */
    uint64_t changes = 0;

    for (uint64_t i = 0; i < total_steps; i++) {
        iop_core_step();
        if (intc->istat != last_istat || intc->imask != last_imask ||
            intc->istat_hi != last_istat_hi || intc->imask_hi != last_imask_hi ||
            iop->cop0[13] != last_cause) {
            changes++;
            if (changes <= 60) {
                printf("[R943B-CHANGE] step=%llu instr=%llu istat=0x%08x(was 0x%08x) imask=0x%08x(was 0x%08x) istat_hi=0x%08x imask_hi=0x%08x cause=0x%08x(was 0x%08x) pc=0x%08x idle=%d\n",
                       (unsigned long long)i, (unsigned long long)iop->instructions_executed,
                       intc->istat, last_istat, intc->imask, last_imask,
                       intc->istat_hi, intc->imask_hi, iop->cop0[13], last_cause, iop->pc, iop->idle);
            }
            last_istat = intc->istat; last_imask = intc->imask;
            last_istat_hi = intc->istat_hi; last_imask_hi = intc->imask_hi;
            last_cause = iop->cop0[13];
        }
        if (iop->halted) { printf("[R943B] IOP halted: %s\n", iop->halt_reason); break; }
    }

    printf("[R943B] done. total_steps=%llu changes_observed=%llu\n",
           (unsigned long long)total_steps, (unsigned long long)changes);
    printf("[R943B] final: pc=0x%08x idle=%d instructions_executed=%llu\n",
           iop->pc, iop->idle, (unsigned long long)iop->instructions_executed);
    printf("[R943B] final intc: istat=0x%08x imask=0x%08x istat_hi=0x%08x imask_hi=0x%08x\n",
           intc->istat, intc->imask, intc->istat_hi, intc->imask_hi);
    return 0;
}
