/*
 * iop_hle_heap.c - Round 421 (task #160): real SYSMEM heap-export
 * sentinel gates. See include/core/hw/iop_hle_heap.h for the full
 * design rationale and citations. Implementation follows the exact
 * "sentinel call-gate" template established by iop_hle_thread.c's
 * CREATETHREAD case (read args via iop_mem_read32/registers, do the
 * real logic against this project's own already-tested model, set
 * $v0, then simulate the return via st->pc = ra; st->next_pc = ra+4).
 */

#include <string.h>
#include <stdint.h>
#include "core/hw/iop_hle_heap.h"
#include "core/hw/iop_heap.h"
#include "core/iop/iop_core.h"

/* Mirrors iop_heap.c's own IOP_HEAP_SIZE constant exactly (851968
 * bytes / 0x000D0000, the whole managed arena) - iop_heap.h does not
 * expose it directly, so QueryMemSize's real "total managed memory"
 * semantics are served from this local copy of the same real,
 * already-cited constant rather than adding a new accessor for a
 * single read-only value. */
#define IOP_HLE_HEAP_TOTAL_SIZE 0x000D0000u

uint32_t iop_hle_heap_sentinel_for_import(const char *module_name, uint32_t ordinal)
{
    if (!module_name) return 0;
    if (strncmp(module_name, "sysmem", 8) == 0) {
        switch (ordinal) {
            case 4: return IOP_HLE_HEAP_ALLOC_SYS_MEMORY;
            case 5: return IOP_HLE_HEAP_FREE_SYS_MEMORY;
            case 6: return IOP_HLE_HEAP_QUERY_MEM_SIZE;
            case 7: return IOP_HLE_HEAP_QUERY_MAX_FREE_MEM_SIZE;
            case 8: return IOP_HLE_HEAP_QUERY_TOTAL_FREE_MEM_SIZE;
            default: return 0;
        }
    }
    return 0;
}

int iop_hle_heap_try_handle(iop_state_t *st, uint32_t pc)
{
    int in_range =
        (pc >= IOP_HLE_HEAP_ALLOC_SYS_MEMORY && pc <= IOP_HLE_HEAP_QUERY_TOTAL_FREE_MEM_SIZE);
    if (!in_range) return 0;

    uint32_t ra = st->gpr[31];

    if (pc == IOP_HLE_HEAP_ALLOC_SYS_MEMORY) {
        /* void *AllocSysMemory(int mode, int size, void *ptr) - real
         * signature cited in this file's header (ps2sdk sysmem.h).
         * a0=mode, a1=size, a2=ptr (only meaningful for ALLOC_LATER;
         * iop_heap_alloc() itself only consults it in that case). */
        int mode = (int)st->gpr[4];
        uint32_t size = st->gpr[5];
        uint32_t ptr = st->gpr[6];
        uint32_t result = iop_heap_alloc(mode, size, ptr);
        st->gpr[2] = result; /* real: NULL/0 on failure, matches iop_heap_alloc()'s own convention */
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_HEAP_FREE_SYS_MEMORY) {
        /* int FreeSysMemory(void *ptr) - a0=ptr. */
        uint32_t ptr = st->gpr[4];
        int result = iop_heap_free(ptr);
        st->gpr[2] = (uint32_t)result; /* real: 0 success, -1 failure */
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_HEAP_QUERY_MEM_SIZE) {
        /* int QueryMemSize(void) - no args; real semantics: total
         * managed memory size of the SYSMEM arena. */
        st->gpr[2] = IOP_HLE_HEAP_TOTAL_SIZE;
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_HEAP_QUERY_MAX_FREE_MEM_SIZE) {
        st->gpr[2] = iop_heap_query_max_free();
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_HEAP_QUERY_TOTAL_FREE_MEM_SIZE) {
        st->gpr[2] = iop_heap_query_total_free();
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    return 0;
}
