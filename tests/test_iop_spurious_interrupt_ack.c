/*
 * test_iop_spurious_interrupt_ack.c - Round 935 (task #919) regression
 * test for the GT3 disc-boot SIF-RPC freeze root-cause fix.
 *
 * Background (see docs/STATUS.md's Round 935 entry for the full
 * citation trail): iop_check_hw_interrupt() (source/core/iop/
 * iop_core.c) falls back to the fixed exception vector
 * (0x80000080/0xBFC00180) whenever a real interrupt source is
 * pending+unmasked but NEITHER real dispatch mechanism
 * (RegisterIntrHandler table nor the older ExCB chain) has a handler
 * registered for it. Before this fix, iop_step()'s "Round 129/131
 * default/spurious-interrupt-return stub" resumed the interrupted
 * instruction at EPC without ever acknowledging/clearing the real
 * intc->istat/istat_hi bit(s) that caused the fallback - so the
 * identical interrupt refired the instant Status.IEc was restored,
 * forever, with zero forward progress (a real, deterministic,
 * self-sustaining 3-step storm, confirmed via direct instrumentation
 * against a live GT3 disc-boot checkpoint: 1,666,667 exceptions over
 * 5,000,000 IOP steps, EPC never advancing past a single fixed PC).
 *
 * The fix: iop_check_hw_interrupt() now records the exact
 * pending+unmasked bit(s) that failed both dispatch mechanisms into
 * g_iop_spurious_istat_mask/g_iop_spurious_istat_hi_mask immediately
 * before falling through to the vector; iop_step()'s spurious-stub
 * then clears exactly those bits out of the real intc->istat/istat_hi
 * registers before resuming - mirroring the already-correct pattern
 * source/hw/iop_hle_intr.c's IOP_HLE_INTR_HANDLER_RETURN_TRAMPOLINE
 * already uses when a REAL handler finishes servicing an IRQ.
 *
 * This test constructs the exact failure shape directly (a pending,
 * unmasked, unhandled low-range AND soft-range interrupt source, with
 * Status.IEc/IM2 set and BEV=0 so the RAM-resident vector applies) and
 * verifies: (1) the interrupt fires exactly once, (2) the specific
 * source bit(s) get cleared out of istat/istat_hi as part of the
 * resume, (3) the IOP makes real forward progress afterward instead of
 * re-vectoring to the same EPC forever, and (4) a handler-registered
 * IRQ is completely unaffected (still dispatches via the existing HLE
 * path and never touches the new spurious-ack bookkeeping) - a direct
 * regression guard against ever reintroducing the storm.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include "core/iop/iop_core.c"
#include "core/hw/iop_hle_intr.h"
#include "core/hw/iop_intc.h"
#include "core/hw/iop_dma.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static iop_state_t *fresh_state(void)
{
    static bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    iop_core_init(&bios);
    return iop_core_get_state();
}

#define TEST_PC 0x00010000u

int main(void)
{
    /* --- Case 1: a single unhandled low-range IRQ (e.g. irq=5) must
     * fire exactly once, get acknowledged, and let real execution
     * resume - not storm forever. --- */
    {
        iop_state_t *st = fresh_state();
        st->pc = TEST_PC;
        st->next_pc = TEST_PC + 4u;
        iop_mem_write32(st, TEST_PC, 0x00000000u);      /* NOP */
        iop_mem_write32(st, TEST_PC + 4u, 0x00000000u); /* NOP */
        iop_mem_write32(st, TEST_PC + 8u, 0x00000000u); /* NOP */
        st->cop0[12] = 0x401u; /* Status: IEc=1, IM2=1, BEV=0 (RAM vector) */
        st->cop0[13] = 0u;     /* Cause clear */

        iop_intc_state_t *intc = iop_intc_get_state();
        intc->istat = 0x20u;  /* irq 5 pending */
        intc->imask = 0x20u;  /* irq 5 unmasked */
        CHECK(iop_hle_intr_get_intr_handler(5) == 0, "irq 5 has no registered handler (precondition)");

        iop_core_step(); /* executes the NOP at TEST_PC, then the tail-of-step interrupt check fires */
        CHECK(st->pc == 0x80000080u, "unhandled irq 5 vectors to the RAM-resident exception vector");
        CHECK(st->exception_pending == 1, "exception_pending set after vectoring");
        CHECK(g_iop_spurious_istat_mask == 0x20u, "irq 5's bit recorded as the spurious-fallback mask");
        CHECK((intc->istat & 0x20u) != 0, "irq 5 bit still SET in istat immediately after vectoring (not yet acked)");

        uint32_t epc_before = st->cop0[14];
        iop_core_step(); /* runs the spurious-stub: should ack + resume */
        CHECK(st->exception_pending == 0, "exception_pending cleared by the spurious-stub");
        CHECK((intc->istat & 0x20u) == 0, "irq 5 bit CLEARED out of istat by the fix (the actual Round 935 bug)");
        CHECK(g_iop_spurious_istat_mask == 0, "spurious mask consumed/reset to 0 after ack");
        CHECK(st->pc == epc_before, "resumed execution at the original EPC (interrupted instruction re-run)");

        /* The critical regression guard: with the source bit cleared,
         * the SAME interrupt must NOT refire on subsequent steps - real
         * forward progress must occur instead of storming back to
         * 0x80000080 with the same EPC forever (the exact Round 935
         * GT3 disc-boot bug). */
        uint32_t pc_after_resume = st->pc;
        iop_core_step();
        CHECK(st->pc == pc_after_resume + 4u, "IOP advances past the resumed instruction (no re-storm)");
        iop_core_step();
        CHECK(st->pc == pc_after_resume + 8u, "IOP continues advancing normally (no re-storm)");
        CHECK(st->exception_pending == 0, "no further spurious exception re-fired");
    }

    /* --- Case 2: simultaneously-pending low-range AND soft-range
     * unhandled IRQs (the exact real GT3 shape: VBLANK_START bit 0 +
     * SIF0-DMA raw line bit 3 low-range, plus IOP_IRQ_DMA_SIF0 bit 42
     * soft-range) must all get acknowledged together, not just the
     * lowest bit. --- */
    {
        iop_state_t *st = fresh_state();
        st->pc = TEST_PC;
        st->next_pc = TEST_PC + 4u;
        iop_mem_write32(st, TEST_PC, 0x00000000u);
        iop_mem_write32(st, TEST_PC + 4u, 0x00000000u);
        st->cop0[12] = 0x401u;
        st->cop0[13] = 0u;

        iop_intc_state_t *intc = iop_intc_get_state();
        intc->istat = 0x9u;      /* bits 0 and 3 pending, low range */
        intc->imask = 0x9u;      /* both unmasked */
        intc->istat_hi = 0x400u; /* bit 10 (soft irq 42) pending */
        intc->imask_hi = 0x400u; /* unmasked */
        CHECK(iop_hle_intr_get_intr_handler(0) == 0 && iop_hle_intr_get_intr_handler(3) == 0 &&
              iop_hle_intr_get_intr_handler(42) == 0,
              "irqs 0, 3, and 42 all have no registered handler (precondition, matches real GT3 checkpoint)");

        iop_core_step();
        CHECK(st->pc == 0x80000080u, "combined low+soft pending unhandled irqs vector to the exception vector");
        CHECK(g_iop_spurious_istat_mask == 0x9u, "both low-range bits (0 and 3) recorded together");
        CHECK(g_iop_spurious_istat_hi_mask == 0x400u, "the soft-range bit (42) recorded too");

        iop_core_step(); /* spurious-stub ack + resume */
        CHECK((intc->istat & 0x9u) == 0, "both low-range bits cleared by the ack");
        CHECK((intc->istat_hi & 0x400u) == 0, "the soft-range bit cleared by the ack too");

        uint32_t pc_after_resume = st->pc;
        iop_core_step();
        CHECK(st->pc == pc_after_resume + 4u, "IOP advances normally after acking the combined pending set (no storm)");
    }

    /* --- Case 3: an IRQ that DOES have a real registered handler must
     * be completely unaffected by this fix - it should dispatch via
     * the existing HLE mechanism and never touch the new spurious-ack
     * bookkeeping at all (regression guard for the non-spurious,
     * already-correct path). --- */
    {
        iop_state_t *st = fresh_state();
        st->pc = TEST_PC;
        st->next_pc = TEST_PC + 4u;
        iop_mem_write32(st, TEST_PC, 0x00000000u);

        /* Register a real handler for irq 7 via the same HLE mechanism
         * test_iop_hle_intr.c already exercises directly. */
        st->gpr[4] = 7;
        st->gpr[5] = 0;
        st->gpr[6] = 0x00110000u;
        st->gpr[7] = 0x00001234u;
        st->gpr[31] = 0x00110040u;
        int handled = iop_hle_intr_try_handle(st, IOP_HLE_INTR_REGISTER_INTR_HANDLER);
        CHECK(handled == 1, "irq 7 handler registered via RegisterIntrHandler HLE for the control case");

        /* Restore a normal fetch context after the synthetic RegisterIntrHandler call above. */
        st->pc = TEST_PC;
        st->next_pc = TEST_PC + 4u;
        st->cop0[12] = 0x401u;
        st->cop0[13] = 0u;

        iop_intc_state_t *intc = iop_intc_get_state();
        intc->istat = 0x80u; /* irq 7 pending */
        intc->imask = 0x80u; /* unmasked */

        iop_core_step();
        CHECK(st->pc == 0x00110000u, "irq 7 (real registered handler) dispatches straight to the handler address, not the fallback vector");
        CHECK(g_iop_spurious_istat_mask == 0, "handler-registered irq never touches the spurious-ack mask");
        CHECK(g_iop_spurious_istat_hi_mask == 0, "handler-registered irq never touches the spurious-ack soft mask");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
