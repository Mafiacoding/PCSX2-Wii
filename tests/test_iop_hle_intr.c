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

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
