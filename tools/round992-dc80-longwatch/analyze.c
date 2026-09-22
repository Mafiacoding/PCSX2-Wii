/* Round 992 (task #972, follow-up to task #971/Round 991): empirically
 * test whether the flag 0x0040dc80 Round 990 found being polled ever
 * becomes non-zero given a much longer real observation window - rather
 * than assuming "stuck forever" from Round 990's short 4096-step sample.
 *
 * Rationale: Round 991's static scan found no EE MIPS instruction
 * anywhere in the resident image writes a non-zero value here, but also
 * found this project's own SIF-RPC completion-delivery code
 * (sif_cmd_iop_send_rpc_bind_rend()/sif_cmd_iop_write_private_queue_copy()
 * in ee_core.c) performs C-level ee_mem_write32() calls to DYNAMICALLY
 * RESOLVED pointers (read from EE memory at delivery time, e.g.
 * MEM[0x0046D618]) - a write path this round's pure static-disassembly
 * scan cannot see at all, since it's not a fixed MIPS instruction
 * sequence. If the struct Round 991 found at 0x0040DB58 (which stores
 * our table's own address at +28) is itself the target of some SIF-RPC
 * or other async delivery whose trigger condition simply hasn't been
 * met yet by instr=56M, running much further - with the EE still
 * spinning in its tight resting loop, but the IOP continuing to run
 * interleaved and any pending interrupt/DMA/timer machinery still
 * ticking every step - could still let a later delivery land here.
 *
 * Method: boot to the Round 990 resting pc (0x0026fe9c) exactly as
 * before, then keep running in large chunks for a MUCH longer total
 * budget, sampling MEM[0x0040dc80] and the EE pc after every chunk.
 * Report the instruction count of the first observed change, if any.
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

#define R990_TARGET_PC 0x0026fe9cu
#define WATCH_ADDR 0x0040dc80u

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <total_budget>\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    uint64_t total_budget = strtoull(argv[2], NULL, 10);

    ee_state_t *ee = ee_core_get_state();
    uint64_t chunk = 1000000ull, done = 0;
    int reached_target = 0;
    /* Phase 1: reach the known resting point (as Round 990/991 did). */
    while (done < total_budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        if (ee->pc == R990_TARGET_PC) { reached_target = 1; break; }
    }
    if (!reached_target) {
        fprintf(stderr, "[R992] did not reach the Round 990 resting pc within budget\n");
        return 1;
    }
    uint64_t reach_instr = ee->instructions_executed;
    uint32_t initial_val = ee_mem_read32(ee, WATCH_ADDR);
    printf("[R992] reached resting pc at ee_instr=%llu, MEM[0x%08x]=0x%08x\n",
           (unsigned long long)reach_instr, WATCH_ADDR, initial_val);

    /* Phase 2: keep running for a much longer window, sampling every
     * chunk. Report the first change, and periodic progress. */
    uint64_t watch_chunk = 5000000ull; /* 20M-instruction samples */
    uint64_t watched = 0;
    uint32_t last_val = initial_val;
    int changed = 0;
    while (done < total_budget && !ee->halted) {
        system_run_interleaved(watch_chunk);
        done += watch_chunk;
        watched += watch_chunk;
        uint32_t v = ee_mem_read32(ee, WATCH_ADDR);
        printf("[R992] watch: ee_instr=%llu pc=0x%08x MEM[0x%08x]=0x%08x%s\n",
               (unsigned long long)ee->instructions_executed, ee->pc, WATCH_ADDR, v,
               (v != last_val) ? "  <-- CHANGED" : "");
        fflush(stdout);
        if (v != last_val) {
            changed = 1;
            last_val = v;
            /* Dump a wider window once it changes, then keep watching
             * briefly to see what happens next. */
            printf("[R992] CHANGE DETECTED at ee_instr=%llu. Table dump 0x0040dc70-0x0040dcd0:\n",
                   (unsigned long long)ee->instructions_executed);
            for (uint32_t a = 0x0040dc70u; a <= 0x0040dcd0u; a += 4)
                printf("  0x%08x: %08x\n", a, ee_mem_read32(ee, a));
        }
    }
    printf("\n[R992] Final: ee_instr=%llu ee_pc=0x%08x ee_halted=%d MEM[0x%08x]=0x%08x changed=%d\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, WATCH_ADDR,
           ee_mem_read32(ee, WATCH_ADDR), changed);

    return 0;
}
