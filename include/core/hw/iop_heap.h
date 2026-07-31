#ifndef PCSX2WII_HW_IOP_HEAP_H
#define PCSX2WII_HW_IOP_HEAP_H

#include <stdint.h>

/* Round 401 (task #128): real IOP heap allocator, ported from the
 * real "[RO]man" SYSMEM module source (sysmem.c/sysmem.h, September
 * 2002, user-uploaded this session, Round 397/398 archive
 * incorporation). This module is the real IOP-side backing for
 * SifAllocIopHeap()/SifFreeIopHeap() - previously this project
 * returned a hardcoded, non-tracking PLACEHOLDER address
 * (0x00001000) for every allocation request (task #203/80th
 * finding), which was explicitly flagged as "NOT a claim of real
 * heap tracking". This header/its .c file replace that placeholder
 * with a real, byte-exact port of SYSMEM's own free-list algorithm.
 *
 * Real allocation-strategy constants (from real sysmem.h):
 * ALLOC_FIRST/ALLOC_LAST/ALLOC_LATER select which free block a real
 * AllocSysMemory() picks (first-fit scanning from the front of the
 * free list / from the back / a specific caller-requested address
 * respectively). Only ALLOC_FIRST is currently driven by this
 * project's EE-side SifAllocIopHeap RPC handler (ee_core.c), but all
 * three are ported faithfully for completeness/fidelity to the real
 * source. */
#define IOP_HEAP_ALLOC_FIRST 0
#define IOP_HEAP_ALLOC_LAST  1
#define IOP_HEAP_ALLOC_LATER 2

/* Real QueryBlockTopAddress/QueryBlockSize high-bit convention
 * (sysmem.h): USED=0, FREE=0x80000000, OR'd into the low 31 bits of
 * block size/address results. */
#define IOP_HEAP_USED 0x00000000u
#define IOP_HEAP_FREE 0x80000000u

/* Reset the heap to a single free block spanning the whole managed
 * arena. Must be called once at IOP-core init (and again on any full
 * IOP reset) before the first AllocSysMemory-equivalent call. */
void iop_heap_init(void);

/* Real AllocSysMemory(flags, size, mem) port. Returns the allocated
 * GUEST IOP address (already within the managed arena, 256-byte
 * aligned - real hardware's own allocation unit), or 0 on failure
 * (matches real hardware's own "NULL means failed" convention - see
 * ee_core.c's SifAllocIopHeap citation). `mem` is only consulted for
 * ALLOC_LATER (a specific requested address); pass 0 for
 * ALLOC_FIRST/ALLOC_LAST. */
uint32_t iop_heap_alloc(int flags, uint32_t size, uint32_t mem);

/* Real FreeSysMemory(mem) port. Returns 0 on success, -1 on failure
 * (block not found / not 256-aligned / double-free), matching real
 * semantics exactly. */
int iop_heap_free(uint32_t mem);

/* Real QueryMaxFreeMemSize()/QueryTotalFreeMemSize() ports. */
uint32_t iop_heap_query_max_free(void);
uint32_t iop_heap_query_total_free(void);

/* Real QueryBlockSize(address) port: returns (size_in_bytes |
 * IOP_HEAP_USED/IOP_HEAP_FREE), or -1 if no block contains `address`
 * (matches real -1-on-miss convention). */
int32_t iop_heap_query_block_size(uint32_t address);

#endif /* PCSX2WII_HW_IOP_HEAP_H */
