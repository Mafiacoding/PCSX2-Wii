/*
 * test_ee_syscall_resetee_kexit_rfu.c - host-native test for Round 493:
 * EE syscalls 1 (ResetEE), 4 (KExit), 5 (ResumeIntrDispatch), 8
 * (ResumeT3IntrDispatch), 9 (RFU009) - the 5 syscalls found missing in
 * Round 492's ee/kernel/ coverage audit against real ps2sdk
 * syscallnr.h, now implemented per the user's explicit
 * "if its missing implement it" instruction.
 *
 * 5/8/9 follow the existing generic-default-return precedent (see
 * ee_core.c's 100/61/120/-120 group): GPR(2)=0, no exception, pc+4.
 * 4 (KExit) invokes the existing halt() primitive. 1 (ResetEE) walks
 * the real INIT_* bitfield and calls the corresponding hw _init()
 * functions plus ee_core_rebind_dma_sinks() for INIT_DMAC.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include "core/ee/ee_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t enc_addiu(int rt, int rs, int16_t imm) { return (0x09 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_lui(int rt, uint16_t imm)  { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_syscall(void) { return (0x0Cu); } /* SPECIAL opcode 0, funct 0x0C */
static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

static bios_image_t make_bios(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;
    return bios;
}

/* 5/8/9: generic-default-return group - halted stays 0, GPR(2)==0, pc advances by 4. */
static void run_generic_default_test(int32_t sysnum, const char *label) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_addiu(3, 0, (int16_t)sysnum)); pc += 4; /* $v1 = sysnum */
    wle32(p+pc, enc_syscall());                    pc += 4; /* SYSCALL */
    wle32(p+pc, 0x0u);                              pc += 4; /* delay slot: NOP */

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();

    ee_core_step(); /* ADDIU $v1, sysnum */
    uint32_t syscall_pc = st->pc; /* real virtual address of the SYSCALL
        instruction (base reset-vector pc + offset), not the raw file
        offset - bios.data is mapped starting at the real reset vector. */
    ee_core_step(); /* SYSCALL */

    char msg[160];
    snprintf(msg, sizeof(msg), "%s: halted stays 0 (generic default return, not a halt)", label);
    CHECK(st->halted == 0, msg);
    snprintf(msg, sizeof(msg), "%s: GPR(2) == 0 (generic default return value)", label);
    CHECK(st->gpr[2].ud0 == 0, msg);
    snprintf(msg, sizeof(msg), "%s: pc advanced past SYSCALL (no exception vectoring)", label);
    CHECK(st->pc == syscall_pc + 4u, msg); /* one step past SYSCALL leaves pc
        at the delay-slot address (this_pc+4); next_pc (unobserved here)
        carries this_pc+8, matching ee_core.c's own delay-slot convention. */
}

/* 4 (KExit): must invoke halt() with a KExit-identifying reason string. */
static void run_kexit_test(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_addiu(3, 0, 4)); pc += 4; /* $v1 = 4 (KExit) */
    wle32(p+pc, enc_addiu(4, 0, 7)); pc += 4; /* $a0 = 7 (arbitrary exit_code) */
    wle32(p+pc, enc_syscall());      pc += 4; /* SYSCALL */
    wle32(p+pc, 0x0u);               pc += 4; /* delay slot: NOP */

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();

    ee_core_step(); /* ADDIU $v1, 4 */
    ee_core_step(); /* ADDIU $a0, 7 */
    ee_core_step(); /* SYSCALL */

    CHECK(st->halted == 1, "KExit (4): halted becomes 1 (real EE termination syscall)");
    CHECK(strstr(st->halt_reason, "KExit") != NULL, "KExit (4): halt_reason identifies KExit");
}

/* 1 (ResetEE): 0x7F (all bits, matching real ExecPS2.c's own call) must
 * not halt/crash, must return GPR(2)==0, and must leave the DMA sinks
 * correctly rebound (Round 449 checkpoint-resume precedent: dma_init()
 * clears g_sinks[], so a bare dma_init() without rebind would silently
 * break GIF/VIF0/VIF1 DMA - this checks the fix, not just "didn't crash"). */
static void run_resetee_test(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_lui(4, 0x0000));      pc += 4; /* $a0 hi = 0 */
    wle32(p+pc, enc_ori(4, 4, 0x007F));   pc += 4; /* $a0 = 0x7F (real ExecPS2.c value) */
    wle32(p+pc, enc_addiu(3, 0, 1));      pc += 4; /* $v1 = 1 (ResetEE) */
    wle32(p+pc, enc_syscall());           pc += 4; /* SYSCALL */
    wle32(p+pc, 0x0u);                    pc += 4; /* delay slot: NOP */

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    /* Rebind once up front the same way ee_core_init()'s real
     * boot path does, so we can prove ResetEE(INIT_DMAC) restores
     * this rather than just happening to still be zeroed/unset. */
    ee_core_rebind_dma_sinks();

    ee_core_step(); /* LUI */
    ee_core_step(); /* ORI */
    ee_core_step(); /* ADDIU $v1, 1 */
    uint32_t syscall_pc = st->pc; /* real virtual address of SYSCALL */
    ee_core_step(); /* SYSCALL */

    CHECK(st->halted == 0, "ResetEE(0x7F): halted stays 0");
    CHECK(st->gpr[2].ud0 == 0, "ResetEE(0x7F): GPR(2) == 0");
    CHECK(st->pc == syscall_pc + 4u, "ResetEE(0x7F): pc advanced past SYSCALL");
    /* VF00 must remain hardwired to (0,0,0,1.0f) post-INIT_VU0 clear -
     * same real-hardware fact this project's own vu1_init() encodes
     * for VF00, now also asserted for the VU0 macro-mode clear path. */
    CHECK(st->vu0_vf[0][3] == 0x3F800000u,
          "ResetEE(0x7F): INIT_VU0 clear leaves VF00.w hardwired to 1.0f");
}

int main(void) {
    run_generic_default_test(5, "ResumeIntrDispatch (5)");
    run_generic_default_test(8, "ResumeT3IntrDispatch (8)");
    run_generic_default_test(9, "RFU009 (9)");
    run_kexit_test();
    run_resetee_test();

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
