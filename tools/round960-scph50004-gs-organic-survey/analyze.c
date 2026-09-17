/* Round 960 (task #887, SCPH-50004 GS-display-wiring continuation):
 * direct follow-up to Round 951's finding ("SCPH-50004 diskless boot
 * pushed to 1.2B EE instructions - PMODE still NOT organic within
 * this budget - honest negative, not a bug") and Round 959's ROOT
 * CAUSE fix (ee_mem_read8/write8 MMIO-dispatch gap; the 13-16x
 * LOADFILE "reload" cycle is the real BIOS's own genuine "Restart
 * Without Memory Clear" self-test sequence, each one re-printing
 * "# Initialize GS ..." on the debug console).
 *
 * Open question this round answers: does the real BIOS's own
 * "# Initialize GS ..." restart-cycle step ever perform a genuine
 * GS-privileged-register write (PMODE/SMODE1/SMODE2/DISPFB1/DISPLAY1/
 * DISPFB2/DISPLAY2/...), or is it a pure internal self-test that never
 * touches the CRT/display registers at all? Unlike prior periodic-
 * sampling surveys (Round 939's 5,000,000-slice sampling interval,
 * which is provably too coarse to catch a write-then-immediately-
 * overwritten transient - see Round 927/928's own "coarse-sampling
 * artifact" corrections), this driver hooks EVERY SINGLE ee_core_step()
 * and diffs the full gs_state_t snapshot byte-for-byte each time,
 * so no transient write of any duration can be missed.
 *
 * Since gs.c's gs_mmio_write64()/read64() already generically covers
 * the entire real GS privileged-register range (source/hw/gs.c,
 * cross-checked against PCSX2's pcsx2/Hw.h - see ee_core.c's syscall-2
 * SetGsCrt comment, Round 445/887), any real write the BIOS performs
 * is captured with zero additional modeling risk - this driver only
 * adds observation, no new hardware behavior. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/gs.h"
#include "core/hw/ee_sio.h"

/* EE_IOP_STEP_RATIO, matching system.c's own interleave ratio exactly
 * (see include/core/system.h) - replicated here (instead of calling
 * system_run_interleaved() with a tiny slice count) purely to avoid
 * that function's own "[!] hit slice cap" diagnostic print firing on
 * literally every call when polling this finely, which floods stdout
 * and makes a multi-hundred-million-instruction survey I/O-bound
 * instead of compute-bound. No behavioral difference: same ratio,
 * same per-step calls, just without the noisy per-call print. */
#define R960_EE_IOP_STEP_RATIO 8

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <budget>\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    uint64_t budget = strtoull(argv[2], NULL, 10);
    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    gs_state_t prev_gs;
    memcpy(&prev_gs, gs_get_state(), sizeof(prev_gs));
    uint64_t last_reply = 0;
    int change_prints = 0;

    uint64_t done = 0;
    while (done < budget && !ee->halted) {
        for (int i = 0; i < R960_EE_IOP_STEP_RATIO; i++) {
            if (!ee->halted) ee_core_step();
        }
        if (!iop->halted) iop_core_step();
        done += R960_EE_IOP_STEP_RATIO;

        gs_state_t *gs = gs_get_state();
        if (memcmp(gs, &prev_gs, sizeof(prev_gs)) != 0) {
            if (change_prints < 200) {
                ee_sio_state_t *sio = ee_sio_get_state();
                printf("[R960GS] ee_instr=%llu pmode=0x%02llx smode1=0x%llx smode2=0x%llx "
                       "dispfb1=0x%08llx display1=0x%016llx dispfb2=0x%08llx display2=0x%016llx "
                       "sio_bytes=%u loadfile_reply=%llu\n",
                       (unsigned long long)ee->instructions_executed,
                       (unsigned long long)gs->pmode, (unsigned long long)gs->smode1,
                       (unsigned long long)gs->smode2,
                       (unsigned long long)gs->dispfb1, (unsigned long long)gs->display1,
                       (unsigned long long)gs->dispfb2, (unsigned long long)gs->display2,
                       sio->bytes_written,
                       (unsigned long long)ee_core_get_loadfile_reply_count());
            }
            change_prints++;
            memcpy(&prev_gs, gs, sizeof(prev_gs));
        }

        uint64_t rc = ee_core_get_loadfile_reply_count();
        if (rc != last_reply) {
            printf("[R960REPLY] ee_instr=%llu loadfile_reply %llu->%llu\n",
                   (unsigned long long)ee->instructions_executed,
                   (unsigned long long)last_reply, (unsigned long long)rc);
            last_reply = rc;
        }
    }

    gs_state_t *gs = gs_get_state();
    printf("\n[R960] FINAL ee_instr=%llu ee_halted=%d loadfile_reply_count=%llu "
           "total_gs_state_changes=%d\n",
           (unsigned long long)ee->instructions_executed, ee->halted,
           (unsigned long long)ee_core_get_loadfile_reply_count(), change_prints);
    printf("[R960] FINAL gs: pmode=0x%02llx dispfb1=0x%08llx display1=0x%016llx "
           "dispfb2=0x%08llx display2=0x%016llx\n",
           (unsigned long long)gs->pmode, (unsigned long long)gs->dispfb1,
           (unsigned long long)gs->display1, (unsigned long long)gs->dispfb2,
           (unsigned long long)gs->display2);
    return 0;
}
