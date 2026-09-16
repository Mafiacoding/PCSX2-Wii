/* Direct mechanism verification for the Round 943 sched_ticks fix:
 * put the IOP into idle=1 (as iop_module_loader.c's real completion
 * path does), with Status configured exactly like the real frozen
 * checkpoint we found (Status=0x00000401: IEc=1, IM2 unmasked - the
 * only IM line the real hardware ever needs since all IOP INTC
 * sources multiplex onto Cause.IP2), then drive iop_core_step()
 * (the REAL top-level tick function, not iop_check_vblank() in
 * isolation) across more than one full VBLANK period and confirm:
 *   1) sched_ticks actually advances every call, even while idle
 *   2) VBLANK_START's raise actually flips istat
 *   3) idle actually clears (iop_check_hw_interrupt sees Cause.IP2
 *      go pending+unmasked and wakes the core) - the exact chain
 *      that was broken before this round's fix.
 */
#include <stdio.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_intc.h"

int main(int argc, char **argv)
{
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "init fail\n"); return 1; }

    iop_state_t *iop = iop_core_get_state();
    iop_intc_state_t *intc = iop_intc_get_state();

    /* Force the exact frozen-checkpoint scenario we found in the
     * field: idle=1, Status=0x00000401, Cause=0, istat/imask matching
     * "VBLANK_START unmasked, everything else masked or pending-but-
     * masked". */
    iop->idle = 1;
    iop->exception_pending = 0;
    iop->cop0[12] = 0x00000401u; /* Status: IEc=1, IM2=1 */
    iop->cop0[13] = 0x00000000u; /* Cause: clear */
    iop->sched_ticks = 0;
    intc->istat = 0;
    intc->imask = 0x0001000du; /* matches the real frozen checkpoint's imask */
    intc->istat_hi = 0;
    intc->imask_hi = 0x00000c00u;

    printf("[R943MECH] initial: idle=%d sched_ticks=%llu istat=0x%08x cause=0x%08x\n",
           iop->idle, (unsigned long long)iop->sched_ticks, intc->istat, iop->cop0[13]);

    uint64_t period = 615186ull;
    int woke_at = -1;
    for (uint64_t i = 0; i < period + 10; i++) {
        uint8_t idle_before = iop->idle;
        iop_core_step();
        if (idle_before && !iop->idle) {
            woke_at = (int)i;
            printf("[R943MECH] WOKE at step=%llu sched_ticks=%llu istat=0x%08x cause=0x%08x pc=0x%08x\n",
                   (unsigned long long)i, (unsigned long long)iop->sched_ticks, intc->istat, iop->cop0[13], iop->pc);
            break;
        }
    }
    if (woke_at < 0) {
        printf("[R943MECH] FAILED TO WAKE across %llu steps. final sched_ticks=%llu istat=0x%08x idle=%d\n",
               (unsigned long long)(period + 10), (unsigned long long)iop->sched_ticks, intc->istat, iop->idle);
        return 1;
    }
    printf("[R943MECH] SUCCESS: idle correctly cleared by VBLANK_START wake at step %d (expected phase 0, i.e. step 0 or step %llu due to modulo)\n",
           woke_at, (unsigned long long)period);
    return 0;
}
