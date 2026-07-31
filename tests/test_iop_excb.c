/*
 * test_iop_excb.c - host-native test for iop_excb.c's real RAM[0x100]
 * exception-handler priority-chain mechanism (Round 22).
 *
 * See include/core/hw/iop_excb.h for the full citation trail (psx-spx
 * kernelbios.md's "BIOS Interrupt/Exception Handling" section -
 * "Priority Chains", "C(02h) - SysEnqIntRP", "C(03h) - SysDeqIntRP").
 * This completes the reference Round 19 was missing: the real BIOS-
 * resident generic exception dispatcher looks up a handler through a
 * chain rooted at RAM[0x100] - this is the real mechanism that builds
 * and manipulates that chain.
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

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    iop_core_init(&bios);
    iop_state_t *st = iop_core_get_state();

    /* --- Table of Tables entry --- */
    CHECK(iop_mem_read32(st, 0x100u) == IOP_EXCB_ARRAY_ADDR,
          "RAM[0x100] points at the real chain-head array (IOP_EXCB_ARRAY_ADDR)");
    CHECK(iop_mem_read32(st, 0x104u) == 0x20u,
          "RAM[0x104] == 4*08h (0x20), the documented Table-of-Tables size field");

    /* --- All 4 priority chains start empty --- */
    for (uint32_t p = 0; p < 4; p++) {
        char msg[80];
        snprintf(msg, sizeof(msg), "priority %u chain head starts NULL (no handler registered)", p);
        CHECK(iop_mem_read32(st, IOP_EXCB_ARRAY_ADDR + p * 8u) == 0u, msg);
    }

    /* --- SysEnqIntRP: insert one node on priority 0 --- */
    uint32_t node_a = 0x00020000u; /* arbitrary IOP-RAM scratch address for the node */
    iop_mem_write32(st, node_a + 0x04u, 0x11110000u); /* second-function ptr (caller-set) */
    iop_mem_write32(st, node_a + 0x08u, 0x22220000u); /* first-function ptr  (caller-set) */
    iop_excb_sys_enq_int_rp(st, 0, node_a);

    CHECK(iop_mem_read32(st, IOP_EXCB_ARRAY_ADDR + 0 * 8u) == node_a,
          "after SysEnqIntRP(0, node_a): priority-0 chain head == node_a");
    CHECK(iop_mem_read32(st, node_a + 0x00u) == 0u,
          "node_a's own next-pointer (00h) == 0 - it was the first/only element, old head was NULL");
    CHECK(iop_mem_read32(st, node_a + 0x08u) == 0x22220000u,
          "node_a's first-function pointer (08h) is untouched by SysEnqIntRP (caller-owned field)");

    /* --- SysEnqIntRP: insert a SECOND node on the same priority - must
     * become the new head, with node_a now linked as its `next` --- */
    uint32_t node_b = 0x00020020u;
    iop_mem_write32(st, node_b + 0x08u, 0x33330000u);
    iop_excb_sys_enq_int_rp(st, 0, node_b);

    CHECK(iop_mem_read32(st, IOP_EXCB_ARRAY_ADDR + 0 * 8u) == node_b,
          "after SysEnqIntRP(0, node_b): priority-0 chain head == node_b (newest-first, real documented order)");
    CHECK(iop_mem_read32(st, node_b + 0x00u) == node_a,
          "node_b's next-pointer == node_a (the previous head is now correctly linked after it)");

    /* --- Other priority chains remain untouched by priority-0 inserts --- */
    CHECK(iop_mem_read32(st, IOP_EXCB_ARRAY_ADDR + 1 * 8u) == 0u,
          "priority-1 chain head still NULL - unaffected by priority-0 SysEnqIntRP calls");

    /* --- SysDeqIntRP: removing the HEAD (node_b) works correctly --- */
    iop_excb_sys_deq_int_rp(st, 0, node_b);
    CHECK(iop_mem_read32(st, IOP_EXCB_ARRAY_ADDR + 0 * 8u) == node_a,
          "after SysDeqIntRP(0, node_b) [head]: priority-0 chain head correctly becomes node_a");

    /* --- SysDeqIntRP: removing a NON-head element is the documented
     * real bug - modeled as a safe no-op, not a fabricated outcome --- */
    uint32_t node_c = 0x00020040u;
    iop_excb_sys_enq_int_rp(st, 0, node_c); /* head is now node_c, next=node_a */
    iop_excb_sys_deq_int_rp(st, 0, node_a); /* node_a is NOT the head - real bug case */
    CHECK(iop_mem_read32(st, IOP_EXCB_ARRAY_ADDR + 0 * 8u) == node_c,
          "SysDeqIntRP on a non-head element left the chain untouched (documented real-hardware bug, modeled as a no-op)");
    CHECK(iop_excb_get_state()->deq_bug_noop_count == 1,
          "the non-head SysDeqIntRP call was counted as a documented-bug no-op");

    /* --- Out-of-range priority is safely ignored, not undefined --- */
    iop_excb_sys_enq_int_rp(st, 4, node_a); /* priority 4 doesn't exist (only 0-3) */
    CHECK(iop_excb_get_state()->enq_calls == 4,
          "out-of-range-priority SysEnqIntRP call was still counted (4 total enq calls made)");

    /* --- Wired through the real C0-table HLE trap (C(02h)) --- */
    {
        st->gpr[9]  = 0x02u;         /* $t1 = function 0x02 (SysEnqIntRP) */
        st->gpr[4]  = 1u;            /* $a0 = priority 1 */
        st->gpr[5]  = 0x00020060u;   /* $a1 = struc */
        st->gpr[31] = 0xBFC00040u;   /* $ra = arbitrary return address */
        int handled = iop_hle_bios_try_handle(st, IOP_HLE_TABLE_C0);
        CHECK(handled == 1, "C0-table function 0x02 (SysEnqIntRP) was recognized and handled");
        CHECK(iop_mem_read32(st, IOP_EXCB_ARRAY_ADDR + 1 * 8u) == 0x00020060u,
              "the real C(02h) trap correctly inserted the node onto priority-1's chain");
        CHECK(st->pc == 0xBFC00040u, "control correctly returned to $ra after the real C(02h) call");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
