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

/* Round 761 (task #762, user-approved deliberate exception to this
 * project's standing "no fabrication" discipline - see docs/STATUS.md
 * "Round 761" for the full writeup and the user's explicit sign-off,
 * 2026-09-01): SYSMEM's real ordinal-10 export, QueryBlockSize(void
 * *address) (I_QueryBlockSize = DECLARE_IMPORT(10, QueryBlockSize),
 * ps2sdk sysmem.h, fetched live from
 * https://ps2dev.github.io/ps2sdk/sysmem_8h.html), is NOT resolved via
 * the normal dynamic import-table mechanism the five sentinel gates
 * above intercept. Round 748 confirmed via direct BIOS ROM-byte
 * disassembly that its own stub body is a hardcoded, unconditional
 * `j 0x0000044C` baked directly into SYSMEM's own loaded module code -
 * a real, fixed low-IOP-RAM address that genuinely holds Sony's own
 * QueryBlockSize function body on real hardware, content this project
 * has no legitimate way to obtain or transcribe (its standing clean-
 * room convention). In this project's own model, that same address
 * currently holds EXCEPMAN's unrelated handler-table data instead
 * (Round 757), which the pre-existing Round-173 tripwire eventually
 * and correctly halts on once GT3's own real code walks off the end
 * of it (Round 759's full root-cause writeup, and Round 760's
 * companion fixed-address-loading fix that also touches this call
 * chain).
 *
 * This constant is deliberately NOT placed in the invented-sentinel
 * range above (0x220-0x230) - unlike those five, 0x0000044C is not a
 * synthetic address this project chose; it is Sony's own real,
 * hardcoded jump target, confirmed by ROM bytes, and the intercept
 * below fires only for genuine arrivals at that exact real address
 * (Round 758 already confirmed nothing else in this project's model
 * ever fetches from 0x400-0x4C0 during any other boot path).
 *
 * Per the user's explicit instruction, this gate provides a synthetic
 * stand-in for the missing real function body so GT3's checkpoint
 * chain can proceed past this exact wall and reveal what (if
 * anything) blocks progress further downstream - understanding this
 * does NOT reconstruct real Sony ROM bytes and is NOT claimed to be
 * evidence-backed the way this project's other shipped fixes are.
 * It turned out to need far less fabrication than expected, though:
 * the real QueryBlockSize(address) computation itself already has a
 * complete, real, byte-exact ported implementation sitting unused in
 * this codebase since Round 401 -
 * iop_heap_query_block_size() (core/hw/iop_heap.h), built from the
 * same uploaded real September-2002 SYSMEM source this project already
 * used for AllocSysMemory/FreeSysMemory/QueryMemSize/etc (the five
 * gates above) - it was simply never wired to any real IOP-side call
 * site, because nothing reached it until Round 759's GT3 checkpoint
 * chain confirmed this exact call fires. So the only fabricated
 * choice here is intercepting fetch at Sony's real address at all and
 * treating every arrival there as this one identified call; the
 * arithmetic behind the returned answer is a real, already-tested,
 * real-source port, not invented. */
#define IOP_HLE_HEAP_SYSMEM_ORDINAL10_QUERYBLOCKSIZE 0x0000044Cu

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
