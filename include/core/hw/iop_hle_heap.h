#ifndef PCSX2WII_HW_IOP_HLE_HEAP_H
#define PCSX2WII_HW_IOP_HLE_HEAP_H

#include <stdint.h>
#include "core/iop/iop_core.h"

/* Round 421 (task #160): real sysmem-export sentinel gates.
 *
 * DESIGN: exactly the same "sentinel call-gate + intercept before
 * fetch" mechanism already established for INTRMAN/EXCEPMAN (task
 * #109, iop_hle_intr.h) and THREADMAN (Round 389, iop_hle_thread.h) -
 * see iop_hle_intr_sentinel_for_import()'s own precedent, which this
 * file follows exactly.
 *
 * WHY THIS IS NEEDED (see docs/STATUS.md Round 420 for the full,
 * live-disassembly-confirmed root cause): with Round 409's SYSMEM
 * $a0 fix, real guest-side MIPS code now genuinely calls through to
 * SYSMEM's own real AllocSysMemory()/FreeSysMemory() exports (via
 * HEAPLIB's real "create pool" logic, per Round 407-409's own already-
 * documented call chain) for the first time ever. Real SYSMEM's own
 * interpreted code manages a heap spanning nearly all of IOP RAM
 * (real hardware convention - see iop_heap.c's own header comment),
 * with zero awareness of this project's own SEPARATE, synthetic
 * module-loading bump allocator (iop_module_loader.c's bump_alloc(),
 * also starting at BUMP_BASE=0x00100000) - live write-watch evidence
 * (Round 420) directly caught real HEAPLIB/SYSCLIB code allocating
 * and writing over already-loaded LOADCORE module code as a direct
 * result.
 *
 * FIX: redirect the specific real (library="sysmem", ordinal) import
 * calls that would otherwise run real, un-coordinated SYSMEM
 * interpreted code to this project's own ALREADY-BUILT, ALREADY-
 * TESTED synthetic heap model instead (iop_heap.c, Round 401 -
 * previously only reachable via the EE-side SifAllocIopHeap RPC
 * shortcut, now also reachable from genuine IOP-side module calls).
 * iop_heap.c's own managed arena (0x00020000-0x000F0000, documented
 * in its own header as deliberately chosen to avoid every region
 * this project's other subsystems already claim, INCLUDING
 * BUMP_BASE) already does not collide with the module-loading arena -
 * reusing it here is a direct, minimal, low-risk fix that needs no
 * new address-space bookkeeping of its own. */

/* Real (library, ordinal) citations - ps2sdk sysmem.h, fetched live
 * from https://ps2dev.github.io/ps2sdk/sysmem_8h.html (Round 421):
 *   I_AllocSysMemory          = DECLARE_IMPORT(4, AllocSysMemory)
 *   I_FreeSysMemory           = DECLARE_IMPORT(5, FreeSysMemory)
 *   I_QueryMemSize            = DECLARE_IMPORT(6, QueryMemSize)
 *   I_QueryMaxFreeMemSize     = DECLARE_IMPORT(7, QueryMaxFreeMemSize)
 *   I_QueryTotalFreeMemSize   = DECLARE_IMPORT(8, QueryTotalFreeMemSize)
 * Sentinel addresses chosen in the first free gap above
 * IOP_HLE_THREAD_ALARM_RETURN_TRAMPOLINE (0x00000210, the highest
 * sentinel address already claimed by iop_hle_thread.h) - well clear
 * of every other HLE gate's own claimed range. */
#define IOP_HLE_HEAP_ALLOC_SYS_MEMORY        0x00000220u
#define IOP_HLE_HEAP_FREE_SYS_MEMORY         0x00000224u
#define IOP_HLE_HEAP_QUERY_MEM_SIZE          0x00000228u
#define IOP_HLE_HEAP_QUERY_MAX_FREE_MEM_SIZE 0x0000022Cu
#define IOP_HLE_HEAP_QUERY_TOTAL_FREE_MEM_SIZE 0x00000230u

/* Same contract as iop_hle_intr_sentinel_for_import()/
 * iop_hle_thread_sentinel_for_import(): returns the sentinel address
 * to redirect an import stub to for a (library name, ordinal) pair
 * this file handles, 0 if this file has no gate for that pair (in
 * which case normal resolution against the real, live-interpreted
 * target proceeds as before - this is a narrow, additive gate, not a
 * blanket replacement of real SYSMEM interpretation for every export). */
uint32_t iop_hle_heap_sentinel_for_import(const char *module_name, uint32_t ordinal);

/* Called from iop_step() before fetch, same spot/priority as
 * iop_hle_intr_try_handle()/iop_hle_thread_try_handle(). Returns 1 if
 * pc was one of this file's sentinel addresses (and was fully
 * handled - registers set, pc redirected to $ra), 0 otherwise. */
int iop_hle_heap_try_handle(iop_state_t *st, uint32_t pc);

#endif
