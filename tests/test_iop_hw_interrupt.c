/*
 * test_iop_hw_interrupt.c - host-native test for iop_core.c's real
 * hardware-interrupt delivery (Round 22, see docs/STATUS.md).
 *
 * Background: while sweeping "all IOP problems" per the user's
 * explicit directive, iop_intc.c's own scope comment already flagged
 * that nothing in iop_core.c ever raised a real CPU interrupt/
 * exception when I_STAT & I_MASK became nonzero - meaning
 * Status.IEc had no observable effect anywhere in this project,
 * regardless of its value. This is the fix: iop_check_hw_interrupt()
 * in iop_core.c, called at the end of every real instruction step.
 *
 * Cited from the public psx-spx reference
 * (https://psx-spx.consoledev.net/interrupts/, explicitly noted there
 * as applying to the PS2 IOP too): the IOP/PS1 architecture routes
 * every peripheral IRQ through ONE single CPU interrupt line -
 * Cause.bit10 (IP2) mirrors "(I_STAT AND I_MASK)=nonzero" live and
 * NON-latching (auto-clears the instant that condition goes false),
 * and the interrupt is actually taken only once Status.bit10 (IM2)
 * AND Status.bit0 (IEc) are ALSO both set.
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
 * otherwise leave behind. This is especially important in THIS file:
 * these tests inspect Cause/EPC/Status left behind by an EARLIER,
 * already-tested real hardware-interrupt exception, which a naive
 * "keep running past BREAK" replacement would silently overwrite. */
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

static uint32_t enc_lui(uint32_t rt, uint32_t imm16)             { return (0x0Fu<<26) | (rt<<16) | (imm16 & 0xFFFFu); }
static uint32_t enc_ori(uint32_t rt, uint32_t rs, uint32_t imm16) { return (0x0Du<<26) | (rs<<21) | (rt<<16) | (imm16 & 0xFFFFu); }
static uint32_t enc_mtc0(uint32_t rt, uint32_t rd)                { return (0x10u<<26) | (0x04u<<21) | (rt<<16) | (rd<<11); }
static uint32_t enc_sw(uint32_t rt, uint32_t rs, uint32_t imm16)  { return (0x2Bu<<26) | (rs<<21) | (rt<<16) | (imm16 & 0xFFFFu); }
static uint32_t enc_break(void)   { return 0x0Du; }

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

/* $t0=8, $t1=9, $t2=10, $s0=16, $s1=17 */

int main(void) {
    /* --- Test 1: interrupt fires when IEc=1, IM2=1, and I_STAT&I_MASK
     * becomes nonzero (via a real SW to I_MASK) --- */
    {
        bios_image_t bios;
        memset(&bios, 0, sizeof(bios));
        bios.data = memalign(32, BIOS_MAX_SIZE);
        memset(bios.data, 0, BIOS_MAX_SIZE);
        bios.size = BIOS_MAX_SIZE;
        bios.loaded = 1;
        uint8_t *p = bios.data;

        wle32(p + 0x00, enc_lui(8, 0x0040));
        wle32(p + 0x04, enc_ori(8, 8, 0x0401)); /* $t0 = 0x00400401: BEV=1,IM2=1,IEc=1 */
        wle32(p + 0x08, enc_mtc0(8, 12));
        wle32(p + 0x0C, enc_lui(9, 0x1F80));
        wle32(p + 0x10, enc_ori(9, 9, 0x1074)); /* $t1 = 0x1F801074 (I_MASK) */
        wle32(p + 0x14, enc_lui(10, 0x0000));
        wle32(p + 0x18, enc_ori(10, 10, 0x0001)); /* $t2 = 1 (enable IRQ bit 0) */
        wle32(p + 0x1C, enc_sw(10, 9, 0)); /* SW $t2, 0($t1) - enables the line */
        wle32(p + 0x20, enc_ori(16, 0, 0x1111)); /* marker: must NOT execute */
        wle32(p + 0x24, enc_ori(17, 0, 0x2222)); /* marker: must NOT execute */
        wle32(p + 0x180, enc_break()); /* bootstrap vector */

        iop_core_init(&bios);
        iop_state_t *st = iop_core_get_state();
        iop_intc_raise(0); /* simulate a peripheral asserting IRQ 0 (I_STAT bit 0) before I_MASK enables it */

        iop_run_until_break();

        CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
              "core took the interrupt and halted on BREAK at the bootstrap vector (0xBFC00180)");
        CHECK(st->cop0[14] == 0xBFC00020u,
              "EPC == 0xBFC00020 - the marker instruction right after the enabling SW, correctly preempted");
        CHECK(st->gpr[16] == 0 && st->gpr[17] == 0,
              "both marker instructions after the SW were correctly never executed");
        CHECK((st->cop0[13] & 0x7Fu) == 0u,
              "Cause.ExcCode == 0 (Interrupt), not left over from anything else");
        CHECK((st->cop0[13] & IOP_CAUSE_IP2) != 0u,
              "Cause.IP2 (bit 10) is set, mirroring I_STAT & I_MASK == nonzero");
        CHECK(st->cop0[12] == 0x00400404u,
              "Status == 0x00400404 after the push: IEc correctly cleared (bit0=0), "
              "IEp correctly set to the pre-exception IEc (bit2=1), BEV/IM2 untouched");
    }

    /* --- Test 2: no interrupt when Status.IEc=0, even with I_STAT &
     * I_MASK already nonzero from the very start --- */
    {
        bios_image_t bios;
        memset(&bios, 0, sizeof(bios));
        bios.data = memalign(32, BIOS_MAX_SIZE);
        memset(bios.data, 0, BIOS_MAX_SIZE);
        bios.size = BIOS_MAX_SIZE;
        bios.loaded = 1;
        uint8_t *p = bios.data;

        /* IEc left at 0 (reset default) - only IM2 gets set, deliberately
         * not enough on its own per the real gating rule. */
        wle32(p + 0x00, enc_lui(8, 0x0000));
        wle32(p + 0x04, enc_ori(8, 8, 0x0400)); /* $t0 = 0x00000400: IM2=1, IEc=0 */
        wle32(p + 0x08, enc_mtc0(8, 12));
        wle32(p + 0x0C, enc_ori(16, 0, 0x3333)); /* must execute normally */
        wle32(p + 0x10, enc_break());            /* expected halt point */
        wle32(p + 0x180, enc_break());            /* must NOT be reached */

        iop_core_init(&bios);
        iop_state_t *st = iop_core_get_state();
        iop_intc_raise(0); /* I_STAT bit 0 pending from the start */
        /* Enable the matching I_MASK bit directly (equivalent to the
         * real SW in test 1, just via the direct API for brevity here
         * since test 1 already covers the real MMIO write path). */
        iop_intc_get_state()->imask |= 1u;

        iop_run_until_break();

        CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
              "with IEc=0, core reached and halted on the BREAK at 0x10 (not the bootstrap vector)");
        CHECK(st->gpr[16] == 0x3333u,
              "the marker instruction at 0x0C executed normally - no interrupt preempted it despite I_STAT & I_MASK != 0");
        CHECK(st->instructions_executed == 4,
              "exactly 4 instructions counted (LUI, ORI, MTC0, ORI-marker) before halting on BREAK - "
              "proves the marker genuinely ran as the 4th real instruction, not skipped by a wrongly-taken interrupt");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
