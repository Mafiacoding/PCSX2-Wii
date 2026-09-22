/* Round 1005 (task #983, continuing Round 1004): Round 1004 fixed the
 * stale one-shot RPCINIT-ready arm bug and found the EE's resting
 * point moves from the old dead poll loop (pc=0x0026fe9c) to a new
 * location, pc=0x00257964, which disassembly showed to be a real EE
 * BIOS syscall 68 (WaitSema) stub - a genuine kernel blocking
 * primitive, not a synthetic/dead loop.
 *
 * This tool identifies exactly WHICH semaphore ID WaitSema is
 * blocked on at that resting point, and dumps that semaphore's real
 * bookkeeping state, using the public, non-scratch accessor
 * ee_core_get_state() and the WaitSema calling convention already
 * documented in ee_core.c:4296 ("uint32_t semid = (uint32_t)GPR(4)"
 * i.e. semaphore ID is passed in EE GPR $a0 = gpr[4]). No scratch
 * copy or instrumentation needed this round - gpr[] is a public field
 * of ee_state_t (include/core/ee/ee_core.h) and semid is simply
 * whatever's resident in $a0 at the moment we observe the parked pc,
 * since WaitSema's park branch (verified in Round 1004, and again by
 * reading the source directly this round) re-executes the SAME
 * syscall instruction every step without touching $a0 - so sampling
 * $a0 at any step while parked there gives the real, stable semid.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/gs.h"

#define R1004_TARGET_PC 0x00257964u

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
    ee_state_t *ee_probe = ee_core_get_state();
    int reached_target = 0;
    uint64_t reached_at_instr = 0;
    while (done < budget && !ee_probe->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        if (ee_probe->pc == R1004_TARGET_PC && !reached_target) {
            reached_target = 1;
            reached_at_instr = ee_probe->instructions_executed;
        }
    }

    ee_state_t *ee = ee_core_get_state();
    printf("\n[R1005] reached_target_pc=%d reached_at_instr=%llu final_ee_instr=%llu ee_pc=0x%08x halted=%d\n",
           reached_target, (unsigned long long)reached_at_instr,
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);

    printf("\n[R1005] EE GPR snapshot at final pc (all 32, for full calling-convention context):\n");
    for (int i = 0; i < 32; i++) {
        printf("  $%-4d = 0x%016llx%s\n", i, (unsigned long long)ee->gpr[i].ud0,
               (i == 4) ? "   <-- $a0 (WaitSema arg0 = sema_id per ee_core.c:4296)" : "");
    }

    uint32_t semid = (uint32_t)ee->gpr[4].ud0;
    printf("\n[R1005] WaitSema semid = %u (0x%x)\n", semid, semid);

    printf("\n[R1005] EE COP0 state: Status=0x%08x Cause=0x%08x EPC=0x%08x BadVAddr=0x%08x Count=0x%08x\n",
           ee->cop0[12], ee->cop0[13], ee->cop0[14], ee->cop0[8], ee->cop0[9]);

    /* Dump raw code bytes around the parked pc for a quick sanity re-check
     * of the syscall instruction itself (opcode 0x0C = jal is NOT expected
     * here; "syscall" is a SPECIAL opcode, funct=0x0C, encoding 0x0000000C
     * with the code field in bits 6-25 carrying the syscall number). */
    printf("\n[R1005] Raw words around parked pc 0x%08x:\n", ee->pc);
    for (uint32_t a = ee->pc - 16u; a <= ee->pc + 16u; a += 4) {
        uint32_t w = ee_mem_read32(ee, a);
        printf("  [0x%08x] = 0x%08x%s\n", a, w, (a == ee->pc) ? "  <-- pc" : "");
    }

    /* $ra tells us the real caller - useful for identifying which BIOS
     * subsystem/thread issued this particular WaitSema call. */
    printf("\n[R1005] $ra (return address, identifies the real caller) = 0x%016llx\n",
           (unsigned long long)ee->gpr[31].ud0);

    return 0;
}
