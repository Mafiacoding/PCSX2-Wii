/*
 * test_ee_syscall_alarm_eventflag_tlb.c - host-native test for Round
 * 192 (task #358): continuing Round 187's fresh full EE syscall-table
 * audit, found three more real families unhandled (silently halting
 * the whole machine via the generic "no BIOS syscall table
 * implemented" fallback if ever reached):
 *
 *   Alarm family:      _SetAlarm(24), _ReleaseAlarm(25), SetAlarm(252),
 *                       ReleaseAlarm(254), and their real fast/
 *                       interrupt-context forms _iSetAlarm(-30),
 *                       _iReleaseAlarm(-31), iSetAlarm(-253),
 *                       iReleaseAlarm(-255).
 *   EventFlag family:  CreateEventFlag(80), DeleteEventFlag(81),
 *                       SetEventFlag(82), iSetEventFlag(83).
 *   TLB-wrapper family: PutTLBEntry(85), _SetTLBEntry(86),
 *                       GetTLBEntry(87), ProbeTLBEntry(88), and their
 *                       real fast/interrupt-context forms
 *                       iPutTLBEntry(-85), iSetTLBEntry(-86),
 *                       iGetTLBEntry(-87), iProbeTLBEntry(-88).
 *
 * All real, cited numeric slots from ps2sdk's own
 * ee/kernel/include/syscallnr.h (this project's local cached copy,
 * /tmp/ps2sdk/ps2sdk-master). Same established fix pattern as syscalls
 * 6/7/16/17/18/19/33-57/124: raise a real MIPS Syscall exception
 * (ExcCode 8) instead of halting, so genuine BIOS-resident kernel code
 * runs rather than this project guessing at internal bookkeeping.
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

/* sysnum lives in $v1 (GPR 3), real EE convention. */
static void run_syscall_test(int32_t sysnum, const char *label) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_addiu(3, 0, (int16_t)sysnum)); pc += 4; /* $v1 = sysnum */
    uint32_t syscall_pc = pc;
    wle32(p+pc, enc_syscall());                    pc += 4; /* SYSCALL */
    wle32(p+pc, 0x0u);                              pc += 4; /* delay slot: NOP */

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    uint32_t base_pc = st->pc;

    ee_core_step(); /* ADDIU $v1, sysnum */
    ee_core_step(); /* SYSCALL - should raise a real exception */

    char msg[128];
    snprintf(msg, sizeof(msg), "%s: halted must remain 0 (not the old halt() behavior)", label);
    CHECK(st->halted == 0, msg);

    snprintf(msg, sizeof(msg), "%s: Cause.ExcCode == Syscall (8)", label);
    CHECK((st->cop0[13] & 0x7Cu) == EE_EXC_CODE_SYS, msg);

    snprintf(msg, sizeof(msg), "%s: EPC points at the SYSCALL instruction itself", label);
    CHECK(st->cop0[14] == base_pc + syscall_pc, msg);

    snprintf(msg, sizeof(msg), "%s: pc vectors to the general exception offset (0xBFC00380, BEV=1 reset default)", label);
    CHECK(st->pc == 0xBFC00380u, msg);
}

int main(void) {
    /* Alarm family */
    run_syscall_test(24, "_SetAlarm (24/0x18)");
    run_syscall_test(25, "_ReleaseAlarm (25/0x19)");
    run_syscall_test(252, "SetAlarm (252/0xfc)");
    run_syscall_test(254, "ReleaseAlarm (254/0xfe)");
    run_syscall_test(-30, "_iSetAlarm (-30/-0x1e)");
    run_syscall_test(-31, "_iReleaseAlarm (-31/-0x1f)");
    run_syscall_test(-253, "iSetAlarm (-253/-0xfd)");
    run_syscall_test(-255, "iReleaseAlarm (-255/-0xff)");

    /* EventFlag family */
    run_syscall_test(80, "CreateEventFlag (80/0x50)");
    run_syscall_test(81, "DeleteEventFlag (81/0x51)");
    run_syscall_test(82, "SetEventFlag (82/0x52)");
    run_syscall_test(83, "iSetEventFlag (83/0x53)");

    /* TLB-wrapper family */
    run_syscall_test(85, "PutTLBEntry (85/0x55)");
    run_syscall_test(86, "_SetTLBEntry (86/0x56)");
    run_syscall_test(87, "GetTLBEntry (87/0x57)");
    run_syscall_test(88, "ProbeTLBEntry (88/0x58)");
    run_syscall_test(-85, "iPutTLBEntry (-85/-0x55)");
    run_syscall_test(-86, "iSetTLBEntry (-86/-0x56)");
    run_syscall_test(-87, "iGetTLBEntry (-87/-0x57)");
    run_syscall_test(-88, "iProbeTLBEntry (-88/-0x58)");

    /* Regression check: real native COP0 TLB instructions (TLBWI)
     * this project already implements at the hardware level (task
     * #60) must remain completely unaffected by this round's new
     * syscall-level wrapper handling - these are a different opcode
     * class entirely (COP0 CO-format, not SYSCALL), so there should
     * be no interaction, but this is asserted explicitly rather than
     * assumed. */
    {
        bios_image_t bios = make_bios();
        uint8_t *p = bios.data;
        /* MTC0 $zero, Index (cop0 reg 0) then TLBWI (cop0 CO-format,
         * funct 0x02) - a real, already-modeled hardware op, not a
         * syscall. */
        wle32(p+0, (0x10u << 26) | (4u << 21) | (0u << 16)); /* mtc0 $zero, $0 (Index) */
        wle32(p+4, (0x10u << 26) | (0x10u << 21) | 0x02u);    /* cop0 CO-format: TLBWI */
        ee_core_init(&bios);
        ee_state_t *st = ee_core_get_state();
        ee_core_step();
        ee_core_step();
        CHECK(st->halted == 0, "TLBWI regression: real hardware TLB op still not halted");
        CHECK((st->cop0[13] & 0x7Cu) != EE_EXC_CODE_SYS, "TLBWI regression: real hardware TLB op does NOT raise a syscall exception");
    }

    if (failures == 0)
        printf("ALL CHECKS PASSED (0 failures)\n");
    else
        printf("%d check(s) FAILED\n", failures);
    return failures != 0;
}
