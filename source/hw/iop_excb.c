/*
 * iop_excb.c - IOP kernel exception-handler priority chains (ExCB).
 * See include/core/hw/iop_excb.h for the full citation trail and
 * scope note (real container/mechanism, not real handler bodies).
 */
#include "core/hw/iop_excb.h"
#include <string.h>

static iop_excb_state_t g_excb;

iop_excb_state_t *iop_excb_get_state(void) { return &g_excb; }

void iop_excb_init(iop_state_t *st)
{
    memset(&g_excb, 0, sizeof(g_excb));

    /* RAM[0x100]/RAM[0x104]: pointer+size to the real chain-head
     * array, per psx-spx's "Table of Tables" ("ExCB Exception Chain
     * Entrypoints (addr=var, size=4*08h)"). */
    iop_mem_write32(st, IOP_EXCB_TABLE_ADDR, IOP_EXCB_ARRAY_ADDR);
    iop_mem_write32(st, IOP_EXCB_TABLE_ADDR + 4u, IOP_EXCB_TABLE_SIZE);

    /* The array itself: 4 priority chains, correctly starting all-
     * empty (head pointer = 0/NULL for each) - matching "before any
     * handler is registered", exactly the scenario Round 19's trace
     * found. Each slot is 8 bytes (head pointer + 4 reserved bytes),
     * matching the documented 4*08h total size. */
    for (uint32_t i = 0; i < IOP_EXCB_NUM_PRIO; i++) {
        iop_mem_write32(st, IOP_EXCB_ARRAY_ADDR + i * 8u, 0u);      /* chain head = NULL */
        iop_mem_write32(st, IOP_EXCB_ARRAY_ADDR + i * 8u + 4u, 0u); /* reserved */
    }
}

static uint32_t chain_head_addr(uint32_t priority)
{
    return IOP_EXCB_ARRAY_ADDR + priority * 8u;
}

void iop_excb_sys_enq_int_rp(iop_state_t *st, uint32_t priority, uint32_t struc)
{
    g_excb.enq_calls++;
    if (priority >= IOP_EXCB_NUM_PRIO)
        return; /* undocumented for out-of-range priority - not guessed at */

    /* Real, documented behavior (psx-spx "C(02h) - SysEnqIntRP"):
     * insert at the HEAD of the chain - the new node's own 00h field
     * (next-pointer) is overwritten by the BIOS with whatever was
     * previously the head, then the chain-head slot is updated to
     * point at the new node. Fields 04h (second function) and 08h
     * (first function) are the CALLER's own responsibility (already
     * filled in before calling SysEnqIntRP) and are left untouched
     * here. */
    uint32_t addr = chain_head_addr(priority);
    uint32_t old_head = iop_mem_read32(st, addr);
    iop_mem_write32(st, struc + 0x00u, old_head); /* struc->next = old head */
    iop_mem_write32(st, addr, struc);             /* chain head  = struc    */
}

void iop_excb_sys_deq_int_rp(iop_state_t *st, uint32_t priority, uint32_t struc)
{
    g_excb.deq_calls++;
    if (priority >= IOP_EXCB_NUM_PRIO)
        return;

    uint32_t addr = chain_head_addr(priority);
    uint32_t head = iop_mem_read32(st, addr);

    if (head == struc) {
        /* The one case real hardware handles correctly: unlink the
         * first element via its own next-pointer. */
        uint32_t next = iop_mem_read32(st, struc + 0x00u);
        iop_mem_write32(st, addr, next);
        return;
    }

    /* Real, documented BUG (psx-spx "C(03h) - SysDeqIntRP"): removing
     * anything other than the first element reads garbage from an
     * uninitialized stack location and behaves unpredictably on real
     * hardware. Not reproducible/citable as a SPECIFIC outcome, so
     * this project deliberately leaves the chain untouched (a safe,
     * conservative no-op) rather than fabricating a garbage-dependent
     * result - see the header comment for the full rationale. */
    g_excb.deq_bug_noop_count++;
}
