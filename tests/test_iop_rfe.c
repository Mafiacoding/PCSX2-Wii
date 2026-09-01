/*
 * test_iop_rfe.c - host-native test for iop_core.c's RFE (Restore
 * From Exception, COP0 CO-format funct=0x10) implementation.
 *
 * Background (see docs/STATUS.md "Round 22"): while investigating
 * the user-directed "all IOP problems" sweep, direct code inspection
 * of iop_core.c's COP0 dispatch found it only ever handled MFC0/MTC0
 * (rs=0x00/0x04) - any CO-format op (rs with bit 0x10 set, e.g. real
 * RFE) fell through to an "unimplemented COP0 sub-opcode" halt. Since
 * every real MIPS I exception handler ends in RFE before returning
 * (restoring the pre-exception KU/IE mode stack and re-enabling
 * interrupts if they were enabled beforehand), this was a genuine gap
 * that would have silently masked forward progress the moment any
 * real handler actually ran to completion and tried to return - a
 * strong root-cause candidate for why Status.IEc has never been
 * observed to become 1 anywhere in this project's traced execution.
 *
 * Fix ported from PCSX2's R3000A.cpp psxException()'s RFE case:
 *   Status = (Status & ~0xF) | ((Status & 0x3C) >> 2)
 * which shifts the "previous"/"old" KU/IE bit-pairs down into
 * "current"/"previous", leaving the top "old" pair (bits 4-5)
 * untouched - the mirror of the exception-entry push this project
 * already implements ((Status & 0x0F) << 2 into bits 2-5, bits 0-1
 * cleared).
 *
 * This test hand-verifies the exact bit pattern through one full
 * exception-entry-then-RFE round trip:
 *   initial Status low6 = 0b000101 (IEc=1,KUc=0,IEp=1,KUp=0,IEo=0,KUo=0)
 *   after SYSCALL push:  0b010100 (IEc=0,KUc=0,IEp=1,KUp=0,IEo=1,KUo=0)
 *   after RFE:            0b010101 (IEc=1,KUc=0,IEp=1,KUp=0,IEo=1,KUo=0)
 * (BEV, bit 22/0x400000, is carried through unchanged in the test
 * value throughout, so bootstrap vectoring stays deterministic.)
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include "core/iop/iop_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

/* Task #735 test-harness compatibility helper - see test_iop_core.c
 * for the full rationale. Drop-in replacement for iop_core_run() that
 * preserves the pre-existing syscall-recovery special case inside
 * the BREAK handler (task #156) and freezes execution in exactly the
 * state that existed the instant a genuine BREAK was reached,
 * undoing the (now real) Breakpoint-exception delivery it would
 * otherwise leave behind. This matters especially here: this test's
 * whole point is inspecting Cause/Status exactly as RFE left them,
 * one instruction before the trailing BREAK. */
static void iop_run_until_break(void) {
    iop_state_t *st = iop_core_get_state();
    long guard;
    for (guard = 0; guard < 2000000L; guard++) {
        if (st->halted) return;
        uint32_t pre_pc = st->pc;
        uint32_t pre_next_pc = st->next_pc;
        uint32_t pre_status = st->cop0[12];
        uint32_t pre_cause = st->cop0[13];
        uint32_t pre_epc = st->cop0[14];
        uint8_t  pre_pending = st->exception_pending;
        uint64_t pre_count = st->instructions_executed;

        if (iop_step()) return; /* genuine halt - not a BREAK, leave as-is */

        if (st->exception_pending && (st->cop0[13] & 0x7Cu) == 0x24u && st->cop0[14] == pre_pc) {
            st->cop0[12] = pre_status;
            st->cop0[13] = pre_cause;
            st->cop0[14] = pre_epc;
            st->exception_pending = pre_pending;
            st->pc = pre_pc;
            st->next_pc = pre_next_pc;
            st->instructions_executed = pre_count;
            st->halted = 1;
            snprintf(st->halt_reason, sizeof(st->halt_reason),
                     "BREAK (task #735: real Breakpoint exception raised and unwound by the test harness, ExcCode 9)");
            return;
        }
    }
    st->halted = 1;
    snprintf(st->halt_reason, sizeof(st->halt_reason),
             "iop_run_until_break() safety cap reached without a Breakpoint exception");
}

static uint32_t enc_lui(uint32_t rt, uint32_t imm16)            { return (0x0Fu<<26) | (rt<<16) | (imm16 & 0xFFFFu); }
static uint32_t enc_ori(uint32_t rt, uint32_t rs, uint32_t imm16){ return (0x0Du<<26) | (rs<<21) | (rt<<16) | (imm16 & 0xFFFFu); }
static uint32_t enc_mtc0(uint32_t rt, uint32_t rd)               { return (0x10u<<26) | (0x04u<<21) | (rt<<16) | (rd<<11); }
static uint32_t enc_rfe(void)   { return (0x10u<<26) | (0x10u<<21) | 0x10u; } /* 0x42000010 */
static uint32_t enc_syscall(void) { return 0x0Cu; }
static uint32_t enc_break(void)   { return 0x0Du; }

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    uint8_t *p = bios.data;
    const uint32_t T0 = 8;
    const uint32_t INIT_STATUS = 0x00400005u; /* BEV=1, low6=0b000101 */

    /* Reset vector (0xBFC00000): load INIT_STATUS into $t0, write it
     * to Status via MTC0, then SYSCALL (traps to the bootstrap vector
     * since BEV=1). */
    wle32(p + 0x000, enc_lui(T0, INIT_STATUS >> 16));
    wle32(p + 0x004, enc_ori(T0, T0, INIT_STATUS & 0xFFFFu));
    wle32(p + 0x008, enc_mtc0(T0, 12));
    wle32(p + 0x00C, enc_syscall());

    /* Bootstrap exception vector (0xBFC00180): RFE, then BREAK so the
     * test can observe whether execution actually continued past RFE
     * (if RFE were still unimplemented, the core would halt on the
     * RFE instruction itself with a distinguishable "unimplemented
     * COP0 CO-format op" reason instead of reaching this BREAK). */
    wle32(p + 0x180, enc_rfe());
    wle32(p + 0x184, enc_break());

    iop_core_init(&bios);
    iop_state_t *st = iop_core_get_state();

    iop_run_until_break();

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core reached and halted on BREAK *after* RFE (RFE did not halt as 'unimplemented')");
    CHECK((st->cop0[13] & 0x7Fu) == 0x20u,
          "Cause.ExcCode is still 8 (Syscall) from the earlier trap - RFE does not touch Cause");
    CHECK(st->cop0[12] == 0x00400015u,
          "Status == 0x00400015 after SYSCALL-push-then-RFE: low6 0b010101 "
          "(IEc=1,KUc=0,IEp=1,KUp=0,IEo=1,KUo=0) - exactly the real R3000A "
          "KU/IE mode-stack round trip, matching PCSX2's psxException() RFE formula");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
