/*
 * test_iop_hle_intr.c - host-native test for the Round 109 clean-room
 * RegisterIntrHandler/RegisterExceptionHandler HLE dispatch table -
 * see include/core/hw/iop_hle_intr.h for the full design rationale.
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

int main(void)
{
    /* --- Sentinel matching: exactly the five real, cited
     * (library, ordinal) pairs, nothing else. --- */
    {
        CHECK(iop_hle_intr_sentinel_for_import("intrman", 4) == IOP_HLE_INTR_REGISTER_INTR_HANDLER,
              "intrman#4 -> RegisterIntrHandler sentinel");
        CHECK(iop_hle_intr_sentinel_for_import("intrman", 5) == IOP_HLE_INTR_RELEASE_INTR_HANDLER,
              "intrman#5 -> ReleaseIntrHandler sentinel");
        CHECK(iop_hle_intr_sentinel_for_import("excepman", 4) == IOP_HLE_INTR_REGISTER_EXCEPTION_HANDLER,
              "excepman#4 -> RegisterExceptionHandler sentinel");
        CHECK(iop_hle_intr_sentinel_for_import("excepman", 6) == IOP_HLE_INTR_REGISTER_DEFAULT_EXCEPTION_HANDLER,
              "excepman#6 -> RegisterDefaultExceptionHandler sentinel");
        CHECK(iop_hle_intr_sentinel_for_import("excepman", 7) == IOP_HLE_INTR_RELEASE_EXCEPTION_HANDLER,
              "excepman#7 -> ReleaseExceptionHandler sentinel");
        CHECK(iop_hle_intr_sentinel_for_import("intrman", 9) == 0,
              "intrman#9 (EnableIntr, unrelated) -> no sentinel");
        CHECK(iop_hle_intr_sentinel_for_import("sifman", 4) == 0,
              "unrelated library -> no sentinel");
        CHECK(iop_hle_intr_sentinel_for_import(NULL, 4) == 0,
              "NULL module name -> no sentinel (no crash)");
    }

    /* --- RegisterIntrHandler(irq=5, mode=0, handler=0x00110000, arg=0x00001234) --- */
    {
        iop_state_t *st = fresh_state();
        st->gpr[4] = 5;          /* a0 = irq */
        st->gpr[5] = 0;          /* a1 = mode */
        st->gpr[6] = 0x00110000u; /* a2 = handler */
        st->gpr[7] = 0x00001234u; /* a3 = arg */
        st->gpr[31] = 0x00110040u; /* ra = fake caller return address */

        int handled = iop_hle_intr_try_handle(st, IOP_HLE_INTR_REGISTER_INTR_HANDLER);
        CHECK(handled == 1, "RegisterIntrHandler sentinel recognized");
        CHECK(iop_hle_intr_get_intr_handler(5) == 0x00110000u, "irq 5 handler address stored");
        CHECK(st->gpr[2] == 0, "RegisterIntrHandler returns 0 (success)");
        CHECK(st->pc == 0x00110040u, "RegisterIntrHandler returns to caller's $ra");
        CHECK(st->next_pc == 0x00110044u, "next_pc = ra+4");

        /* --- ReleaseIntrHandler(irq=5) clears it --- */
        st->gpr[4] = 5;
        st->gpr[31] = 0x00110080u;
        handled = iop_hle_intr_try_handle(st, IOP_HLE_INTR_RELEASE_INTR_HANDLER);
        CHECK(handled == 1, "ReleaseIntrHandler sentinel recognized");
        CHECK(iop_hle_intr_get_intr_handler(5) == 0, "irq 5 handler cleared");
        CHECK(st->gpr[2] == 0, "ReleaseIntrHandler returns 0");

        /* --- Out-of-range irq is rejected, not silently accepted --- */
        st->gpr[4] = 999;
        st->gpr[6] = 0x00110000u;
        st->gpr[31] = 0x001100C0u;
        iop_hle_intr_try_handle(st, IOP_HLE_INTR_REGISTER_INTR_HANDLER);
        CHECK(st->gpr[2] == (uint32_t)-1, "out-of-range irq returns -1, not success");

        /* --- Round 111 regression guard: irq=0x2A/0x2B (real
         * IOP_IRQ_DMA_SIF0/IOP_IRQ_DMA_SIF1 per ps2sdk's cited
         * intrman.h enum) must NOT be rejected - a host-native
         * diagnostic run against the real SCPH-10000 BIOS this round
         * caught real module code registering exactly these two irq
         * numbers, which the original 32-entry table silently
         * dropped as "out of range". --- */
        st->gpr[4] = 0x2A;
        st->gpr[6] = 0x00150000u;
        st->gpr[31] = 0x00150040u;
        iop_hle_intr_try_handle(st, IOP_HLE_INTR_REGISTER_INTR_HANDLER);
        CHECK(st->gpr[2] == 0, "real IOP_IRQ_DMA_SIF0 (0x2A) registers successfully, not rejected");
        CHECK(iop_hle_intr_get_intr_handler(0x2A) == 0x00150000u, "IOP_IRQ_DMA_SIF0 handler stored");

        st->gpr[4] = 0x2B;
        st->gpr[6] = 0x00160000u;
        st->gpr[31] = 0x00160040u;
        iop_hle_intr_try_handle(st, IOP_HLE_INTR_REGISTER_INTR_HANDLER);
        CHECK(st->gpr[2] == 0, "real IOP_IRQ_DMA_SIF1 (0x2B) registers successfully, not rejected");
        CHECK(iop_hle_intr_get_intr_handler(0x2B) == 0x00160000u, "IOP_IRQ_DMA_SIF1 handler stored");

        /* IOP_IRQ_SW2 = 0x3F (63), the real, cited highest valid irq value. */
        st->gpr[4] = 0x3F;
        st->gpr[6] = 0x00170000u;
        st->gpr[31] = 0x00170040u;
        iop_hle_intr_try_handle(st, IOP_HLE_INTR_REGISTER_INTR_HANDLER);
        CHECK(st->gpr[2] == 0, "real IOP_IRQ_SW2 (0x3F, highest valid value) registers successfully");
    }

    /* --- RegisterExceptionHandler: real struct layout (next/info/
     * funccode[]) - funccode[0] at handler_ptr+8 is what gets
     * stored, read directly out of guest RAM the "caller" set up. --- */
    {
        iop_state_t *st = fresh_state();
        uint32_t struct_addr = 0x00050000u;
        iop_mem_write32(st, struct_addr + 0u, 0);          /* next */
        iop_mem_write32(st, struct_addr + 4u, 0);          /* info */
        iop_mem_write32(st, struct_addr + 8u, 0x00120000u); /* funccode[0] - real entry point */

        st->gpr[4] = 3;              /* a0 = exception class */
        st->gpr[5] = struct_addr;    /* a1 = handler struct ptr */
        st->gpr[31] = 0x00120040u;

        int handled = iop_hle_intr_try_handle(st, IOP_HLE_INTR_REGISTER_EXCEPTION_HANDLER);
        CHECK(handled == 1, "RegisterExceptionHandler sentinel recognized");
        CHECK(iop_hle_intr_get_exc_handler(3) == 0x00120000u, "exception class 3 handler = funccode[0]");
        CHECK(st->gpr[2] == 0, "RegisterExceptionHandler returns 0");

        /* --- ReleaseExceptionHandler clears it --- */
        st->gpr[4] = 3;
        st->gpr[31] = 0x00120080u;
        iop_hle_intr_try_handle(st, IOP_HLE_INTR_RELEASE_EXCEPTION_HANDLER);
        CHECK(iop_hle_intr_get_exc_handler(3) == 0, "exception class 3 handler cleared");

        /* --- RegisterDefaultExceptionHandler --- */
        uint32_t struct_addr2 = 0x00050100u;
        iop_mem_write32(st, struct_addr2 + 8u, 0x00130000u);
        st->gpr[4] = struct_addr2;
        st->gpr[31] = 0x00130040u;
        handled = iop_hle_intr_try_handle(st, IOP_HLE_INTR_REGISTER_DEFAULT_EXCEPTION_HANDLER);
        CHECK(handled == 1, "RegisterDefaultExceptionHandler sentinel recognized");
        CHECK(st->gpr[2] == 0, "RegisterDefaultExceptionHandler returns 0");
    }

    /* --- Full interrupt-dispatch round trip: a real handler IS
     * registered for the firing IRQ, so iop_check_hw_interrupt()
     * should jump straight into it instead of the fixed vector, and
     * the return trampoline should correctly restore EPC/Status and
     * acknowledge the IRQ once the handler "returns". --- */
    {
        iop_state_t *st = fresh_state();

        iop_intc_state_t *intc = iop_intc_get_state();
        memset(intc, 0, sizeof(*intc));
        intc->istat = 0x1u;  /* IRQ 0 pending */
        intc->imask = 0x1u;  /* IRQ 0 unmasked */

        st->gpr[4] = 0;               /* a0 = irq 0 */
        st->gpr[6] = 0x00140000u;     /* a2 = handler entry point */
        st->gpr[7] = 0xCAFEBABEu;     /* a3 = arg */
        st->gpr[31] = 0x00140080u;    /* fake caller ra, irrelevant to this test */
        iop_hle_intr_try_handle(st, IOP_HLE_INTR_REGISTER_INTR_HANDLER);
        CHECK(iop_hle_intr_get_intr_handler(0) == 0x00140000u, "irq 0 handler registered for dispatch test");

        /* Set up CPU state so the interrupt is actually deliverable:
         * Status.IEc=1, Status.IM2=1 (IOP_STATUS_IM2), BEV=0. */
        st->cop0[12] = 0x1u | IOP_STATUS_IM2;
        st->pc = 0x00160000u;
        st->next_pc = 0x00160004u;

        iop_check_hw_interrupt(st, 0x00160004u);

        CHECK(st->pc == 0x00140000u, "interrupt redirected straight into registered handler, not fixed vector");
        CHECK(st->gpr[4] == 0xCAFEBABEu, "handler called with real RegisterIntrHandler arg in $a0");
        CHECK(st->gpr[31] == IOP_HLE_INTR_HANDLER_RETURN_TRAMPOLINE, "handler's $ra set to this project's own return trampoline");

        /* Simulate the handler finishing with a real `jr $ra`. */
        int handled = iop_hle_intr_try_handle(st, st->gpr[31]);
        CHECK(handled == 1, "return trampoline recognized");
        CHECK(st->pc == 0x00160004u, "resumes at the real saved EPC after handler returns");
        CHECK((iop_intc_get_state()->istat & 0x1u) == 0, "IRQ 0 acknowledged (istat bit cleared) after real handling");
    }

    /* --- No handler registered: falls back to existing fixed-vector
     * default behavior, completely unchanged from before this round. --- */
    {
        iop_state_t *st = fresh_state();
        iop_intc_state_t *intc = iop_intc_get_state();
        memset(intc, 0, sizeof(*intc));
        intc->istat = 0x2u;  /* IRQ 1 pending, nothing registered for it */
        intc->imask = 0x2u;

        st->cop0[12] = 0x1u | IOP_STATUS_IM2;
        st->pc = 0x00160000u;
        st->next_pc = 0x00160004u;

        iop_check_hw_interrupt(st, 0x00160004u);

        CHECK(st->pc == 0x80000080u, "no handler registered -> unchanged fixed-vector default behavior");
        CHECK(st->cop0[14] == 0x00160004u, "EPC still set exactly as before this round");
    }

    /* --- Round 112 (task #172/#267/#268, 152nd finding follow-up):
     * the real 32-63 "soft" irq range (istat_hi/imask_hi) must now
     * genuinely reach dispatch, not just be storable in the table.
     * Uses irq=0x2A (real IOP_IRQ_DMA_SIF0) since that's the exact
     * real irq Round 111's host-native diagnostic caught actual
     * module code registering. --- */
    {
        iop_state_t *st = fresh_state();
        iop_intc_state_t *intc = iop_intc_get_state();
        memset(intc, 0, sizeof(*intc));

        st->gpr[4] = 0x2A;             /* a0 = irq 0x2A (SIF0) */
        st->gpr[6] = 0x00180000u;      /* a2 = handler entry point */
        st->gpr[7] = 0xD00DFEEDu;      /* a3 = arg */
        st->gpr[31] = 0x00180080u;
        iop_hle_intr_try_handle(st, IOP_HLE_INTR_REGISTER_INTR_HANDLER);
        CHECK(iop_hle_intr_get_intr_handler(0x2A) == 0x00180000u, "irq 0x2A (SIF0) handler registered for soft-range dispatch test");

        iop_intc_raise_soft(0x2A);
        CHECK((iop_intc_get_state()->istat_hi & (1u << (0x2A - 32))) != 0, "iop_intc_raise_soft(0x2A) sets the real istat_hi bit");
        intc->imask_hi = 0xFFFFFFFFu; /* unmask everything in the soft range */

        st->cop0[12] = 0x1u | IOP_STATUS_IM2;
        st->pc = 0x00190000u;
        st->next_pc = 0x00190004u;

        iop_check_hw_interrupt(st, 0x00190004u);

        CHECK(st->pc == 0x00180000u, "soft-range irq redirected straight into registered handler, not fixed vector");
        CHECK(st->gpr[4] == 0xD00DFEEDu, "soft-range handler called with real RegisterIntrHandler arg in $a0");
        CHECK(st->gpr[31] == IOP_HLE_INTR_HANDLER_RETURN_TRAMPOLINE, "soft-range handler's $ra set to this project's own return trampoline");

        int handled = iop_hle_intr_try_handle(st, st->gpr[31]);
        CHECK(handled == 1, "return trampoline recognized for soft-range irq");
        CHECK(st->pc == 0x00190004u, "resumes at the real saved EPC after soft-range handler returns");
        CHECK((iop_intc_get_state()->istat_hi & (1u << (0x2A - 32))) == 0, "irq 0x2A (SIF0) acknowledged (istat_hi bit cleared) after real handling, no UB from shifting >= 32");
    }

    /* --- Round 112: hardware range (0-31) still takes priority over
     * the soft range when both are pending, matching real MIPS'
     * lowest-numbered-bit-first convention already relied on above. --- */
    {
        iop_state_t *st = fresh_state();
        iop_intc_state_t *intc = iop_intc_get_state();
        memset(intc, 0, sizeof(*intc));

        /* Register handler for irq 0 (hw) and irq 0x2A (soft), then
         * make BOTH pending - irq 0's handler must win. */
        st->gpr[4] = 0;
        st->gpr[6] = 0x001A0000u;
        st->gpr[7] = 0x11111111u;
        st->gpr[31] = 0x001A0080u;
        iop_hle_intr_try_handle(st, IOP_HLE_INTR_REGISTER_INTR_HANDLER);

        st->gpr[4] = 0x2A;
        st->gpr[6] = 0x001B0000u;
        st->gpr[7] = 0x22222222u;
        st->gpr[31] = 0x001B0080u;
        iop_hle_intr_try_handle(st, IOP_HLE_INTR_REGISTER_INTR_HANDLER);

        intc->istat = 0x1u;
        intc->imask = 0x1u;
        iop_intc_raise_soft(0x2A);
        intc->imask_hi = 0xFFFFFFFFu;

        st->cop0[12] = 0x1u | IOP_STATUS_IM2;
        st->pc = 0x001C0000u;
        st->next_pc = 0x001C0004u;
        iop_check_hw_interrupt(st, 0x001C0004u);

        CHECK(st->pc == 0x001A0000u, "hw range (irq 0) takes priority over pending soft range (irq 0x2A)");
        CHECK((iop_intc_get_state()->istat_hi & (1u << (0x2A - 32))) != 0, "soft-range irq 0x2A left pending, untouched, since hw range was serviced first");
    }

    /* --- Round 113 (task #172/#268/#269): real EnableIntr/DisableIntr
     * (intrman#6/#7), ported from ps2sdk's real intrman.c. For
     * irq < 32, plain I_MASK bit. For irq 32-45, the REAL target is
     * the DMA controller's own DMA_ICR/DMA_ICR2 registers (already
     * modeled in iop_dma.c) - not a separate soft-mask register -
     * verified directly against iop_dma_get_state() here, plus the
     * Round 112 imask_hi mirror this real API is now responsible for
     * setting for the first time. --- */
    {
        iop_state_t *st = fresh_state();
        iop_intc_state_t *intc = iop_intc_get_state();
        iop_dma_state_t *dma = iop_dma_get_state();
        memset(intc, 0, sizeof(*intc));
        memset(dma, 0, sizeof(*dma));

        /* EnableIntr(5) - plain hw-range irq, real I_MASK bit. */
        st->gpr[4] = 5;
        st->gpr[31] = 0x001D0000u;
        int handled = iop_hle_intr_try_handle(st, IOP_HLE_INTR_ENABLE_INTR);
        CHECK(handled == 1, "EnableIntr sentinel recognized");
        CHECK(st->gpr[2] == 0, "EnableIntr(5) returns 0 (success)");
        CHECK((iop_intc_get_state()->imask & (1u << 5)) != 0u, "EnableIntr(5) sets the real I_MASK bit 5");

        /* EnableIntr(0x22) - real IOP_IRQ_DMA_SIF2, in the
         * MDEC_IN..GPU_OTC (channel 0-6) real DMA_ICR (dicr1) branch:
         * mask bit at (irq_index-32+16). */
        st->gpr[4] = 0x22;
        st->gpr[31] = 0x001D0010u;
        iop_hle_intr_try_handle(st, IOP_HLE_INTR_ENABLE_INTR);
        CHECK(st->gpr[2] == 0, "EnableIntr(0x22, SIF2) returns 0 (success)");
        CHECK((dma->icr & (1u << (0x22 - 32 + 16))) != 0u, "EnableIntr(0x22) sets the real DMA_ICR (dicr1) mask bit (irq_index-32+16)");
        CHECK((dma->icr & 0x800000u) != 0u, "EnableIntr(0x22) sets the real DMA_ICR bit 23");
        CHECK((iop_intc_get_state()->imask & 8u) != 0u, "EnableIntr(0x22) sets the real I_MASK bit 3 (IOP_IRQ_DMA)");
        CHECK((iop_intc_get_state()->imask_hi & (1u << (0x22 - 32))) != 0u, "EnableIntr(0x22) mirrors into Round 112's imask_hi simplification");

        /* EnableIntr(0x2A) - real IOP_IRQ_DMA_SIF0, in the
         * SPU2..SIO2_OUT (channel 7-12) real DMA_ICR2 (dicr2) branch:
         * mask bit at (irq_index-40+16) - the exact real irq this
         * project's Round 111 host-native diagnostic caught actual
         * module code registering a handler for. */
        st->gpr[4] = 0x2A;
        st->gpr[31] = 0x001D0020u;
        iop_hle_intr_try_handle(st, IOP_HLE_INTR_ENABLE_INTR);
        CHECK(st->gpr[2] == 0, "EnableIntr(0x2A, SIF0) returns 0 (success)");
        CHECK((dma->icr2 & (1u << (0x2A - 40 + 16))) != 0u, "EnableIntr(0x2A) sets the real DMA_ICR2 (dicr2) mask bit (irq_index-40+16)");
        CHECK((iop_intc_get_state()->imask_hi & (1u << (0x2A - 32))) != 0u, "EnableIntr(0x2A) mirrors into imask_hi too");

        /* EnableIntr(0x27) - real IOP_IRQ_DMA_BERR, deliberately NOT
         * handled by real EnableIntr (falls to the real else branch,
         * KE_ILLEGAL_INTRCODE = -101, per the cited kerr.h). */
        st->gpr[4] = 0x27;
        st->gpr[31] = 0x001D0030u;
        iop_hle_intr_try_handle(st, IOP_HLE_INTR_ENABLE_INTR);
        CHECK((int32_t)st->gpr[2] == -101, "EnableIntr(0x27, DMA_BERR) returns real KE_ILLEGAL_INTRCODE (-101), matching real intrman.c's own gap");

        /* DisableIntr(5, &res) - real hw-range disable, round trip. */
        st->gpr[4] = 5;
        st->gpr[5] = 0x00050200u; /* res pointer, in guest RAM */
        st->gpr[31] = 0x001D0040u;
        handled = iop_hle_intr_try_handle(st, IOP_HLE_INTR_DISABLE_INTR);
        CHECK(handled == 1, "DisableIntr sentinel recognized");
        CHECK((int32_t)st->gpr[2] == 0, "DisableIntr(5) returns 0 (was enabled)");
        CHECK((iop_intc_get_state()->imask & (1u << 5)) == 0u, "DisableIntr(5) clears the real I_MASK bit 5");
        CHECK(iop_mem_read32(st, 0x00050200u) == 5u, "DisableIntr(5) writes real irq_index into *res");

        /* DisableIntr(0x2A, &res) - real soft-range disable, must
         * clear the real DMA_ICR2 mask bit AND the imask_hi mirror. */
        st->gpr[4] = 0x2A;
        st->gpr[5] = 0x00050204u;
        st->gpr[31] = 0x001D0050u;
        iop_hle_intr_try_handle(st, IOP_HLE_INTR_DISABLE_INTR);
        CHECK((int32_t)st->gpr[2] == 0, "DisableIntr(0x2A) returns 0 (was enabled)");
        CHECK((dma->icr2 & (1u << (0x2A - 40 + 16))) == 0u, "DisableIntr(0x2A) clears the real DMA_ICR2 mask bit");
        CHECK((iop_intc_get_state()->imask_hi & (1u << (0x2A - 32))) == 0u, "DisableIntr(0x2A) clears the imask_hi mirror too");

        /* DisableIntr on an irq that was never enabled -> real
         * KE_INTRDISABLE (-103, cited kerr.h), res still written. */
        st->gpr[4] = 6; /* never enabled in this test */
        st->gpr[5] = 0x00050208u;
        st->gpr[31] = 0x001D0060u;
        iop_hle_intr_try_handle(st, IOP_HLE_INTR_DISABLE_INTR);
        CHECK((int32_t)st->gpr[2] == -103, "DisableIntr on a never-enabled irq returns real KE_INTRDISABLE (-103)");

        /* DisableIntr with res=NULL must not crash (real code checks
         * `if (res)` before writing). */
        st->gpr[4] = 5;
        st->gpr[5] = 0; /* NULL */
        st->gpr[31] = 0x001D0070u;
        handled = iop_hle_intr_try_handle(st, IOP_HLE_INTR_DISABLE_INTR);
        CHECK(handled == 1, "DisableIntr with res=NULL handled without crashing");
    }

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
