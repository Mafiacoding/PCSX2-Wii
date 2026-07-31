/*
 * test_ee_cdvd_ncmd_reentry.c - host-native test for Round 347's IOP
 * RPC re-entry architecture: ee_try_cdvd_ncmd_real_dispatch() /
 * ee_check_cdvd_ncmd_pending() in source/core/ee/ee_core.c.
 *
 * This test proves the mechanism end-to-end using the project's own
 * real, already-working pieces, without needing a full BIOS/disc
 * boot (this project's own real boot trace only ever issues
 * rpc_number=10/CD_NCMD_CDDASTREAM before parking - see Round 345/347
 * STATUS.md notes - so the mapped rpc_numbers below have never been
 * exercised by a live trace and need direct coverage here):
 *
 *   1. A real IOP IRQ2 handler is registered via the project's own
 *      real RegisterIntrHandler HLE path (iop_hle_intr_try_handle()),
 *      exactly as CDVDMAN's real init does per Round 338's citation.
 *      The handler body is a real, hand-encoded "jr $ra; nop" - the
 *      test doesn't care what a real CDVDMAN handler computes, only
 *      that the dispatch/return plumbing is real and general.
 *   2. ee_try_cdvd_ncmd_real_dispatch() is called directly (as the
 *      SIF_SID_CDVD_NCMD branch now does) with a mapped rpc_number,
 *      driving the real iop_cdvd_mmio_write8() -> dispatch_ncmd() ->
 *      iop_intc_raise(2) path (source/hw/iop_cdvd.c).
 *   3. Real iop_core_step() calls advance the IOP: it takes the real
 *      interrupt, jumps into the real registered handler, executes
 *      the handler's real "jr $ra", and returns through
 *      IOP_HLE_INTR_HANDLER_RETURN_TRAMPOLINE - all genuine project
 *      mechanisms, none of it faked for this test.
 *   4. ee_check_cdvd_ncmd_pending() is polled (as the EE step loop
 *      now does every instruction) and must only deliver the EE
 *      reply once the real completion counter has actually moved.
 *
 * Also covers the explicit non-regression guarantee: rpc_number=10
 * (the one value this project's own real traces have ever observed)
 * must NOT be claimed by ee_try_cdvd_ncmd_real_dispatch(), so the
 * existing immediate-reply fallback in ee_core.c is untouched for it.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

#include "core/ee/ee_core.c"
#include "core/iop/iop_core.h"
#include "core/hw/iop_hle_intr.h"
#include "core/hw/iop_cdvd.h"
#include "core/hw/iop_intc.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static void wle32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

#define IOP_JR_RA  0x03e00008u
#define IOP_NOP    0x00000000u
#define IOP_IMASK_ADDR 0x1F801074u
#define HANDLER_ADDR   0x00120000u

int main(void)
{
    /* --- shared setup: blank EE+IOP bios images, both cores init'd
     * independently (mirrors test_iop_hle_intr.c's fresh_state()
     * pattern), no full system_init()/disc/module boot needed since
     * this test drives the mechanism directly. --- */
    static bios_image_t ee_bios, iop_bios;
    memset(&ee_bios, 0, sizeof(ee_bios));
    ee_bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(ee_bios.data, 0, BIOS_MAX_SIZE);
    ee_bios.size = BIOS_MAX_SIZE;
    ee_bios.loaded = 1;

    memset(&iop_bios, 0, sizeof(iop_bios));
    iop_bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(iop_bios.data, 0, BIOS_MAX_SIZE);
    iop_bios.size = BIOS_MAX_SIZE;
    iop_bios.loaded = 1;
    /* IOP reset vector (0xBFC00000, offset 0) - one real NOP so the
     * first real instruction step is harmless, then
     * iop_check_hw_interrupt() (called after every real step) finds
     * the already-pending, already-enabled IRQ2 and dispatches it. */
    wle32(iop_bios.data + 0x00, IOP_NOP);

    ee_core_init(&ee_bios);
    iop_core_init(&iop_bios);
    iop_hle_intr_init();

    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    /* Real handler body in IOP RAM: jr $ra ; nop (delay slot) -
     * a genuine two-instruction MIPS function that immediately
     * returns, standing in for CDVDMAN's real (much longer) handler
     * without this test needing to model Sony's actual code. */
    iop_mem_write32(iop, HANDLER_ADDR + 0u, IOP_JR_RA);
    iop_mem_write32(iop, HANDLER_ADDR + 4u, IOP_NOP);

    /* Real RegisterIntrHandler(irq=2, mode=0, handler=HANDLER_ADDR,
     * arg=0) via this project's own real, cited HLE sentinel path -
     * exactly the mechanism Round 338 confirmed CDVDMAN's real init
     * genuinely uses. */
    iop->gpr[4] = 2;               /* a0 = irq (real IOP CDVD IRQ line, per iop_cdvd.c's iop_intc_raise(2) citation) */
    iop->gpr[5] = 0;                /* a1 = mode */
    iop->gpr[6] = HANDLER_ADDR;     /* a2 = handler */
    iop->gpr[7] = 0;                /* a3 = arg */
    iop->gpr[31] = 0x00110040u;     /* ra = fake caller return (unused by this test) */
    int reg_ok = iop_hle_intr_try_handle(iop, IOP_HLE_INTR_REGISTER_INTR_HANDLER);
    CHECK(reg_ok == 1, "real RegisterIntrHandler(irq=2, handler) recognized");
    CHECK(iop_hle_intr_get_intr_handler(2) == HANDLER_ADDR, "irq 2 handler address genuinely stored");

    /* Real IOP interrupt-taking preconditions (same real formula
     * test_iop_hw_interrupt.c's own header cites from psx-spx,
     * confirmed applicable to the IOP too): Status.IEc=1, Status.IM2
     * (bit 10)=1, and I_MASK bit 2 enabled so I_STAT&I_MASK becomes
     * nonzero once iop_intc_raise(2) sets I_STAT bit 2. */
    iop->cop0[12] = 0x00000401u; /* IM2 (bit10) | IEc (bit0) */
    iop_mem_write32(iop, IOP_IMASK_ADDR, (1u << 2));
    iop->pc = 0xBFC00000u;
    iop->next_pc = 0xBFC00004u;

    /* --- Regression guard: rpc_number=10 (CD_NCMD_CDDASTREAM, the
     * ONE value this project's own real boot traces have ever
     * observed - see Round 345) must be explicitly rejected by the
     * dispatcher, leaving the existing immediate-reply fallback
     * behavior in ee_core.c completely untouched. --- */
    {
        uint32_t recvbuf = 0x80300000u;
        ee_mem_write32(ee, recvbuf, 0xDEADBEEFu);
        int claimed = ee_try_cdvd_ncmd_real_dispatch(ee, 10u, recvbuf, 0x80310000u, 0u, 0u);
        CHECK(claimed == 0, "rpc_number=10 (CDDASTREAM) is NOT claimed - existing fallback untouched");
        CHECK(g_ee_cdvd_ncmd_reentry.valid == 0, "no re-entry armed for the unmapped rpc_number");
    }

    /* --- Real end-to-end re-entry: rpc_number=6 (CD_NCMD_STANDBY ->
     * NCMD_STANDBY, no sendbuf params needed per the real mapping) --- */
    {
        uint32_t ee_recvbuf = 0x80300100u;
        uint32_t ee_cd = 0x80310100u;
        ee_mem_write32(ee, ee_recvbuf, 0x11111111u); /* poison value - must be overwritten only on real completion */

        uint32_t before = iop_hle_intr_get_handler_completion_count(2);
        CHECK(before == 0, "IRQ2 completion counter starts at 0");

        int claimed = ee_try_cdvd_ncmd_real_dispatch(ee, 6u, ee_recvbuf, ee_cd, 0u, 0u);
        CHECK(claimed == 1, "rpc_number=6 (STANDBY) is claimed by the real dispatcher");
        CHECK(g_ee_cdvd_ncmd_reentry.valid == 1, "re-entry state armed");
        CHECK(iop_cdvd_get_last_ncommand() == 0x02u, "real NCMD register genuinely holds NCMD_STANDBY (0x02) after the real MMIO write");

        /* Not complete yet - the real IOP handler hasn't run. */
        ee_check_cdvd_ncmd_pending(ee);
        CHECK(ee_mem_read32(ee, ee_recvbuf) == 0x11111111u, "EE reply NOT delivered yet - real handler hasn't finished");
        CHECK(g_ee_cdvd_ncmd_reentry.valid == 1, "re-entry still pending before the real handler runs");

        /* Drive the real IOP: step until the real dispatch + real
         * handler + real trampoline return have all genuinely
         * happened (a handful of real instruction steps: the pending
         * NOP, the interrupt-check dispatch, the handler's own two
         * real instructions, and the trampoline return). Generous
         * cap, real completion should land within single digits. */
        int i, dispatched = 0;
        for (i = 0; i < 32 && !dispatched; i++) {
            iop_core_step();
            if (iop_hle_intr_get_handler_completion_count(2) != before) dispatched = 1;
        }
        CHECK(dispatched == 1, "real IOP execution genuinely reached and returned from the registered IRQ2 handler");
        CHECK(iop_hle_intr_get_stats()->real_handler_dispatches >= 1u, "real_handler_dispatches stat genuinely incremented (not a fabricated count)");

        /* Now the poll must deliver the real, async reply. */
        ee_check_cdvd_ncmd_pending(ee);
        CHECK(g_ee_cdvd_ncmd_reentry.valid == 0, "re-entry state cleared after real completion");
        CHECK(ee_mem_read32(ee, ee_recvbuf) == 0u, "EE reply delivered (0 = success) only after real handler completion");
    }

    /* --- Re-entrancy guard: while one re-entry is in flight, a
     * second call must not stomp it (matches the function's own
     * "if (g_ee_cdvd_ncmd_reentry.valid) return 0;" guard). --- */
    {
        uint32_t ee_recvbuf = 0x80300200u;
        int claimed_first = ee_try_cdvd_ncmd_real_dispatch(ee, 7u, ee_recvbuf, 0x80310200u, 0u, 0u);
        CHECK(claimed_first == 1, "first in-flight call (rpc_number=7/STOP) claimed");
        int claimed_second = ee_try_cdvd_ncmd_real_dispatch(ee, 6u, 0x80300300u, 0x80310300u, 0u, 0u);
        CHECK(claimed_second == 0, "second call while one is already in flight is correctly rejected, not double-armed");

        /* drain it so it doesn't leak into later CHECKs */
        uint32_t before2 = g_ee_cdvd_ncmd_reentry.armed_completion_count;
        int i, dispatched2 = 0;
        for (i = 0; i < 32 && !dispatched2; i++) {
            iop_core_step();
            if (iop_hle_intr_get_handler_completion_count(2) != before2) dispatched2 = 1;
        }
        ee_check_cdvd_ncmd_pending(ee);
        CHECK(dispatched2 == 1 && g_ee_cdvd_ncmd_reentry.valid == 0, "in-flight call drained cleanly");
    }

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
