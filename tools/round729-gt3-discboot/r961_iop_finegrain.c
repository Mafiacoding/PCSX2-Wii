/* Round 961 follow-up (task #887/DISP2): tests whether Round 960's
 * "IOP pc frozen at 0x00155b40 across 720,000,000-instruction chain
 * continuations" finding is a REAL permanent freeze, or a coarse-
 * sampling artifact of tools/round729-gt3-discboot/chain_driver.c
 * (which only checks state at 10,000,000-slice CHUNK boundaries, not
 * every step) - exactly the kind of false-freeze this project has
 * been burned by before (see STATUS.md's own Round 927/928 "coarse-
 * sampling artifact" corrections and Round 867-873's similar GT3
 * correction).
 *
 * Round 961's own r961_iop_freeze_dump.c already found the real
 * mechanism this tests: idle=1, sched_ticks actively incrementing
 * (180,000,000 and counting), Status=0x401 (IEc=1, IM2=1 - both
 * conditions iop_check_hw_interrupt() needs already satisfied),
 * istat=0 at the sampled instant. Separately, source/hw/iop_module_
 * loader.c's iop_module_loader_try_handle() has a documented (Round
 * 425/426) re-idle path: when the real fetch/decode path (iop_step())
 * re-enters at pc==g.trampoline_addr with g.idle_transition_done
 * already set, it immediately re-arms idle=1 and returns. That is a
 * plausible, real "wake briefly for VBLANK, do nothing, go back to
 * sleep" cycle - which would make pc/idle look permanently frozen at
 * any sampling interval coarser than one NTSC frame's worth of ticks
 * (IOP_CYCLES_PER_FRAME_NTSC = 615186 sched_ticks, per iop_core.c),
 * while still being correct, cycling hardware-idle behavior underneath.
 *
 * This driver settles the question directly: single-steps
 * iop_core_step() (not system_run_interleaved(), to avoid its noisy
 * per-call "[!] hit slice cap" print - same methodology fix as Round
 * 960's own analyze.c) for enough iterations to span multiple full
 * NTSC frames, logging every single transition of `idle` (0<->1) or
 * `pc` (any change at all), with the sched_ticks/istat/Cause context
 * at each transition. Read-only, no tracked-source changes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_intc.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios> <disc> <ckpt_path> [num_iop_steps]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    uint64_t num_steps = (argc >= 5) ? strtoull(argv[4], NULL, 10) : 3000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) {
        fprintf(stderr, "checkpoint_load FAILED for %s\n", ckpt_path);
        return 1;
    }

    iop_state_t *iop = iop_core_get_state();
    fprintf(stderr, "[R961FG] start: pc=0x%08x idle=%u sched_ticks=%llu\n",
            iop->pc, iop->idle, (unsigned long long)iop->sched_ticks);

    uint32_t last_pc = iop->pc;
    uint8_t last_idle = iop->idle;
    int transitions = 0;
    uint64_t first_wake_tick = 0;
    int wake_count = 0;

    for (uint64_t i = 0; i < num_steps; i++) {
        if (iop->halted) {
            fprintf(stderr, "[R961FG] iop halted at step=%llu pc=0x%08x reason=\"%s\"\n",
                    (unsigned long long)i, iop->pc, iop->halt_reason);
            break;
        }
        iop_core_step();

        if (iop->idle != last_idle || iop->pc != last_pc) {
            iop_intc_state_t *intc = iop_intc_get_state();
            if (transitions < 400) {
                fprintf(stderr, "[R961FG-EVT] step=%llu sched_ticks=%llu pc=0x%08x->0x%08x idle=%u->%u "
                        "istat=0x%08x cause=0x%08x exc_pending=%u\n",
                        (unsigned long long)i, (unsigned long long)iop->sched_ticks,
                        last_pc, iop->pc, last_idle, iop->idle,
                        intc->istat, iop->cop0[13], iop->exception_pending);
            }
            if (last_idle == 1 && iop->idle == 0 && wake_count == 0) {
                first_wake_tick = iop->sched_ticks;
            }
            if (last_idle == 1 && iop->idle == 0) wake_count++;
            transitions++;
            last_pc = iop->pc;
            last_idle = iop->idle;
        }
    }

    fprintf(stderr, "\n[R961FG] FINAL after %llu steps: pc=0x%08x idle=%u sched_ticks=%llu "
            "total_transitions=%d wake_count=%d first_wake_sched_tick=%llu\n",
            (unsigned long long)num_steps, iop->pc, iop->idle,
            (unsigned long long)iop->sched_ticks, transitions, wake_count,
            (unsigned long long)first_wake_tick);
    return 0;
}
