/* Round 401 (task #128): real IOP heap allocator.
 *
 * Ported from the real "[RO]man" SYSMEM module source (sysmem.c,
 * September 2002, user-uploaded, Round 397/398 archive
 * incorporation). SYSMEM is the real IOP kernel module backing
 * AllocSysMemory()/FreeSysMemory() - the real functions
 * SifAllocIopHeap()/SifFreeIopHeap() (EE-side, ee/kernel/src/
 * iopheap.c, already cited in ee_core.c) call into via SIF RPC
 * (sid=SIF_SID_IOPHEAP, fno=1).
 *
 * This is a byte-faithful port of SYSMEM's real free-list algorithm:
 * the same packed-bitfield `info` word layout (mALLOCATED/mADDRESS/
 * mSIZE, 256-byte allocation units, 15-bit address + 15-bit size),
 * the same smFIRST/smFIRSTF/smCHECK/smLAST/smMAX table-slot
 * constants, the same ALLOC_FIRST/ALLOC_LAST/ALLOC_LATER strategy
 * bodies (transcribed from the real source's `alloc()` switch,
 * including its real "shuffle remaining free space forward through
 * the list" mechanism), and the same free()-with-coalescing /
 * maintain()-with-table-growth behavior.
 *
 * One deliberate, explicitly-documented simplification: real SYSMEM
 * stores its own allocTABLE bookkeeping structs INSIDE the very IOP
 * RAM it manages (self-hosting - see the real sysmem_init_memory()'s
 * own self-allocating AllocSysMemory(ALLOC_FIRST, sizeof(allocTABLE),
 * NULL) call). Nothing in this project's boot trace ever reads
 * SYSMEM's real table bytes as raw guest memory (the EE-side
 * SifAllocIopHeap request is answered by this project's own
 * synthetic SIF-RPC-reply shortcut in ee_core.c, not by genuine IOP-
 * side MIPS execution of real sysmem.c code), so there is no
 * observable difference between storing the bookkeeping tables in
 * guest RAM bytes versus host-native C structs. This port therefore
 * keeps the allocTABLE chain as host-native memory (malloc'd once
 * per table, exactly mirroring the real maintain()'s one-table-at-a-
 * time growth condition) while every ADDRESS the algorithm computes
 * and returns is a genuine guest IOP RAM address within the managed
 * arena - the real address-space bookkeeping semantics are preserved
 * exactly, only the location of the bookkeeping structs themselves
 * (guest bytes vs. host struct) differs, and that difference is
 * unobservable from the modeled system's perspective.
 *
 * Managed arena placement: real SYSMEM's own arena starts around
 * real IOP address 0x1500 and spans nearly all of real IOP RAM. This
 * project does not track every real module's own IOP RAM footprint
 * precisely enough to safely reuse that literal real base without
 * risking an aliasing collision with this project's own already-
 * modeled regions (0x00000000-0x00000100 HLE trap/trampoline
 * sentinels, 0x0000E000-0x00010000 IOP_EXCB_ARRAY_ADDR/kernel-memory
 * region - see iop_core.c/iop_excb.h -, 0x00100000-up BUMP_BASE
 * module/boot-info loader arena - see iop_module_loader.c -,
 * 0x00180000-0x001F0000 thread stack arena, 0x001FFF00 initial SP -
 * see iop_hle_thread.c). 0x00020000-0x000F0000 (896000 bytes, well
 * within SYSMEM's real 15-bit/8MB-minus-256 addressing limit) sits
 * entirely in the large gap between the kernel-memory region and
 * BUMP_BASE that no other subsystem currently claims - chosen as a
 * safe, explicitly-scoped reservation for this new subsystem, not a
 * claim about real hardware's own literal address layout. */

#include "core/hw/iop_heap.h"
#include <stdlib.h>
#include <string.h>

/* Real bit-packed `info` word layout (sysmem.c):
 *  [ size:15 | unknown:1 | address:15 | allocated:1 ]
 * upper 15 bits = block size in 256-byte units (8MB addressability)
 * next 15 bits  = block address in 256-byte units (8MB addressability)
 * low bit       = allocated flag */
#define M_ALLOCATED(info) ((info) & 1u)
#define M_ADDRESS(info)   (((info) >> 1) & 0x7FFFu)
#define M_SIZE(info)      ((info) >> 17)

/* Real smFIRST/smFIRSTF/smCHECK/smLAST/smMAX constants. */
#define SM_FIRST  0
#define SM_FIRSTF 2
#define SM_CHECK  27
#define SM_LAST   30
#define SM_MAX    (SM_LAST + 1) /* 31 */

struct alloc_elem {
    struct alloc_elem *next;
    uint32_t info;
};

struct alloc_table {
    struct alloc_table *next;
    struct alloc_elem list[SM_MAX];
};

/* Managed arena - see file header comment for the placement
 * rationale. Real EIGHTMEGSm256 (8*1024*1024-256) is SYSMEM's own
 * real maximum-addressable-size clamp; our arena is far smaller than
 * that real ceiling, so it is never hit. */
#define IOP_HEAP_BASE 0x00020000u
#define IOP_HEAP_SIZE 0x000D0000u /* 851968 bytes = 3328 * 256 */

static struct alloc_table *g_alloclist;

static struct alloc_table *new_table(void) {
    struct alloc_table *t = (struct alloc_table *)malloc(sizeof(struct alloc_table));
    int i;
    if (!t) return NULL;
    t->next = NULL;
    for (i = SM_FIRST; i < SM_MAX; i++) {
        t->list[i].next = (i + 1 < SM_MAX) ? &t->list[i + 1] : NULL;
        t->list[i].info = 0u;
    }
    return t;
}

void iop_heap_init(void) {
    struct alloc_table *t;
    /* Free any previously-allocated host tables (a full IOP reset
     * re-inits the heap from scratch, matching real sysmem_start()
     * being re-run on real IOP reboot). */
    while (g_alloclist) {
        struct alloc_table *n = g_alloclist->next;
        free(g_alloclist);
        g_alloclist = n;
    }
    t = new_table();
    if (!t) return;
    g_alloclist = t;
    /* Real sysmem_init_memory(): single free block covering the
     * whole managed size, address = arena base (in 256-byte units),
     * not allocated. */
    g_alloclist->list[SM_FIRST].info =
        ((IOP_HEAP_SIZE >> 8) << 17) | ((IOP_HEAP_BASE >> 8) << 1) | 0u;
}

static struct alloc_table *last_table(void) {
    struct alloc_table *t = g_alloclist;
    if (!t) return NULL;
    while (t->next) t = t->next;
    return t;
}

/* Real alloc() port - ALLOC_FIRST/ALLOC_LAST/ALLOC_LATER cases
 * transcribed directly from the real source's bit-shuffle logic. */
static uint32_t do_alloc(int flags, uint32_t size, uint32_t mem) {
    struct alloc_elem *a, *last, *k;
    uint32_t bsize, baddress, i, tmp, address;

    if (!g_alloclist) return 0u;

    bsize = (size + 255u) >> 8; /* round up to 256-byte units */
    if (bsize == 0u) return 0u;

    switch (flags) {
    case IOP_HEAP_ALLOC_FIRST:
        for (a = g_alloclist->list; a; a = a->next)
            if (!M_ALLOCATED(a->info) && M_SIZE(a->info) >= bsize)
                break;
        if (a == NULL) return 0u;

        if (M_SIZE(a->info) == bsize) {
            a->info |= 1u;
            return M_ADDRESS(a->info) << 8;
        }

        address = M_ADDRESS(a->info) << 8;
        i = a->info;

        a->info = ((a->info | 1u) & 0x1FFFFu) | (bsize << 17);

        i = (i & 0xFFFF0001u) | (((M_ADDRESS(i) + bsize) & 0x7FFFu) << 1);
        i = (i & 0x1FFFFu) | ((M_SIZE(i) - bsize) << 17);

        a = a->next;
        while (a && M_SIZE(i) > 0u) {
            tmp = a->info;
            a->info = i;
            i = tmp;
            a = a->next;
        }
        return address;

    case IOP_HEAP_ALLOC_LAST:
        last = NULL;
        for (a = g_alloclist->list; a; a = a->next)
            if (!M_ALLOCATED(a->info) && M_SIZE(a->info) >= bsize)
                last = a;
        a = last;
        if (a == NULL) return 0u;

        if (M_SIZE(a->info) == bsize) {
            a->info |= 1u;
            return M_ADDRESS(a->info) << 8;
        }

        a->info = (a->info & 0x0001FFFFu) | ((M_SIZE(a->info) - bsize) << 17);

        i = ((((M_ADDRESS(a->info) + M_SIZE(a->info)) & 0x7FFFu) << 1)
             & 0x0001FFFFu)
            | (bsize << 17) | 1u;
        a = a->next;

        address = M_ADDRESS(i) << 8;
        while (a && M_SIZE(i) > 0u) {
            tmp = a->info;
            a->info = i;
            i = tmp;
            a = a->next;
        }
        return address;

    case IOP_HEAP_ALLOC_LATER:
        if (mem & 0xFFu) return 0u;
        baddress = mem >> 8;
        for (a = g_alloclist->list; a; a = a->next) {
            if (baddress < M_ADDRESS(a->info)) return 0u;
            if (!M_ALLOCATED(a->info) &&
                (M_SIZE(a->info) + M_ADDRESS(a->info) >= baddress + bsize))
                break;
        }
        if (a == NULL) return 0u;

        if (M_ADDRESS(a->info) < baddress) {
            tmp = M_ADDRESS(a->info) + M_SIZE(a->info) - baddress;
            a->info = (a->info & 0x1FFFFu) | ((M_SIZE(a->info) - tmp) << 17);

            i = (((((M_ADDRESS(a->info) + M_SIZE(a->info)) & 0x7FFFu) << 1))
                 & 0x1FFFEu)
                | (tmp << 17);

            k = a = a->next;
            while (a && M_SIZE(i) > 0u) {
                tmp = a->info;
                a->info = i;
                i = tmp;
                a = a->next;
            }
            a = k;
        }

        if (a == NULL) return 0u;

        if (M_SIZE(a->info) == bsize) {
            a->info |= 1u;
            return M_ADDRESS(a->info) << 8;
        }

        address = M_ADDRESS(a->info) << 8;
        i = a->info;

        a->info = ((a->info | 1u) & 0x1FFFFu) | (bsize << 17);

        i = (i & 0xFFFF0001u) | (((M_ADDRESS(i) + bsize) & 0x7FFFu) << 1);
        i = (i & 0x1FFFFu) | ((M_SIZE(i) - bsize) << 17);

        a = a->next;
        while (a && M_SIZE(i) > 0u) {
            tmp = a->info;
            a->info = i;
            i = tmp;
            a = a->next;
        }
        return address;
    }

    return 0u;
}

/* Real maintain() port: host-malloc a new table when SM_CHECK's slot
 * has become non-empty (same real growth trigger condition), free
 * the last table when it has become entirely empty again. Real
 * source recursively calls its own alloc() to carve the new table's
 * storage out of managed memory (self-hosting - see file header);
 * this port host-mallocs the table struct directly instead, since
 * nothing reads these bytes as guest memory (see file header). */
static void do_maintain(void) {
    struct alloc_table *table, *nt;

    table = last_table();
    if (!table) return;

    if (M_SIZE(table->list[SM_CHECK].info) > 0u) {
        nt = new_table();
        if (nt) {
            table->next = nt;
            table->list[SM_LAST].next = &nt->list[SM_FIRST];
        }
    }

    table = g_alloclist;
    if (table && table->next) {
        while (table->next->next) table = table->next;
        if (table->next && M_SIZE(table->list[SM_CHECK].info) == 0u) {
            struct alloc_table *t = table->next;
            table->list[SM_LAST].next = NULL;
            table->next = NULL;
            free(t);
        }
    }
}

/* Real free() port, including real coalesce-with-next/coalesce-with-
 * previous-free-block logic. */
static int do_free(uint32_t mem) {
    struct alloc_elem *a, *p, *n;
    int skip;

    if (!g_alloclist) return -1;
    if (mem & 0xFFu) return -1; /* only 256-byte multiples */

    /* Real free() scans starting at smFIRSTF (index 2), not
     * smFIRST (index 0) - real sysmem_init_memory() self-allocates
     * TWO blocks for its own table storage before any user
     * allocation can happen (self-hosting - see file header), always
     * consuming list[0]/list[1], so real user blocks can never
     * legitimately start before index 2 and free() need not scan
     * there. This port's bookkeeping is host-native (nothing to
     * self-host - see file header), so there are no reserved slots
     * to skip; scanning from SM_FIRST (index 0) is the correct
     * adaptation for that difference, not a fidelity loss to any
     * real OBSERVABLE behavior (a real caller's block is always
     * found either way - this port just doesn't waste two slots on
     * a self-reservation it doesn't need). */
    p = NULL;
    for (a = &g_alloclist->list[SM_FIRST]; a; a = a->next) {
        if (M_SIZE(a->info) && (M_ADDRESS(a->info) == (mem >> 8))) break;
        p = a;
    }
    if (a == NULL) return -1;              /* block not found */
    if (!M_ALLOCATED(a->info)) return -1;  /* cannot free a freed block */

    n = NULL;
    skip = 0;

    a->info &= 0xFFFFFFFEu; /* free the block */

    if (a->next && !M_ALLOCATED(a->next->info)) { /* bind with next free block */
        n = a->next;
        skip = 1;
        a->info = (a->info & 0x1FFFEu) | ((M_SIZE(a->info) + M_SIZE(a->next->info)) << 17);
    }

    if (p && !M_ALLOCATED(p->info)) { /* bind with previous free block */
        n = a;
        skip++;
        p->info = (p->info & 0x1FFFFu) | ((M_SIZE(p->info) + M_SIZE(a->info)) << 17);
    }

    if (skip && n) {
        a = n;
        while (--skip != -1 && a) a = a->next;
        while (a) {
            n->info = a->info;
            a = a->next;
            n = n->next;
        }
    }
    return 0;
}

/* Real findblock(): a SINGLE linear walk following list[].next only
 * (NOT a nested per-table loop) - real maintain() links the last
 * element of one table's list directly to the first element of the
 * next table's list (`table->list[smLAST].next =
 * &new->list[smFIRST]`), so one continuous list[].next chain already
 * spans every chained table. A nested "for each table, for each
 * list[]" loop would double-walk every table after the first, since
 * table->list[0]->...->next already reaches into subsequent tables
 * via that same real chaining. */
static struct alloc_elem *findblock(uint32_t a_addr) {
    struct alloc_elem *p;
    if (!g_alloclist) return NULL;
    for (p = g_alloclist->list; p; p = p->next)
        if (a_addr >= (M_ADDRESS(p->info) << 8) &&
            a_addr < ((M_ADDRESS(p->info) << 8) + (M_SIZE(p->info) << 8)))
            return p;
    return NULL;
}

uint32_t iop_heap_alloc(int flags, uint32_t size, uint32_t mem) {
    uint32_t r;
    if (!g_alloclist || flags >= 3) return 0u;
    r = do_alloc(flags, size, mem);
    do_maintain();
    return r;
}

int iop_heap_free(uint32_t mem) {
    int r = do_free(mem);
    if (r == 0) do_maintain();
    return r;
}

uint32_t iop_heap_query_max_free(void) {
    uint32_t maxfree = 0u;
    struct alloc_elem *p;
    if (!g_alloclist) return 0u;
    for (p = g_alloclist->list; p; p = p->next)
        if (!M_ALLOCATED(p->info) && M_SIZE(p->info) > maxfree)
            maxfree = M_SIZE(p->info);
    return maxfree << 8;
}

uint32_t iop_heap_query_total_free(void) {
    uint32_t freesize = 0u;
    struct alloc_elem *p;
    if (!g_alloclist) return 0u;
    for (p = g_alloclist->list; p; p = p->next)
        if (!M_ALLOCATED(p->info))
            freesize += M_SIZE(p->info);
    return freesize << 8;
}

int32_t iop_heap_query_block_size(uint32_t address) {
    struct alloc_elem *p = findblock(address);
    if (!p) return -1;
    return (int32_t)((M_SIZE(p->info) << 8) |
                      (M_ALLOCATED(p->info) ? IOP_HEAP_USED : IOP_HEAP_FREE));
}

/* Round 448 (task #247): host-native checkpoint/resume support.
 *
 * g_alloclist is this file's one piece of host-heap-allocated state
 * (malloc()'d alloc_table nodes - see file header for why the
 * bookkeeping lives in host memory rather than guest RAM bytes).
 * This project's test-only host-native checkpoint tooling
 * (driver_r313.c, never committed/shipped) does a raw byte dump/
 * restore of this file's [__data_start,_end) static-storage range,
 * which includes the g_alloclist POINTER itself - but the pointee
 * memory it references was malloc()'d by a DIFFERENT process (the
 * one that wrote the checkpoint) and does not exist in the resuming
 * process's address space, so blindly restoring the raw pointer
 * value and later dereferencing it faults (observed directly as
 * "[R313-SIGSEGV] fault at addr=..." in Rounds 307-447's
 * checkpoint/resume attempts - this was the root cause, isolated by
 * Round 448 confirming this file is the ONLY translation unit in
 * source/ that calls malloc()/calloc()/strdup() at all).
 *
 * Fix: expose an explicit save/load pair (mirroring how
 * driver_r313.c already explicitly re-points ee->ram/iop->ram rather
 * than trusting their raw restored pointer values) that serializes
 * only the process-independent state - each table's list[]
 * info-bitfield array, in chain order - and reconstructs the chain
 * from scratch in the resuming process on load, using fresh
 * malloc()'d nodes and the exact same deterministic next-pointer
 * wiring new_table()/do_maintain() already use (within-table
 * &list[i+1] chaining, cross-table &next_table->list[SM_FIRST]
 * stitching at each table boundary). The `next` pointers themselves
 * are never part of the serialized data and never need to be -
 * do_alloc()/do_free() only ever shuffle `info` values between fixed
 * slots (see their "tmp = a->info; a->info = i; i = tmp;" swap
 * pattern throughout this file), so a table's link structure is
 * always exactly what new_table()/do_maintain() built it as. */

uint32_t iop_heap_snapshot_size(void) {
    uint32_t count = 0u;
    struct alloc_table *t;
    for (t = g_alloclist; t; t = t->next) count++;
    return (uint32_t)sizeof(uint32_t) + count * (uint32_t)(SM_MAX * sizeof(uint32_t));
}

void iop_heap_snapshot_save(void *buf) {
    uint8_t *p = (uint8_t *)buf;
    uint32_t count = 0u, i;
    struct alloc_table *t;
    for (t = g_alloclist; t; t = t->next) count++;
    memcpy(p, &count, sizeof(count));
    p += sizeof(count);
    for (t = g_alloclist; t; t = t->next) {
        for (i = 0u; i < (uint32_t)SM_MAX; i++) {
            memcpy(p, &t->list[i].info, sizeof(uint32_t));
            p += sizeof(uint32_t);
        }
    }
}

void iop_heap_snapshot_load(const void *buf, uint32_t size) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t count = 0u, i, tidx;
    struct alloc_table *prev = NULL, *first = NULL, *t;
    (void)size;

    /* Deliberately do NOT walk/free() the incoming g_alloclist here.
     * In this function's primary real use (driver_r313.c's
     * load_checkpoint(), called AFTER the raw [__data_start,_end)
     * block has already been restored) g_alloclist at this point
     * already holds a STALE pointer value from whichever OTHER
     * process wrote the checkpoint - walking/free()'ing it would
     * dereference memory that was never valid in THIS process's
     * address space (this was the exact, confirmed root cause of
     * the "[R313-SIGSEGV] fault at addr=..." resume failures from
     * Rounds 307-448: an earlier version of this function tried to
     * free() that stale chain first and crashed immediately). It is
     * always safe to simply overwrite g_alloclist with a freshly
     * built chain below and accept leaking whatever it pointed to
     * before this call - this is host-process memory in a short-
     * lived, never-shipped test/checkpoint tool, not guest state,
     * so a one-time leak of a few hundred KB has no correctness
     * impact. (In the standalone-test call pattern - see
     * tests/test_iop_heap.c - the caller's own iop_heap_init() is
     * called immediately before this anyway, so the leaked chain is
     * always small and short-lived either way.) */

    memcpy(&count, p, sizeof(count));
    p += sizeof(count);

    for (tidx = 0u; tidx < count; tidx++) {
        t = new_table(); /* sets deterministic within-table next-chain */
        if (!t) break;
        for (i = 0u; i < (uint32_t)SM_MAX; i++) {
            memcpy(&t->list[i].info, p, sizeof(uint32_t));
            p += sizeof(uint32_t);
        }
        if (prev) {
            prev->next = t;
            prev->list[SM_LAST].next = &t->list[SM_FIRST]; /* cross-table stitch, matches do_maintain() */
        } else {
            first = t;
        }
        prev = t;
    }
    g_alloclist = first;
}
