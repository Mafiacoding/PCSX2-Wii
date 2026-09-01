/*
 * test_iop_hle_sysmem_ordinal10.c - Round 761 (task #762, user-approved
 * fabrication - see include/core/hw/iop_hle_heap.h and docs/STATUS.md
 * "Round 761" for the full writeup): regression coverage for the new
 * SYSMEM ordinal-10 (QueryBlockSize) HLE intercept at the real,
 * hardcoded jump target pc=0x0000044C.
 *
 * This does NOT (and cannot) verify that GT3's own real code gets
 * further past the Round-173 wall - that requires re-running the full,
 * multi-billion-instruction GT3 checkpoint chain, deferred to a future
 * round (see STATUS.md). What this DOES verify, cheaply and honestly,
 * is the gate's own documented contract: real tracked addresses get a
 * real answer from the already-tested Round 401 heap model, untracked
 * addresses get the real -1-on-miss convention, the return-via-$ra
 * wiring is correct, and the gate is precisely scoped to 0x44C only
 * (not accidentally over-broad).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "core/iop/iop_core.h"
#include "core/hw/iop_hle_heap.h"
#include "core/hw/iop_heap.h"

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_failures++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

int main(void)
{
    iop_state_t st;
    memset(&st, 0, sizeof(st));
    iop_heap_init();

    /* Real tracked block, obtained the same way a genuine AllocSysMemory
     * (ordinal 4) caller would have - this file's gate reuses the exact
     * same already-tested Round 401 heap model AllocSysMemory's own HLE
     * gate uses, so a pointer obtained via iop_heap_alloc() is precisely
     * what QueryBlockSize would realistically be asked about. */
    uint32_t addr = iop_heap_alloc(IOP_HEAP_ALLOC_FIRST, 4096, 0);
    CHECK(addr != 0, "iop_heap_alloc() returned a real non-zero block address");

    /* Test 1: real tracked address -> real size, correct $ra return. */
    st.pc = IOP_HLE_HEAP_SYSMEM_ORDINAL10_QUERYBLOCKSIZE;
    st.gpr[4] = addr;
    st.gpr[31] = 0x00123456u;
    int handled = iop_hle_heap_try_handle(&st, st.pc);
    CHECK(handled == 1, "gate fires for pc=0x0000044C");
    CHECK(st.pc == 0x00123456u && st.next_pc == 0x0012345Au,
          "returns via $ra/$ra+4, matching every sibling HLE gate's convention");
    CHECK((int32_t)st.gpr[2] != -1, "real tracked address is not misreported as -1/miss");
    CHECK(st.gpr[2] == 0x00001000u, "real tracked 4096-byte block reports exactly 0x1000 (size|USED, USED=0)");

    /* Test 2: untracked/foreign address -> real -1 miss convention
     * (iop_heap.h's own documented contract, matches real hardware). */
    memset(&st, 0, sizeof(st));
    st.pc = IOP_HLE_HEAP_SYSMEM_ORDINAL10_QUERYBLOCKSIZE;
    st.gpr[4] = 0x00777777u;
    st.gpr[31] = 0x00654321u;
    handled = iop_hle_heap_try_handle(&st, st.pc);
    CHECK(handled == 1, "gate fires for pc=0x0000044C (test 2)");
    CHECK((int32_t)st.gpr[2] == -1, "untracked address correctly reports -1 (real miss convention)");
    CHECK(st.pc == 0x00654321u, "returns via $ra even on a miss");

    /* Test 3: the gate must be precisely scoped to 0x44C, not a whole
     * range - EXCEPMAN's own real handler-table data legitimately lives
     * at neighboring addresses (0x440-0x47C) and must never be
     * intercepted as if it were this call. */
    memset(&st, 0, sizeof(st));
    st.pc = 0x00000450u;
    handled = iop_hle_heap_try_handle(&st, st.pc);
    CHECK(handled == 0, "neighboring address 0x450 is correctly NOT claimed by this gate");

    memset(&st, 0, sizeof(st));
    st.pc = 0x00000448u;
    handled = iop_hle_heap_try_handle(&st, st.pc);
    CHECK(handled == 0, "neighboring address 0x448 is correctly NOT claimed by this gate");

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d check(s) failed\n", g_failures);
    return 1;
}
