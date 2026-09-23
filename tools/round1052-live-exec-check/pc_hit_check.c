/* Round 1052: does the real IOP ever actually fetch/execute any PC in
 * 0x00115800-0x00115D20 (the Reschedule()/GetHighestReadyPriority()/
 * enclosing-dispatch-function region Round 1051 found) during a real
 * boot run? Direct execution-based confirmation, not just static
 * xref - resumes from the Round 1045 steady-state checkpoint and runs
 * forward, counting hits per-address in that range.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"

static uint32_t hitcount[0x600/4];

int main(int argc, char **argv)
{
    bios_image_t bios;
    bios_load(argv[1], &bios);
    checkpoint_load(argv[2], &bios, &bios, NULL);
    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();
    uint64_t budget = argc > 3 ? strtoull(argv[3], NULL, 10) : 30000000ull;
    uint64_t chunk = 100000ull, done = 0;
    while (done < budget && !ee->halted) {
        uint32_t pc = iop->pc;
        if (pc >= 0x00115800u && pc < 0x00115E00u) {
            hitcount[(pc - 0x00115800u)/4]++;
        }
        system_run_interleaved(chunk);
        done += chunk;
    }
    int any = 0;
    for (int i = 0; i < 0x600/4; i++) {
        if (hitcount[i]) { printf("[R1052-HIT] pc=0x%08x hits=%u\n", 0x00115800u+i*4, hitcount[i]); any=1; }
    }
    if (!any) printf("[R1052-HIT] no hits in 0x00115800-0x00115E00 across %llu instructions (sampled every 100000)\n", (unsigned long long)done);
    return 0;
}
