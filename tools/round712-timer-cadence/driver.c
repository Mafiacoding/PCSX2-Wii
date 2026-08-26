/*
 * Round 712 (task #684) - baseline survey of the EE HLE scheduler's
 * thread-switch cadence during a diskless JP-BIOS boot, cross-checked
 * against Status.IE (COP0 Status register bit 0, real MIPS "interrupt
 * enable") at fixed instruction checkpoints.
 *
 * Motivation (see docs/STATUS.md Round 710/711 and this round's own
 * investigation): Round 710 found real GS-refresh/XGKICK work only
 * happens roughly once every several hundred million EE instructions,
 * while ee_core.c's own ee_check_vblank() fires the real VBLANK
 * interrupt source at the CORRECT real cadence (EE_CYCLES_PER_FRAME_
 * NTSC = 4,921,488, matching real 294.912MHz EE clock / 59.94Hz NTSC
 * exactly - confirmed by direct read of ee_core.c this round). Since
 * ee_intc_raise() sets the INTC_STAT bit unconditionally on every real
 * VBLANK boundary regardless of Status.IE, the ~100x gap between
 * VBLANK's own firing cadence and the observed XGKICK/thread-body
 * cadence must be explained by something ELSE gating actual interrupt
 * DELIVERY (ee_check_intc_interrupt() in ee_core.c requires Status.IE=1
 * AND Status.EIE=1 AND not EXL/ERL before it will actually raise the
 * exception) or by the HLE scheduler's own preemption policy
 * (ee_hle_thread_check_preempt(), strict-priority-only, matching real
 * PS2 EE kernel semantics per Round 597-625's own citations).
 *
 * This driver samples, at fixed EE-instruction checkpoints throughout
 * a long diskless boot: instructions_executed, COP0 Status (raw +
 * decoded IE/EXL/ERL/EIE bits), the live HLE current_thread_id, and
 * that thread's own status/priority - to directly test the hypothesis
 * that Status.IE sits at 0 for the vast majority of the boot (which
 * would starve VBLANK delivery regardless of how correctly it's
 * modeled at the ee_intc_raise() level), and to see how often
 * current_thread_id actually changes.
 *
 * Host-native only (tools/ is excluded from the Wii SOURCES glob per
 * existing convention - see e.g. round577/round588 precedent).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"

static const char *status_str(uint32_t s)
{
    switch (s) {
        case 0x01: return "RUN";
        case 0x02: return "READY";
        case 0x04: return "WAIT";
        case 0x08: return "SUSPEND";
        case 0x0C: return "WAITSUSPEND";
        case 0x10: return "DORMANT";
        default: return "?";
    }
}

int main(int argc, char **argv)
{
    const char *bios_path = argc > 1 ? argv[1] : "/tmp/round238_diag/bios.bin";
    uint64_t chunk = 2000000ull;
    uint64_t total_budget = argc > 2 ? strtoull(argv[2], NULL, 10) : 400000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) {
        printf("[FAIL] could not load BIOS at %s\n", bios_path);
        return 1;
    }
    if (system_init(&bios, &bios) != 0) {
        printf("[FAIL] system_init failed\n");
        return 1;
    }
    printf("[R712] diskless boot, BIOS=%s, budget=%llu\n", bios_path, (unsigned long long)total_budget);

    ee_state_t *ee = ee_core_get_state();

    uint64_t ie_zero_samples = 0, ie_one_samples = 0, total_samples = 0;
    int last_tid = -1;
    uint64_t tid_changes = 0;
    uint64_t last_switch_instr = 0;
    uint64_t max_gap = 0;

    uint64_t done = 0;
    int halted = 0;
    while (done < total_budget && !halted) {
        int rc = system_run_interleaved(chunk);
        done += chunk;

        uint32_t status = ee->cop0[12];
        int ie = status & 0x1;
        int exl = (status >> 1) & 0x1;
        int erl = (status >> 2) & 0x1;
        int eie = (status >> 16) & 0x1;
        total_samples++;
        if (ie) ie_one_samples++; else ie_zero_samples++;

        int tid = ee_hle_thread_get_current_thread_id();
        int tcount = ee_hle_thread_get_thread_count();
        if (tid != last_tid) {
            uint64_t gap = ee->instructions_executed - last_switch_instr;
            if (last_tid != -1 && gap > max_gap) max_gap = gap;
            printf("[R712] instr=%llu SWITCH tid %d->%d status=0x%08x IE=%d EXL=%d ERL=%d EIE=%d gap=%llu\n",
                   (unsigned long long)ee->instructions_executed, last_tid, tid, status, ie, exl, erl, eie,
                   (unsigned long long)gap);
            last_tid = tid;
            last_switch_instr = ee->instructions_executed;
            tid_changes++;
        }

        if (total_samples % 10 == 0) {
            printf("[R712] instr=%llu tid=%d tcount=%d status=0x%08x IE=%d EXL=%d ERL=%d EIE=%d\n",
                   (unsigned long long)ee->instructions_executed, tid, tcount, status, ie, exl, erl, eie);
            for (int i = 1; i <= tcount; i++) {
                uint32_t st = ee_hle_thread_get_status(i);
                if (st == 0) continue;
                uint32_t prio = ee_hle_thread_get_priority(i);
                uint32_t entry = ee_hle_thread_get_entry(i);
                uint32_t pc = ee_hle_thread_get_saved_pc(i);
                uint32_t wt = ee_hle_thread_get_wait_type(i);
                uint32_t wid = ee_hle_thread_get_wait_id(i);
                uint32_t wc = ee_hle_thread_get_wakeup_count(i);
                printf("[R712]   thread[%d] status=0x%x(%s) prio=%u entry=0x%08x pc=0x%08x wait_type=%u wait_id=%u wakeups=%u\n",
                       i, st, status_str(st), prio, entry, pc, wt, wid, wc);
            }
        }
    }

    printf("[R712] DONE instr=%llu total_samples=%llu ie_zero=%llu (%.2f%%) ie_one=%llu (%.2f%%) tid_changes=%llu max_gap=%llu\n",
           (unsigned long long)ee->instructions_executed,
           (unsigned long long)total_samples,
           (unsigned long long)ie_zero_samples, 100.0 * (double)ie_zero_samples / (double)(total_samples ? total_samples : 1),
           (unsigned long long)ie_one_samples, 100.0 * (double)ie_one_samples / (double)(total_samples ? total_samples : 1),
           (unsigned long long)tid_changes, (unsigned long long)max_gap);

    /* real VBLANK cadence for cross-reference, ported from ee_core.c's
     * own cited constant (see this file's header comment) */
    const uint64_t EE_CYCLES_PER_FRAME_NTSC = 4921488ull;
    uint64_t expected_vblanks = ee->instructions_executed / EE_CYCLES_PER_FRAME_NTSC;
    printf("[R712] real-cadence cross-check: instructions_executed / EE_CYCLES_PER_FRAME_NTSC = %llu expected VBLANKs over this window\n",
           (unsigned long long)expected_vblanks);

    return 0;
}
