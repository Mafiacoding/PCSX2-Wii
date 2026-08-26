/*
 * Round 716 (task #696-699), per the user's "make the code work that
 * would wake it up" directive: samples EE_INTC per-cause raise counts
 * (ee_intc_get_raise_count(), Round 716 diagnostic) alongside the
 * existing null-jalr recovery-guard hit total (g_ee_null_jalr_guard_hits,
 * Round 630) and the EE-timer per-cause IRQ counter
 * (ee_timers_get_irq_count(), Round 715) across a diskless JP-BIOS
 * boot survey. Goal: determine whether VBLANK_START(2)/VBLANK_END(3)
 * interrupts are (a) never actually raised, (b) raised but failing to
 * dispatch (would show up as many null-jalr guard hits), or (c) raised
 * and dispatching successfully to a real, non-null handler every time
 * (raise count climbs steadily, guard-hit count stays flat/low).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/hw/ee_intc.h"
#include "core/hw/ee_timers.h"

extern long g_ee_null_jalr_guard_hits;

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *bios_path = argc > 1 ? argv[1] : "/tmp/r708/bios.bin";
    uint64_t budget = argc > 2 ? strtoull(argv[2], NULL, 10) : 100000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { printf("[FAIL] bios load\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { printf("[FAIL] system_init\n"); return 1; }
    printf("[R716] diskless boot, BIOS=%s, budget=%llu\n", bios_path, (unsigned long long)budget);

    uint64_t chunk = 4000000ull, done = 0;
    uint32_t last_raise[16] = {0};
    long last_guard = 0;

    while (done < budget) {
        system_run_interleaved(chunk);
        done += chunk;

        int changed = (g_ee_null_jalr_guard_hits != last_guard);
        for (int i = 0; i < 16 && !changed; i++)
            if (ee_intc_get_raise_count(i) != last_raise[i]) changed = 1;

        if (changed) {
            printf("[R716] instr=%llu guard_hits=%ld  raise[GS=%u SBUS=%u VBS=%u VBE=%u VIF0=%u VIF1=%u VU0=%u VU1=%u IPU=%u T0=%u T1=%u T2=%u T3=%u SFIFO=%u VUWD=%u]\n",
                (unsigned long long)done, g_ee_null_jalr_guard_hits,
                ee_intc_get_raise_count(0), ee_intc_get_raise_count(1),
                ee_intc_get_raise_count(2), ee_intc_get_raise_count(3),
                ee_intc_get_raise_count(4), ee_intc_get_raise_count(5),
                ee_intc_get_raise_count(6), ee_intc_get_raise_count(7),
                ee_intc_get_raise_count(8), ee_intc_get_raise_count(9),
                ee_intc_get_raise_count(10), ee_intc_get_raise_count(11),
                ee_intc_get_raise_count(12), ee_intc_get_raise_count(13),
                ee_intc_get_raise_count(14));
            last_guard = g_ee_null_jalr_guard_hits;
            for (int i = 0; i < 16; i++) last_raise[i] = ee_intc_get_raise_count(i);
        }
    }

    printf("[R716] FINAL after %llu instructions: guard_hits=%ld\n", (unsigned long long)done, g_ee_null_jalr_guard_hits);
    for (int i = 0; i < 16; i++)
        printf("[R716]   cause %d: raise_count=%u\n", i, ee_intc_get_raise_count(i));
    printf("[R716]   TIMER3 (ee_timers_get_irq_count(3))=%u\n", ee_timers_get_irq_count(3));
    return 0;
}
