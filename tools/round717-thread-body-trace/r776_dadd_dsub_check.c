/*
 * Round 776b: narrow, standalone verification of the DADD (funct=0x2C)
 * and DSUB (funct=0x2E) fix just added to ee_core.c's SPECIAL-opcode
 * switch. Directly encodes and executes one DADD and one DSUB
 * instruction against known GPR inputs and checks the 64-bit result,
 * proving the new fallthrough cases compute the same real MIPS III
 * add/sub semantics as their already-tested DADDU/DSUBU siblings
 * (this project doesn't model integer-overflow traps, so DADD/DSUB
 * and DADDU/DSUBU are expected to be numerically identical - only the
 * opcode dispatch was missing before this round's fix).
 *
 * Not part of tests/README.md's suite (that suite's per-test compile
 * commands are separately stale, pre-existing task #554-class issue,
 * unrelated to this fix) - this is a direct, minimal, honest check of
 * exactly what changed.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"

/* MIPS encoding helper: SPECIAL opcode (op=0), rs,rt,rd,sa,funct */
static uint32_t enc_special(int rs, int rt, int rd, int sa, int funct) {
    return (0u << 26) | ((rs & 0x1F) << 21) | ((rt & 0x1F) << 16) |
           ((rd & 0x1F) << 11) | ((sa & 0x1F) << 6) | (funct & 0x3F);
}

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = calloc(1, 4 * 1024 * 1024);
    bios.size = 4 * 1024 * 1024;
    bios.loaded = 1;

    if (system_init(&bios, &bios) != 0) { printf("[FAIL] system_init\n"); return 1; }
    ee_state_t *ee = ee_core_get_state();

    /* Place instructions directly at a scratch PC and single-step via
     * ee_core_step(); use registers $t0(rs=8),$t1(rt=9),$t2(rd=10).
     * Must be a KSEG0 address (0x80000000+) - KUSEG addresses go
     * through TLB translation (ee_mem_read32 -> ee_mem_ptr) and this
     * scratch driver installs no TLB entries, so a KUSEG pc silently
     * TLB-misses (redirects pc to the exception vector, no halt, no
     * error return) instead of fetching our instruction - confirmed
     * by first attempting base=0x00100000 (KUSEG) and observing both
     * results silently stay at the sentinel value. */
    uint32_t base = 0x80100000;
    /* DADD $t2, $t0, $t1  (funct=0x2C) */
    *(uint32_t*)(ee->ram + (base & 0x1FFFFFFF)) = enc_special(8, 9, 10, 0, 0x2C);
    /* DSUB $t2, $t0, $t1  (funct=0x2E) */
    *(uint32_t*)(ee->ram + ((base+4) & 0x1FFFFFFF)) = enc_special(8, 9, 10, 0, 0x2E);
    /* two NOPs to land safely */
    *(uint32_t*)(ee->ram + ((base+8) & 0x1FFFFFFF)) = 0;
    *(uint32_t*)(ee->ram + ((base+12) & 0x1FFFFFFF)) = 0;

    ee->pc = base;
    ee->next_pc = base + 4;
    ee->gpr[8].ud0 = 100;   /* $t0 = 100 */
    ee->gpr[9].ud0 = 30;    /* $t1 = 30 */
    ee->gpr[10].ud0 = 0xdeadbeef; /* $t2, sentinel */

    printf("[DEBUG] before step: pc=0x%08x next_pc=0x%08x instr@pc=0x%08x\n",
           ee->pc, ee->next_pc, *(uint32_t*)(ee->ram + (base & 0x1FFFFFFF)));

    int rc1 = ee_core_step();
    uint64_t dadd_result = ee->gpr[10].ud0;
    int fail = 0;
    printf("[DEBUG] after DADD step: rc1=%d pc=0x%08x\n", rc1, ee->pc);
    if (rc1 != 0) { printf("[FAIL] DADD step returned %d (halted)\n", rc1); fail = 1; }
    else if (dadd_result != 130) { printf("[FAIL] DADD: expected 130, got %llu\n", (unsigned long long)dadd_result); fail = 1; }
    else printf("[PASS] DADD $t2,$t0,$t1 -> %llu (100+30)\n", (unsigned long long)dadd_result);

    ee->gpr[10].ud0 = 0xdeadbeef;
    int rc2 = ee_core_step();
    printf("[DEBUG] after DSUB step: rc2=%d pc=0x%08x\n", rc2, ee->pc);
    uint64_t dsub_result = ee->gpr[10].ud0;
    if (rc2 != 0) { printf("[FAIL] DSUB step returned %d (halted)\n", rc2); fail = 1; }
    else if (dsub_result != 70) { printf("[FAIL] DSUB: expected 70, got %llu\n", (unsigned long long)dsub_result); fail = 1; }
    else printf("[PASS] DSUB $t2,$t0,$t1 -> %llu (100-30)\n", (unsigned long long)dsub_result);

    printf(fail ? "[R776-DADD-DSUB-CHECK] FAIL\n" : "[R776-DADD-DSUB-CHECK] PASS\n");
    return fail;
}
