/* Round 1005 (task #983) companion driver: links against a SCRATCH
 * COPY of ee_core.c (/tmp/ee_core_r1005.c, per this project's
 * backup-before-experimenting rule - never committed/tracked)
 * instrumented with 3 fprintf trace points (CreateSema id assignment,
 * WaitSema(0)'s first park, and every SignalSema/iSignalSema call) to
 * find: (a) which real code creates semaphore 0, and (b) whether
 * anything in the real boot trace ever calls SignalSema/iSignalSema(0)
 * at all - i.e. whether this is a producer that hasn't run yet, or a
 * genuinely absent producer (same class of question Round 1004
 * answered for the RPCINIT-ready mailbox). This driver itself is
 * generic (no instrumentation of its own) - all trace output comes
 * from the scratch ee_core.c via stderr.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <budget>\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    uint64_t budget = strtoull(argv[2], NULL, 10);
    uint64_t chunk = 1000000ull, done = 0;
    ee_state_t *ee = ee_core_get_state();
    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
    }
    printf("\n[R1005-DRIVER] final ee_instr=%llu ee_pc=0x%08x halted=%d\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);
    return 0;
}
