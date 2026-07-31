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

/* Round 29: reads the chain-head ARRAY's base address dynamically
 * from RAM[0x100] (IOP_EXCB_TABLE_ADDR) instead of assuming it's
 * always IOP_EXCB_ARRAY_ADDR. This matters now that
 * IOP_HLE_B0_ALLOC_KERNEL_MEMORY is implemented for real (see
 * iop_hle_bios.h/.c): a real, executing BIOS can legitimately
 * allocate the ExCB array via a real B(00h) call and store whatever
 * address it actually got back into RAM[0x100] - which will usually
 * (but is not guaranteed to) be IOP_EXCB_ARRAY_ADDR itself, e.g. if
 * some earlier B(00h) call already consumed part of the Kernel Memory
 * region first. Matches the real exception dispatcher's own behavior,
 * confirmed via live disassembly (docs/STATUS.md's "Round 29"
 * section): `addiu $s3,zero,0x100 / lw $s3,($s3)` before ever touching
 * the array itself. Falls back to IOP_EXCB_ARRAY_ADDR if RAM[0x100]
 * is still 0 (nothing has allocated it yet - e.g. host-native unit
 * tests that call this directly without a real BIOS boot), preserving
 * every existing test's behavior. */
static uint32_t chain_head_addr(iop_state_t *st, uint32_t priority)
{
    uint32_t table_addr = iop_mem_read32(st, IOP_EXCB_TABLE_ADDR);
    if (table_addr == 0u)
        table_addr = IOP_EXCB_ARRAY_ADDR;
    return table_addr + priority * 8u;
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
    uint32_t addr = chain_head_addr(st, priority);
    uint32_t old_head = iop_mem_read32(st, addr);
    iop_mem_write32(st, struc + 0x00u, old_head); /* struc->next = old head */
    iop_mem_write32(st, addr, struc);             /* chain head  = struc    */
}

void iop_excb_sys_deq_int_rp(iop_state_t *st, uint32_t priority, uint32_t struc)
{
    g_excb.deq_calls++;
    if (priority >= IOP_EXCB_NUM_PRIO)
        return;

    uint32_t addr = chain_head_addr(st, priority);
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

/* ------------------------------------------------------------------
 * Round 168: real-ExCB-chain interrupt dispatch fallback.
 * See include/core/hw/iop_excb.h's "ROUND 168 UPDATE" comment for the
 * full design rationale, citations, and an honest flag of which parts
 * are directly psx-spx-cited vs. this project's own conservative
 * architectural inference (cross-node/cross-priority continuation).
 * ------------------------------------------------------------------ */
#include "core/hw/iop_intc.h"

typedef struct {
    int active;            /* a dispatch sequence is in flight */
    int trying_second;     /* currently on the CURRENT node's "second" function */
    uint32_t priority;     /* current priority chain index, 0-3 */
    uint32_t node;          /* current node address (as stored in RAM - may carry a KSEG0 prefix, same convention iop_mem_read32/write32 already handle transparently elsewhere in this file) */
    uint32_t dispatched_irq;
    uint32_t saved_epc;
} iop_excb_dispatch_t;

static iop_excb_dispatch_t g_excb_dispatch;

/* Finds the first non-empty chain at priority >= start_priority.
 * Returns 1 and fills *out_priority and *out_node if found, 0 otherwise. */
static int find_next_chain(iop_state_t *st, uint32_t start_priority, uint32_t *out_priority, uint32_t *out_node)
{
    for (uint32_t p = start_priority; p < IOP_EXCB_NUM_PRIO; p++) {
        uint32_t head = iop_mem_read32(st, chain_head_addr(st, p));
        if (head != 0u) {
            *out_priority = p;
            *out_node = head;
            return 1;
        }
    }
    return 0;
}

/* Jumps into a node's "first" (trying_second=0) or "second"
 * (trying_second=1) function - real, cited node layout: 08h=first
 * function, 04h=second function (iop_excb.h header comment). No real,
 * cited argument-register convention exists for these callbacks (the
 * header's own citation only documents the pointer layout and the
 * r2-based first/second handoff, not an ABI for arguments), so this
 * project deliberately does NOT fabricate one - registers other than
 * $ra/$pc/$next_pc are left exactly as the interrupted code left
 * them, same conservative choice already made elsewhere in this
 * project when no citable ABI exists. */
static void enter_node_function(iop_state_t *st, uint32_t node, int second)
{
    uint32_t func = iop_mem_read32(st, node + (second ? 0x04u : 0x08u));
    g_excb_dispatch.trying_second = second;
    st->gpr[31] = IOP_EXCB_DISPATCH_RETURN_TRAMPOLINE;
    st->pc = func;
    st->next_pc = func + 4u;
}

/* Advances the dispatch state machine to the next candidate (this
 * node's second function, or the next node, or the next priority's
 * head) and jumps into it. Returns 1 if a next candidate was found
 * and entered, 0 if every chain/node is exhausted. */
static int advance_and_enter(iop_state_t *st)
{
    if (!g_excb_dispatch.trying_second) {
        uint32_t second_func = iop_mem_read32(st, g_excb_dispatch.node + 0x04u);
        if (second_func != 0u) {
            enter_node_function(st, g_excb_dispatch.node, 1);
            return 1;
        }
    }

    /* This node is done (either the second function also declined, or
     * there was no second function to try) - move to the next node in
     * the same chain (real, cited: 00h = next-pointer, 0 = end). */
    uint32_t next_node = iop_mem_read32(st, g_excb_dispatch.node + 0x00u);
    if (next_node != 0u) {
        g_excb_dispatch.node = next_node;
        enter_node_function(st, next_node, 0);
        return 1;
    }

    /* Chain exhausted at this priority - try the next priority level. */
    uint32_t next_priority, next_head;
    if (find_next_chain(st, g_excb_dispatch.priority + 1u, &next_priority, &next_head)) {
        g_excb_dispatch.priority = next_priority;
        g_excb_dispatch.node = next_head;
        enter_node_function(st, next_head, 0);
        return 1;
    }

    return 0; /* every chain, every node, exhausted */
}

/* Real MIPS/R3000A Status.BEV-selected general exception vector - same
 * formula this project's own iop_check_hw_interrupt() already uses at
 * its default-vector fallback (source/core/iop/iop_core.c). Duplicated
 * here (rather than shared) because by the time this project reaches
 * "every ExCB candidate declined", control is no longer at that
 * function's own call site - it's inside this file's return
 * trampoline, reached one or more real guest instructions later. */
static void vector_to_default(iop_state_t *st)
{
    uint32_t vector = (st->cop0[12] & 0x400000u) ? 0xBFC00180u : 0x80000080u;
    st->pc = vector;
    st->next_pc = vector + 4u;
}

int iop_excb_dispatch_interrupt(iop_state_t *st, uint32_t irq)
{
    uint32_t priority, node;
    if (!find_next_chain(st, 0u, &priority, &node))
        return 0; /* every priority chain is empty - caller falls back to its own default vectoring, unchanged */

    g_excb.dispatch_attempts++;

    g_excb_dispatch.active = 1;
    g_excb_dispatch.priority = priority;
    g_excb_dispatch.node = node;
    g_excb_dispatch.dispatched_irq = irq;
    g_excb_dispatch.saved_epc = st->cop0[14]; /* EPC, already written by the caller before calling this */

    enter_node_function(st, node, 0); /* always try the "first" function first, real cited order */
    return 1;
}

int iop_excb_try_handle(iop_state_t *st, uint32_t pc)
{
    if (pc != IOP_EXCB_DISPATCH_RETURN_TRAMPOLINE || !g_excb_dispatch.active)
        return 0;

    uint32_t r2 = st->gpr[2];
    if (r2 == 0u) {
        /* Real, cited convention: r2==0 means this function claimed
         * the interrupt/exception. Finish exactly like a real RFE
         * would - same formula iop_hle_intr.c's own return trampoline
         * already uses. */
        iop_intc_state_t *intc = iop_intc_get_state();
        uint32_t irq = g_excb_dispatch.dispatched_irq;
        if (irq < 32u)
            intc->istat &= ~(1u << irq);
        else
            intc->istat_hi &= ~(1u << (irq - 32u));
        st->cop0[12] = (st->cop0[12] & ~0x0Fu) | ((st->cop0[12] >> 2) & 0x0Fu); /* Status stack pop, real RFE formula */
        st->pc = g_excb_dispatch.saved_epc;
        st->next_pc = g_excb_dispatch.saved_epc + 4u;
        g_excb_dispatch.active = 0;
        g_excb.dispatch_claimed++;
        return 1;
    }

    /* r2 != 0: this function declined - try the next candidate. */
    if (advance_and_enter(st))
        return 1;

    /* Everything declined - fall through to this project's existing
     * default-vector behavior, one level later than usual (see
     * vector_to_default()'s comment). Note: unlike the claimed case,
     * this does NOT pop the Status stack or ack the irq - matching
     * exactly what the caller's own pre-existing default-vector path
     * already did before this round (it never touched Status/irq
     * either; only the actual guest exception handler that eventually
     * runs at the vector is responsible for that, same as real
     * hardware). */
    vector_to_default(st);
    g_excb_dispatch.active = 0;
    g_excb.dispatch_exhausted++;
    return 1;
}
