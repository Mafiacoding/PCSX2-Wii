/*
 * Round 715 (task #693/#694), per the user's explicit "break the chain
 * and search every single page sdk etc to fix this timer issue, focus
 * only on this big gap" instruction: deep instrumentation of the real
 * EE peripheral Timer0-3 registers (ee_timers.c) across a diskless
 * JP-BIOS boot, cross-referenced against the real ps2sdk EE kernel
 * alarm.c/timer.c sources (docs/reference/ps2sdk/ee/kernel/src/) which
 * confirm Timer3 (EE_INTC bit 12) is the real hardware timer underlying
 * the kernel's own SetAlarm/iSetAlarm subsystem (alarm.c's InitAlarm()
 * checks `T3_MODE & 0x100` - the real REPEAT_IRQ bit - to detect
 * whether the real kernel has already armed T3 in repeat mode).
 *
 * Goal: determine empirically whether T3 (or any timer) is armed by
 * real BIOS code in this scenario, whether REPEAT_IRQ is set, and
 * whether ee_timers_get_irq_count() (Round 715's new diagnostic) shows
 * a timer firing ONCE (matching Round 630's "null-JALR guard fires
 * exactly once" observation) vs firing repeatedly as a real periodic
 * heartbeat should.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/hw/ee_timers.h"
#include "core/ee/ee_hle_thread.h"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *bios_path = argc > 1 ? argv[1] : "/tmp/r708/bios.bin";
    uint64_t budget = argc > 2 ? strtoull(argv[2], NULL, 10) : 100000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { printf("[FAIL] bios load\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { printf("[FAIL] system_init\n"); return 1; }
    printf("[R715] diskless boot, BIOS=%s, budget=%llu\n", bios_path, (unsigned long long)budget);

    uint64_t chunk = 2000000ull, done = 0;
    uint32_t last_mode[4] = {0,0,0,0};
    uint32_t last_irq[4] = {0,0,0,0};

    ee_timers_state_t *ts = ee_timers_get_state();

    while (done < budget) {
        system_run_interleaved(chunk);
        done += chunk;

        for (int i = 0; i < 4; i++) {
            uint32_t mode = ts->t[i].mode;
            uint32_t irq = ee_timers_get_irq_count(i);
            if (mode != last_mode[i] || irq != last_irq[i]) {
                printf("[R715] instr=%llu T%d: MODE=0x%04x COUNT=0x%04x COMP=0x%04x CUE=%d CMPE=%d OVFE=%d REPEAT=%d ZRET=%d EQUF=%d OVFF=%d irq_count=%u (CHANGED)\n",
                       (unsigned long long)done, i, mode, ts->t[i].count, ts->t[i].comp,
                       !!(mode & 0x0400), !!(mode & 0x0040), !!(mode & 0x0080),
                       !!(mode & 0x0100), !!(mode & 0x0020), !!(mode & 0x0800), !!(mode & 0x1000),
                       irq);
                last_mode[i] = mode;
                last_irq[i] = irq;
            }
        }
    }

    printf("[R715] FINAL after %llu instructions:\n", (unsigned long long)done);
    for (int i = 0; i < 4; i++) {
        printf("[R715]   T%d: MODE=0x%04x COUNT=0x%04x COMP=0x%04x irq_count=%u\n",
               i, ts->t[i].mode, ts->t[i].count, ts->t[i].comp, ee_timers_get_irq_count(i));
    }
    return 0;
}
