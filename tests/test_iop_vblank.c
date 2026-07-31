/*
 * test_iop_vblank.c - host-native test for iop_core.c's real IOP
 * VBLANK_IN/VBLANK_OUT interrupt-source ticking (task #216, 87th
 * finding, continuing tasks #214/#215).
 *
 * Background: real PCSX2 (pcsx2/IopCounters.cpp) raises IOP INTC bit
 * 0 on VBlankStart (iopIntcIrq(0)) and bit 11 on VBlankEnd
 * (iopIntcIrq(11)) - independently corroborated by allkern/iris's
 * src/iop/intc.h (IOP_INTC_VBLANK_IN=0x00000001, IOP_INTC_VBLANK_OUT
 * =0x00000800, i.e. bits 0 and 11), both fetched and cited this
 * round. iop_core.c's iop_check_vblank() reuses this project's own
 * already-cited EE_CYCLES_PER_FRAME_NTSC (4921488, ee_core.c) scaled
 * by the already-documented ~8:1 EE:IOP clock ratio (source/core/
 * system.c), with VBLANK_END at the same 1/12-of-frame offset
 * ee_check_vblank() already uses.
 *
 * This test verifies only the raise mechanism (I_STAT bit 0 at phase
 * 0, bit 11 at phase=IOP_CYCLES_VBLANK_DURATION) - it does NOT
 * assert anything about whether this actually unblocks real BIOS
 * boot, since a live diagnostic run (300M+ instructions, documented
 * in STATUS.md's 87th finding) already showed it does not: Status
 * register is 0x00000000 (IEc=0, IM2=0) at the moment the IOP
 * module loader's `idle=1` shortcut kicks in, so no interrupt source
 * - however real and correctly modeled - can be taken at all under
 * the current model. This is honestly a raise-only unit test.
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

int main(void)
{
    iop_intc_init();

    iop_state_t st;
    memset(&st, 0, sizeof(st));

    /* Phase 0 (instructions_executed == 0): VBLANK_START (bit 0)
     * should raise, VBLANK_END (bit 11) should not. */
    st.instructions_executed = 0;
    iop_check_vblank(&st);
    iop_intc_state_t *intc = iop_intc_get_state();
    CHECK((intc->istat & 0x1u) != 0, "VBLANK_START (bit 0) raised at phase 0");
    CHECK((intc->istat & 0x800u) == 0, "VBLANK_END (bit 11) NOT raised at phase 0");

    /* Reset istat, check VBLANK_END at the documented 1/12-of-frame
     * offset. */
    intc->istat = 0;
    st.instructions_executed = IOP_CYCLES_VBLANK_DURATION;
    iop_check_vblank(&st);
    CHECK((intc->istat & 0x800u) != 0, "VBLANK_END (bit 11) raised at phase=duration");
    CHECK((intc->istat & 0x1u) == 0, "VBLANK_START (bit 0) NOT raised at phase=duration");

    /* Reset istat, check nothing raises at an arbitrary mid-frame
     * phase (no spurious raises). */
    intc->istat = 0;
    st.instructions_executed = IOP_CYCLES_VBLANK_DURATION + 12345u;
    iop_check_vblank(&st);
    CHECK(intc->istat == 0, "no raise at an arbitrary non-boundary phase");

    /* Reset istat, check the NEXT frame's phase-0 (instructions_
     * executed == IOP_CYCLES_PER_FRAME_NTSC, i.e. wraps via modulo)
     * raises VBLANK_START again - confirms periodicity across
     * frames, not just a single first-frame check. */
    intc->istat = 0;
    st.instructions_executed = (uint64_t)IOP_CYCLES_PER_FRAME_NTSC * 3u;
    iop_check_vblank(&st);
    CHECK((intc->istat & 0x1u) != 0, "VBLANK_START re-raises on frame 3's phase 0 (periodicity)");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
