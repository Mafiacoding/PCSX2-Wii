/*
 * test_ee_syscall_enableintc.c - host-native test for Round 188
 * (task #354): EE syscalls 20 (_EnableIntc) / 21 (_DisableIntc) must
 * set/clear the corresponding bit in the real EE_INTC MASK register
 * end state (not halt the machine, the previous, incorrect fallthrough
 * behavior found by Round 187's audit) - implemented via direct
 * software model (like the existing _EnableDmac(22)/_DisableDmac(23)
 * precedent), NOT via the "raise a real exception" pattern (unlike
 * AddIntcHandler/16-17), since this project already fully owns the
 * real INTC_MASK register state.
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
static uint32_t enc_syscall(void) { return (0x0Cu); }
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

/* sysnum in $v1 (GPR3), cause arg in $a0 (GPR4), matching the real EE
 * syscall convention already established throughout this test suite. */
static void run_enable_disable_test(int32_t sysnum, uint32_t cause, const char *label) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_addiu(3, 0, (int16_t)sysnum)); pc += 4; /* $v1 = sysnum */
    wle32(p+pc, enc_addiu(4, 0, (int16_t)cause));  pc += 4; /* $a0 = cause */
    wle32(p+pc, enc_syscall());                    pc += 4; /* SYSCALL */
    wle32(p+pc, 0x0u);                              pc += 4; /* delay slot: NOP */

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    ee_intc_state_t *intc = ee_intc_get_state();

    ee_core_step(); /* ADDIU $v1, sysnum */
    ee_core_step(); /* ADDIU $a0, cause */
    ee_core_step(); /* SYSCALL */

    char msg[160];
    snprintf(msg, sizeof(msg), "%s: halted must remain 0", label);
    CHECK(st->halted == 0, msg);

    snprintf(msg, sizeof(msg), "%s: pc did NOT vector to the exception handler (unlike 16/17/33-57)", label);
    CHECK(st->pc != 0xBFC00380u, msg);

    if (sysnum == 20) {
        snprintf(msg, sizeof(msg), "%s: INTC_MASK bit %u is now SET", label, cause);
        CHECK((intc->mask & (1u << cause)) != 0, msg);
    } else {
        snprintf(msg, sizeof(msg), "%s: INTC_MASK bit %u is now CLEAR", label, cause);
        CHECK((intc->mask & (1u << cause)) == 0, msg);
    }

    snprintf(msg, sizeof(msg), "%s: return value (v0) is 0", label);
    CHECK(st->gpr[2].ud0 == 0, msg);
}

int main(void) {
    run_enable_disable_test(20, 2, "_EnableIntc(VBLANK_S=2)");
    run_enable_disable_test(20, 0, "_EnableIntc(GS=0)");

    /* explicit enable-then-disable sequence on the same cause */
    {
        bios_image_t bios = make_bios();
        uint8_t *p = bios.data;
        int pc = 0;
        wle32(p+pc, enc_addiu(3, 0, 20)); pc += 4;
        wle32(p+pc, enc_addiu(4, 0, 3));  pc += 4; /* cause = VBLANK_E */
        wle32(p+pc, enc_syscall());       pc += 4;
        wle32(p+pc, 0x0u);                pc += 4;
        wle32(p+pc, enc_addiu(3, 0, 21)); pc += 4;
        wle32(p+pc, enc_addiu(4, 0, 3));  pc += 4;
        wle32(p+pc, enc_syscall());       pc += 4;
        wle32(p+pc, 0x0u);                pc += 4;

        ee_core_init(&bios);
        ee_state_t *st = ee_core_get_state();
        ee_intc_state_t *intc = ee_intc_get_state();

        ee_core_step(); ee_core_step(); ee_core_step(); /* _EnableIntc(3) */
        CHECK((intc->mask & (1u << 3)) != 0, "sequence: bit 3 set after _EnableIntc(3)");
        ee_core_step(); /* SYSCALL's own delay-slot NOP (pc=12) must be
                          * stepped explicitly before the next real
                          * instruction stream begins - same delay-slot
                          * convention this file's own syscall handler
                          * (st->pc = this_pc+4 / next_pc = this_pc+8)
                          * already uses elsewhere. */
        ee_core_step(); ee_core_step(); ee_core_step(); /* _DisableIntc(3) */
        CHECK((intc->mask & (1u << 3)) == 0, "sequence: bit 3 cleared after _DisableIntc(3)");
        CHECK(st->halted == 0, "sequence: not halted throughout");
    }

    /* Regression check: 22/23 (_EnableDmac/_DisableDmac) unaffected. */
    {
        bios_image_t bios = make_bios();
        uint8_t *p = bios.data;
        int pc = 0;
        wle32(p+pc, enc_addiu(3, 0, 22)); pc += 4;
        wle32(p+pc, enc_addiu(4, 0, 5));  pc += 4; /* DMA_CHANNEL_SIF0 */
        wle32(p+pc, enc_syscall());       pc += 4;
        wle32(p+pc, 0x0u);                pc += 4;
        ee_core_init(&bios);
        ee_state_t *st = ee_core_get_state();
        ee_core_step(); ee_core_step(); ee_core_step();
        CHECK(st->halted == 0, "_EnableDmac(22) regression: not halted");
        CHECK(st->gpr[2].ud0 == 0, "_EnableDmac(22) regression: returns 0");
    }

    if (failures == 0)
        printf("ALL CHECKS PASSED (0 failures)\n");
    else
        printf("%d check(s) FAILED\n", failures);
    return failures != 0;
}
