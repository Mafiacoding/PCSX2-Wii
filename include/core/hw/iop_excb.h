/*
 * iop_excb.h - IOP kernel exception-handler priority chains (ExCB),
 * the real mechanism rooted at RAM[0x100].
 *
 * Round 22 (see docs/STATUS.md and docs/ROADMAP.md section 2):
 * completes the citable reference this project was missing when
 * Round 19 first found the real BIOS-resident generic exception
 * dispatcher (0x00000c80-0x00000e30) looking up a handler through a
 * chain rooted at RAM[0x100], with no real handler ever registered.
 * The full reference (fetched from psx-spx's kernelbios.md, "BIOS
 * Interrupt/Exception Handling" section - "Priority Chains",
 * "C(02h) - SysEnqIntRP", "C(03h) - SysDeqIntRP") describes exactly
 * this mechanism:
 *
 *   RAM[0x100]/RAM[0x104] (the first "Table of Tables" entry, see
 *   the BIOS RAM Map) is a pointer+size pair: RAM[0x100] = address of
 *   a real array of 4 "ExCB" priority-chain-head pointers (one per
 *   priority level 0-3), RAM[0x104] = 4*08h (the array's byte size -
 *   4 entries, 8 bytes each, matching this project's own 8-byte-
 *   aligned chain-node layout below).
 *
 *   Each chain is a singly-linked list of 16-byte nodes:
 *     00h  4  pointer to next element (0 = end of chain) - written
 *             BY THE BIOS itself when a node is inserted (SysEnqIntRP)
 *     04h  4  pointer to "second" function (called if the first
 *             function returns r2 != 0 - real hardware detail, not
 *             modeled further here, see the scope note below)
 *     08h  4  pointer to "first" function (the real handler body -
 *             called first)
 *     0Ch  4  not used (reserved, always zero on real hardware)
 *
 *   SysEnqIntRP(priority, struc) inserts `struc` (a caller-owned
 *   16-byte node, with 04h/08h already filled in by the caller)
 *   at the HEAD of the given priority's chain - real, documented
 *   behavior, always newest-first, never appended.
 *
 *   SysDeqIntRP(priority, struc) removes `struc` from the chain -
 *   but real hardware has a well-documented, cited BUG here: it can
 *   only correctly remove the FIRST element of the chain (comparing
 *   `struc` against the current head and unlinking via the head's own
 *   next-pointer); for any OTHER position, real hardware "reads a
 *   garbage value from an uninitialized stack location, and acts
 *   more or less unpredictable." This project deliberately does NOT
 *   synthesize a specific garbage-dependent outcome for that case
 *   (there is nothing citable to reproduce - the real behavior is
 *   documented as undefined) - it is modeled as a safe no-op instead,
 *   which is a defensible, conservative stand-in for "unpredictable"
 *   that won't corrupt this project's own state, while the
 *   first-element case (the one real software is expected to rely
 *   on) is fully real and byte-exact.
 *
 * SCOPE NOTE, same rationale as iop_hle_bios.h/iop_hle_modules.h:
 * this implements the real CONTAINER/MECHANISM (the chain-of-nodes
 * data structure and its two manipulation primitives) byte-exactly,
 * because that part is fully, precisely documented. It does NOT
 * implement the real DEFAULT HANDLER CONTENTS a real BIOS installs
 * via EnqueueSyscallHandler(C0h/0x01)/EnqueueTimerAndVblankIrqs(C0h/
 * 0x00)/InitDefInt(C0h/0x0C) - those would require the actual real
 * BIOS-ROM machine code bodies of CdromDmaIrq/CdromIoIrq/
 * SyscallException/VblankIrq/Timer0-2Irq/CardSpecificIrq/PadCardIrq/
 * DefInt, which this project has no verified byte-for-byte reference
 * for (same "don't fabricate real BIOS internals" rule as everywhere
 * else in this project). RAM[0x100]'s chain array is correctly
 * initialized to all-empty (matching "before any handler is
 * registered", the exact scenario Round 19's trace hit) rather than
 * pre-populated with invented handler bodies.
 */
#ifndef PCSX2_WII_IOP_EXCB_H
#define PCSX2_WII_IOP_EXCB_H

#include <stdint.h>
#include "core/iop/iop_core.h"

#define IOP_EXCB_TABLE_ADDR 0x00000100u /* RAM[0x100]: pointer to the chain-head array */
#define IOP_EXCB_TABLE_SIZE 0x00000020u /* RAM[0x104]: 4 * 08h, per psx-spx's "Table of Tables" */
#define IOP_EXCB_NUM_PRIO   4u
#define IOP_EXCB_ARRAY_ADDR 0x0000E000u /* start of the documented "Kernel Memory" region */
#define IOP_KMEM_REGION_SIZE 0x00002000u /* psx-spx BIOS RAM Map: "0000E000h 2000h
                                            * Kernel Memory; ExCBs, EvCBs, and TCBs
                                            * allocated via B(00h)" - see
                                            * iop_hle_bios.h's IOP_HLE_B0_ALLOC_KERNEL_MEMORY
                                            * comment for the real allocator this bounds. */

typedef struct {
    uint64_t enq_calls;
    uint64_t deq_calls;
    uint64_t deq_bug_noop_count; /* SysDeqIntRP called on a non-head element - see header note */
} iop_excb_state_t;

/* Writes RAM[0x100]/RAM[0x104] and zeroes the 4-entry chain-head
 * array at IOP_EXCB_ARRAY_ADDR. Called once from iop_core_init(). */
void iop_excb_init(iop_state_t *st);

/* Real SysEnqIntRP(priority, struc) - see header comment. priority
 * must be 0-3 (per the documented 4 priority chains); out-of-range
 * values are ignored (real hardware behavior for this case isn't
 * documented/cited, so this project doesn't guess at it). */
void iop_excb_sys_enq_int_rp(iop_state_t *st, uint32_t priority, uint32_t struc);

/* Real (bug-preserving) SysDeqIntRP(priority, struc) - see header
 * comment for the documented first-element-only removal behavior. */
void iop_excb_sys_deq_int_rp(iop_state_t *st, uint32_t priority, uint32_t struc);

iop_excb_state_t *iop_excb_get_state(void);

#endif
