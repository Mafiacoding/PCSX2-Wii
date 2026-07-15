/*
 * iop_hle_intr.c - see include/core/hw/iop_hle_intr.h for the full
 * design rationale and citations. Round 109 (task #172/#247/#249
 * continuation).
 */
#include <string.h>
#include "core/hw/iop_hle_intr.h"
#include "core/hw/iop_intc.h"
#include "core/hw/iop_dma.h" /* Round 113: real EnableIntr/DisableIntr target DMA_ICR/DMA_ICR2 */

typedef struct {
    uint32_t intr_handler_addr[IOP_HLE_INTR_NUM_IRQ];
    uint32_t intr_handler_arg[IOP_HLE_INTR_NUM_IRQ];
    uint32_t exc_handler_addr[IOP_HLE_INTR_NUM_EXC];
    uint32_t default_exc_handler_addr;
    iop_hle_intr_stats_t stats;

    /* Round-trip state for iop_hle_intr_dispatch_interrupt(): saved
     * so the return trampoline (IOP_HLE_INTR_HANDLER_RETURN_TRAMPOLINE)
     * knows which IRQ to acknowledge and where to resume, mirroring
     * exactly what iop_module_loader.c's own ExcCode==0 trap-stub-
     * bypass path already does for the no-handler-registered case
     * (task #252/136th finding) - same real semantics, just reached
     * via a different path when a real handler IS registered. */
    uint32_t dispatched_irq;
    uint32_t saved_epc;
    uint32_t in_dispatch;
} iop_hle_intr_globals_t;

static iop_hle_intr_globals_t g;

void iop_hle_intr_init(void)
{
    memset(&g, 0, sizeof(g));
}

const iop_hle_intr_stats_t *iop_hle_intr_get_stats(void)
{
    return &g.stats;
}

uint32_t iop_hle_intr_get_intr_handler(int irq)
{
    if (irq < 0 || irq >= IOP_HLE_INTR_NUM_IRQ) return 0;
    return g.intr_handler_addr[irq];
}

uint32_t iop_hle_intr_get_exc_handler(int exc)
{
    if (exc < 0 || exc >= IOP_HLE_INTR_NUM_EXC) return 0;
    return g.exc_handler_addr[exc];
}

uint32_t iop_hle_intr_sentinel_for_import(const char *module_name, uint32_t ordinal)
{
    if (!module_name) return 0;
    /* Ordinals taken directly from the cited ps2sdk headers'
     * DECLARE_IMPORT() macros - see this file's own header comment
     * and iop_hle_intr.h's iop_hle_intr_sentinel_for_import() doc
     * comment for the full citation. */
    if (strcmp(module_name, "intrman") == 0) {
        if (ordinal == 4u) return IOP_HLE_INTR_REGISTER_INTR_HANDLER;   /* I_RegisterIntrHandler */
        if (ordinal == 5u) return IOP_HLE_INTR_RELEASE_INTR_HANDLER;   /* I_ReleaseIntrHandler */
        if (ordinal == 6u) return IOP_HLE_INTR_ENABLE_INTR;             /* I_EnableIntr (Round 113) */
        if (ordinal == 7u) return IOP_HLE_INTR_DISABLE_INTR;            /* I_DisableIntr (Round 113) */
    } else if (strcmp(module_name, "excepman") == 0) {
        if (ordinal == 4u) return IOP_HLE_INTR_REGISTER_EXCEPTION_HANDLER;         /* I_RegisterExceptionHandler */
        if (ordinal == 6u) return IOP_HLE_INTR_REGISTER_DEFAULT_EXCEPTION_HANDLER; /* I_RegisterDefaultExceptionHandler */
        if (ordinal == 7u) return IOP_HLE_INTR_RELEASE_EXCEPTION_HANDLER;         /* I_ReleaseExceptionHandler */
    }
    return 0;
}

int iop_hle_intr_try_handle(iop_state_t *st, uint32_t pc)
{
    uint32_t ra = st->gpr[31]; /* $ra - real MIPS o32 return-address register */

    if (pc == IOP_HLE_INTR_REGISTER_INTR_HANDLER) {
        /* int RegisterIntrHandler(int irq, int mode,
         *                         int (*handler)(void *arg), void *arg)
         * Real o32 ABI: a0=irq, a1=mode, a2=handler, a3=arg. */
        int32_t irq = (int32_t)st->gpr[4];
        uint32_t handler = st->gpr[6];
        uint32_t arg = st->gpr[7];
        g.stats.calls_seen++;
        if (irq >= 0 && irq < IOP_HLE_INTR_NUM_IRQ) {
            g.intr_handler_addr[irq] = handler;
            g.intr_handler_arg[irq] = arg;
            g.stats.intr_handlers_registered++;
            st->gpr[2] = 0; /* real success return: 0 */
        } else {
            st->gpr[2] = (uint32_t)-1; /* out-of-range irq: real kernel's own illegal-arg convention */
        }
        st->pc = ra;
        st->next_pc = ra + 4u;
        return 1;
    } else if (pc == IOP_HLE_INTR_RELEASE_INTR_HANDLER) {
        /* int ReleaseIntrHandler(int irq) - a0=irq. */
        int32_t irq = (int32_t)st->gpr[4];
        g.stats.calls_seen++;
        if (irq >= 0 && irq < IOP_HLE_INTR_NUM_IRQ) {
            g.intr_handler_addr[irq] = 0;
            g.intr_handler_arg[irq] = 0;
            g.stats.intr_handlers_released++;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra;
        st->next_pc = ra + 4u;
        return 1;
    } else if (pc == IOP_HLE_INTR_REGISTER_EXCEPTION_HANDLER) {
        /* int RegisterExceptionHandler(int exception,
         *                              exception_handler_t handler)
         * a0=exception (0-15), a1=pointer to a REAL, caller-
         * constructed exception_handler_struct_t (next/info/
         * funccode[] - see this file's header for the cited layout).
         * funccode[0] lives at handler_ptr+8; that is the real entry
         * point the caller wants invoked - read directly out of the
         * caller's own already-correct guest RAM, not fabricated. */
        int32_t exc = (int32_t)st->gpr[4];
        uint32_t handler_ptr = st->gpr[5];
        g.stats.calls_seen++;
        if (exc >= 0 && exc < IOP_HLE_INTR_NUM_EXC && handler_ptr != 0) {
            uint32_t funccode0 = iop_mem_read32(st, handler_ptr + 8u);
            g.exc_handler_addr[exc] = funccode0;
            g.stats.exc_handlers_registered++;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra;
        st->next_pc = ra + 4u;
        return 1;
    } else if (pc == IOP_HLE_INTR_RELEASE_EXCEPTION_HANDLER) {
        /* int ReleaseExceptionHandler(int exception, handler) - a0=exception. */
        int32_t exc = (int32_t)st->gpr[4];
        g.stats.calls_seen++;
        if (exc >= 0 && exc < IOP_HLE_INTR_NUM_EXC) {
            g.exc_handler_addr[exc] = 0;
            g.stats.exc_handlers_released++;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra;
        st->next_pc = ra + 4u;
        return 1;
    } else if (pc == IOP_HLE_INTR_REGISTER_DEFAULT_EXCEPTION_HANDLER) {
        /* int RegisterDefaultExceptionHandler(exception_handler_t h)
         * a0=pointer to the same real struct shape. */
        uint32_t handler_ptr = st->gpr[4];
        g.stats.calls_seen++;
        if (handler_ptr != 0) {
            uint32_t funccode0 = iop_mem_read32(st, handler_ptr + 8u);
            g.default_exc_handler_addr = funccode0;
            g.stats.default_exc_handlers_registered++;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra;
        st->next_pc = ra + 4u;
        return 1;
    } else if (pc == IOP_HLE_INTR_ENABLE_INTR) {
        /* int EnableIntr(int irq) - a0=irq. Real body ported from
         * ps2sdk's iop/system/intrman/src/intrman.c (fetched this
         * round): irq_index = irq & 0xFF; three real ranges:
         *   irq_index < 32            -> plain I_MASK bit (this
         *                                 project's existing, real
         *                                 iop_intc_state_t.imask).
         *   32 <= irq_index <= 38     -> DMA_ICR (dicr1) bits, exact
         *                                 real formula below.
         *   40 <= irq_index <= 45     -> DMA_ICR2 (dicr2) bits, exact
         *                                 real formula below.
         *   anything else             -> KE_ILLEGAL_INTRCODE (-101,
         *                                 real cited value, iop/
         *                                 kernel/include/kerr.h).
         * This is the REAL mechanism - real EnableIntr does NOT use a
         * separate soft-irq mask register; it directly toggles the
         * already-modeled DMA controller's own ICR/ICR2 registers
         * (core/hw/iop_dma.h). Round 112's istat_hi/imask_hi remain
         * an explicitly-labeled simplification of INTRMAN's own
         * internal irq-3 re-dispatch (not modeled - its real code
         * isn't in any fetched source) - EnableIntr additionally
         * mirrors into imask_hi purely so that simplification becomes
         * reachable via this real, standard, now-implemented API. */
        int32_t irq = (int32_t)st->gpr[4];
        uint32_t irq_index = (uint32_t)irq & 0xFFu;
        uint32_t upper = ((uint32_t)irq & 0xFF00u) >> 8;
        int32_t ret = 0;
        g.stats.calls_seen++;
        if (irq_index < 32u) {
            iop_intc_get_state()->imask |= (1u << irq_index);
        } else if (irq_index >= 0x20u && irq_index <= 0x26u) {
            iop_dma_state_t *dma = iop_dma_get_state();
            dma->icr = (dma->icr & ~(1u << (irq_index - 32u)) & 0xFFFFFFu)
                     | (((upper & 0x1u) != 0u) ? (1u << (irq_index - 32u)) : 0u)
                     | (1u << (irq_index - 32u + 16u)) | 0x800000u;
            dma->icr2 = (dma->icr2 & ~(1u << (irq_index - 32u)) & 0xFFFFFFu)
                      | (((upper & 0x2u) != 0u) ? (1u << (irq_index - 32u)) : 0u);
            iop_intc_get_state()->imask |= 8u; /* real IOP_IRQ_DMA bit */
            iop_intc_get_state()->imask_hi |= (1u << (irq_index - 32u)); /* Round 112 simplification mirror */
        } else if (irq_index >= 0x28u && irq_index <= 0x2Du) {
            iop_dma_state_t *dma = iop_dma_get_state();
            dma->icr2 = (dma->icr2 & ~(1u << (irq_index - 40u + 7u)) & 0xFFFFFFu)
                      | (((upper & 0x2u) != 0u) ? (1u << (irq_index - 33u)) : 0u)
                      | (1u << (irq_index - 40u + 16u));
            dma->icr = (dma->icr & 0x7FFFFFu) | 0x800000u;
            iop_intc_get_state()->imask |= 8u;
            iop_intc_get_state()->imask_hi |= (1u << (irq_index - 32u));
        } else {
            ret = -101; /* KE_ILLEGAL_INTRCODE, real cited value */
        }
        st->gpr[2] = (uint32_t)ret;
        st->pc = ra;
        st->next_pc = ra + 4u;
        return 1;
    } else if (pc == IOP_HLE_INTR_DISABLE_INTR) {
        /* int DisableIntr(int irq, int *res) - a0=irq, a1=res
         * (may be NULL, real code checks before writing). Real body,
         * same citation as EnableIntr above. */
        int32_t irq = (int32_t)st->gpr[4];
        uint32_t res_ptr = st->gpr[5];
        uint32_t irq_index = (uint32_t)irq & 0xFFu;
        int32_t ret = 0;
        int32_t res_temp = -103; /* KE_INTRDISABLE, real cited value */
        g.stats.calls_seen++;
        if (irq_index < 32u) {
            iop_intc_state_t *intc = iop_intc_get_state();
            uint32_t old_imask = intc->imask;
            intc->imask = old_imask & ~(1u << irq_index);
            if ((old_imask & (1u << irq_index)) != 0u) {
                res_temp = (int32_t)irq_index;
            } else {
                ret = -103;
            }
        } else if (irq_index >= 0x20u && irq_index <= 0x26u) {
            iop_dma_state_t *dma = iop_dma_get_state();
            uint32_t dicr_tmp = dma->icr & 0xFFFFFFu;
            if ((dicr_tmp & (1u << (irq_index - 16u))) != 0u) {
                res_temp = (int32_t)irq_index;
                if (((dicr_tmp >> (irq_index - 32u)) & 1u) != 0u) res_temp |= 0x100;
                if ((dma->icr2 & (1u << (irq_index - 32u))) != 0u) res_temp |= 0x200;
                dma->icr = dicr_tmp & ~(1u << (irq_index - 16u));
                iop_intc_get_state()->imask_hi &= ~(1u << (irq_index - 32u));
            } else {
                ret = -103;
            }
        } else if (irq_index >= 0x28u && irq_index <= 0x2Du) {
            iop_dma_state_t *dma = iop_dma_get_state();
            uint32_t dicr_tmp = dma->icr2 & 0xFFFFFFu;
            if ((dicr_tmp & (1u << (irq_index - 24u))) != 0u) {
                res_temp = (int32_t)irq_index;
                if (((dicr_tmp >> (irq_index - 33u)) & 1u) != 0u) res_temp |= 0x200;
                dma->icr2 = dicr_tmp & ~(1u << (irq_index - 24u));
                iop_intc_get_state()->imask_hi &= ~(1u << (irq_index - 32u));
            } else {
                ret = -103;
            }
        } else {
            ret = -101; /* KE_ILLEGAL_INTRCODE */
        }
        if (res_ptr != 0u)
            iop_mem_write32(st, res_ptr, (uint32_t)res_temp);
        st->gpr[2] = (uint32_t)ret;
        st->pc = ra;
        st->next_pc = ra + 4u;
        return 1;
    } else if (pc == IOP_HLE_INTR_HANDLER_RETURN_TRAMPOLINE) {
        /* A registered handler we jumped into (via
         * iop_hle_intr_dispatch_interrupt(), below) has finished and
         * executed its own real `jr $ra` back to us. Finish the
         * delivery exactly like a real RFE would: pop the Status
         * stack (same formula this project's own real IOP RFE
         * already uses, task #113) and resume at the saved EPC. Also
         * acknowledge (clear) the specific IRQ bit that was
         * delivered, mirroring task #252/136th finding's own "an
         * unhandled interrupt must be acked or it refires forever"
         * reasoning - except here it's genuinely handled (a real
         * handler ran), so the ack is even more clearly correct than
         * that default-path case. */
        if (g.in_dispatch) {
            iop_intc_state_t *intc = iop_intc_get_state();
            /* Round 112: dispatched_irq can now be 0-63 (previously
             * always 0-31), so acknowledging must target whichever
             * of the two real, architecturally-separate ranges the
             * irq actually came from - see iop_intc.h's istat_hi
             * comment. Shifting by >= 32 is undefined behavior in C,
             * so this split is required, not just tidier. */
            if (g.dispatched_irq < 32u)
                intc->istat &= ~(1u << g.dispatched_irq);
            else
                intc->istat_hi &= ~(1u << (g.dispatched_irq - 32u));
            st->cop0[12] = (st->cop0[12] & ~0x0Fu) | ((st->cop0[12] >> 2) & 0x0Fu); /* Status stack pop, real RFE formula */
            st->pc = g.saved_epc;
            st->next_pc = g.saved_epc + 4u;
            g.in_dispatch = 0;
        }
        return 1;
    }

    return 0;
}

int iop_hle_intr_dispatch_interrupt(iop_state_t *st, uint32_t irq)
{
    if (irq >= IOP_HLE_INTR_NUM_IRQ) return 0;
    uint32_t handler = g.intr_handler_addr[irq];
    if (handler == 0) return 0; /* nothing registered - caller falls back to existing default behavior */

    g.dispatched_irq = irq;
    g.saved_epc = st->cop0[14]; /* EPC, already written by the caller before calling this */
    g.in_dispatch = 1;

    st->gpr[4] = g.intr_handler_arg[irq]; /* $a0 = arg, real RegisterIntrHandler ABI */
    st->gpr[31] = IOP_HLE_INTR_HANDLER_RETURN_TRAMPOLINE; /* $ra = our own return gate */
    st->pc = handler;
    st->next_pc = handler + 4u;

    g.stats.real_handler_dispatches++;
    return 1;
}
