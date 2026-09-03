/*
 * Round 817 (task #811/#820/#821 continuation) - GT3 semaphore-5
 * probe, per the user's own explicit, narrowly-gated diagnostic spec
 * following Round 816's exhaustive continuous negative result (zero
 * CDVD SIF RPC calls/binds across ~833M post-handoff instructions,
 * classified bucket 1: "no CDVD import ever reached").
 *
 * Purpose (diagnostic, NOT a fix): answer "does releasing thread 1
 * from its permanent WaitSema(5) park let GT3 reach its first CDVD
 * call?" Cold-boot-only continuous run (same no-checkpoint discipline
 * as r816_continuous_cdvd.c - the ~24M-instruction checkpoint artifact
 * is explicitly NOT used here, both per the user's instruction and
 * because using a checkpoint of unverified fidelity would contaminate
 * this exact experiment's own "is the resumed thread state valid"
 * interpretation branch).
 *
 * The gated probe below fires AT MOST ONCE per process run, and only
 * when ALL of these hold simultaneously:
 *   - GT3 is active (proxy: total_instr has passed the real, twice-
 *     independently-confirmed handoff point ~24-30M raw instructions,
 *     per Rounds 815/816's own captured handoff_pc=0x80002fbc events -
 *     this driver only ever boots GT3, so this is an honest, scoped
 *     proxy, not a general-purpose "is GT3 loaded" check);
 *   - a live EE HLE thread is found WAIT/SEMA on semaphore id 5
 *     (ee_hle_thread_get_status()==EE_THS_WAIT(0x4) &&
 *      get_wait_type()==EE_TSW_SEMA(2) && get_wait_id()==5 - these
 *     raw values are documented in ee_hle_thread.h's own struct-
 *     layout comment, no new accessor needed);
 *   - iop_cdvd_get_ncmd_call_count()==0 (no CDVD import has happened
 *     yet - the exact condition Round 816 already established holds
 *     for the entire observable window);
 *   - iop_cdvd_get_scmd_call_count()>=13 (matches the already-
 *     documented real IOP-internal S-command boilerplate burst that
 *     completes very early in every boot, per Round 814's own
 *     CLOSECONFIG-continuation citation - confirms IOP init is past
 *     its own early boilerplate, not still mid-reset).
 *
 * The wake itself uses ee_hle_thread_debug_signal_sema(5) (Round 817,
 * see ee_hle_thread.c) - the EXACT real SignalSema() count-increment +
 * wake_one_sema_waiter() transition, not a direct TCB mutation. Per
 * the user's own instruction, this is a diagnostic disabled by default
 * (GT3_SEM5_PROBE must be defined at compile time) - the file may stay
 * in the tree for reproducibility, but ships inert.
 *
 * Usage: r817_sem5_probe <bios_path> <disc_path> [budget]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/hw/iop_cdvd.h"

#ifdef GT3_SEM5_PROBE
/* Raw values, documented in ee_hle_thread.h's own struct-layout
 * comment (Status bits/Wait-type section) - no new accessor added
 * for these since the existing get_status()/get_wait_type()/
 * get_wait_id() accessors already return them verbatim. */
#define R817_EE_THS_WAIT 0x04u
#define R817_EE_TSW_SEMA  2u
#define R817_HANDOFF_PROXY_INSTR 30000000ull /* Rounds 815/816: real handoff confirmed ~24-30M raw instructions */
#ifndef R817_PROBE_SEMID
#define R817_PROBE_SEMID 5 /* default target; override at compile time, e.g. -DR817_PROBE_SEMID=0 */
#endif

static int g_probe_fired = 0;
static int g_probe_post_ticks = 0;

static void maybe_probe_gt3_sem5(uint64_t total_instr)
{
    if (g_probe_fired) return;
    if (total_instr < R817_HANDOFF_PROXY_INSTR) return; /* "GT3 is active" proxy - see file header */
    if (iop_cdvd_get_ncmd_call_count() != 0) return;     /* an import already happened - probe moot */
    if (iop_cdvd_get_scmd_call_count() < 13) return;      /* IOP still mid early-boot boilerplate */

    int found_tid = 0;
    int count = ee_hle_thread_get_thread_count();
    for (int tid = 1; tid <= count; tid++) {
        if (ee_hle_thread_get_status(tid) == R817_EE_THS_WAIT &&
            ee_hle_thread_get_wait_type(tid) == R817_EE_TSW_SEMA &&
            ee_hle_thread_get_wait_id(tid) == (uint32_t)R817_PROBE_SEMID) {
            found_tid = tid;
            break;
        }
    }
    if (!found_tid) return;

    g_probe_fired = 1;
    fprintf(stderr, "[R817-PROBE] gate satisfied: tid=%d WAIT/SEMA/%d total_instr=%llu ncmd=0 scmd=%llu\n",
            found_tid, R817_PROBE_SEMID, (unsigned long long)total_instr,
            (unsigned long long)iop_cdvd_get_scmd_call_count());
    fprintf(stderr, "[R817-PROBE] pre-signal current_tid=%d\n", ee_hle_thread_get_current_thread_id());
    for (int t = 1; t <= count; t++) {
        fprintf(stderr, "[R817-PROBE] pre  tid=%d status=0x%x wait_type=%u wait_id=%u saved_pc=0x%08x prio=%u\n",
                t, ee_hle_thread_get_status(t), ee_hle_thread_get_wait_type(t),
                ee_hle_thread_get_wait_id(t), ee_hle_thread_get_saved_pc(t), ee_hle_thread_get_priority(t));
    }
    int rc = ee_hle_thread_debug_signal_sema(R817_PROBE_SEMID);
    fprintf(stderr, "[R817-PROBE] ee_hle_thread_debug_signal_sema(5) rc=%d (1=signaled 0=invalid -1=overflow)\n", rc);
    fprintf(stderr, "[R817-PROBE] post-signal current_tid=%d\n", ee_hle_thread_get_current_thread_id());
    for (int t = 1; t <= count; t++) {
        fprintf(stderr, "[R817-PROBE] post tid=%d status=0x%x wait_type=%u wait_id=%u saved_pc=0x%08x prio=%u\n",
                t, ee_hle_thread_get_status(t), ee_hle_thread_get_wait_type(t),
                ee_hle_thread_get_wait_id(t), ee_hle_thread_get_saved_pc(t), ee_hle_thread_get_priority(t));
    }
    g_probe_post_ticks = 8;
}
#endif

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> [budget]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    uint64_t budget = argc > 3 ? strtoull(argv[3], NULL, 10) : 150000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12 /* CDVD_TYPE_PS2CD */);

    ee_state_t *ee = ee_core_get_state();
#ifdef GT3_SEM5_PROBE
    fprintf(stderr, "[R817-TRACE] cold-boot start, GT3_SEM5_PROBE ENABLED, budget=%llu\n", (unsigned long long)budget);
#else
    fprintf(stderr, "[R817-TRACE] cold-boot start, GT3_SEM5_PROBE disabled (control run), budget=%llu\n", (unsigned long long)budget);
#endif

    const long SLICE = 1000000;
    uint64_t done = 0;
    while (done < budget && !ee->halted) {
        system_run_interleaved(SLICE);
        done += (uint64_t)SLICE;
#ifdef GT3_SEM5_PROBE
        maybe_probe_gt3_sem5(ee->instructions_executed);
        if (g_probe_post_ticks > 0) {
            g_probe_post_ticks--;
            fprintf(stderr, "[R817-PROBE] post-tick current_tid=%d pc=0x%08x ncmd=%llu scmd=%llu\n",
                    ee_hle_thread_get_current_thread_id(), ee->pc,
                    (unsigned long long)iop_cdvd_get_ncmd_call_count(),
                    (unsigned long long)iop_cdvd_get_scmd_call_count());
        }
#endif
        if ((done / (uint64_t)SLICE) % 20 == 0) {
            fprintf(stderr, "[R817-TRACE] progress done=%llu total_instr=%llu pc=0x%08x ncmd=%llu scmd=%llu\n",
                    (unsigned long long)done, (unsigned long long)ee->instructions_executed,
                    ee->pc, (unsigned long long)iop_cdvd_get_ncmd_call_count(),
                    (unsigned long long)iop_cdvd_get_scmd_call_count());
        }
    }

    fprintf(stderr, "[R817-TRACE] FINAL total_instr=%llu pc=0x%08x halted=%u ncmd=%llu scmd=%llu\n",
            (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
            (unsigned long long)iop_cdvd_get_ncmd_call_count(),
            (unsigned long long)iop_cdvd_get_scmd_call_count());
    if (ee->halted) {
        fprintf(stderr, "[R817-TRACE] EE halted: %s\n", ee->halt_reason);
    }
    return 0;
}
