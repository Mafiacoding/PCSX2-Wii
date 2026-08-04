/* Round 401 (task #128): host-native test for the real SYSMEM
 * free-list heap allocator port (source/hw/iop_heap.c). */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "core/hw/iop_heap.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: %s\n", msg); } \
} while (0)

int main(void) {
    iop_heap_init();

    /* total free space should be non-zero and equal to max free
     * space right after init (single big free block). */
    uint32_t total0 = iop_heap_query_total_free();
    uint32_t max0 = iop_heap_query_max_free();
    CHECK(total0 > 0u, "total free > 0 after init");
    CHECK(total0 == max0, "total free == max free with single free block");

    /* basic alloc: address should be non-zero (real success), and
     * 256-byte aligned (real hardware's own allocation unit). */
    uint32_t a1 = iop_heap_alloc(IOP_HEAP_ALLOC_FIRST, 100u, 0u);
    CHECK(a1 != 0u, "first alloc succeeds");
    CHECK((a1 & 0xFFu) == 0u, "first alloc is 256-byte aligned");

    /* total free should have shrunk by the rounded-up block size
     * (100 bytes rounds up to one 256-byte unit). */
    uint32_t total1 = iop_heap_query_total_free();
    CHECK(total1 == total0 - 256u, "total free shrinks by rounded block size");

    /* second alloc should land immediately after the first (real
     * ALLOC_FIRST first-fit carving behavior). */
    uint32_t a2 = iop_heap_alloc(IOP_HEAP_ALLOC_FIRST, 512u, 0u);
    CHECK(a2 == a1 + 256u, "second alloc lands right after first (first-fit carve)");

    /* query block size on an allocated block reports USED. */
    int32_t bs1 = iop_heap_query_block_size(a1);
    CHECK(bs1 >= 0, "query_block_size finds allocated block 1");
    CHECK(((uint32_t)bs1 & 0x80000000u) == IOP_HEAP_USED, "block 1 reports USED");
    CHECK(((uint32_t)bs1 & ~0x80000000u) == 256u, "block 1 reports 256-byte real size");

    /* free block 1, then re-alloc the same size - should reuse the
     * freed block (real first-fit will find it again, now free). */
    int fr = iop_heap_free(a1);
    CHECK(fr == 0u, "free succeeds");
    /* NOTE: real QueryBlockSize's FREE flag (0x80000000) makes any
     * found FREE block's return value negative when read as int32_t
     * (same bit as the sign bit) - only real -1 (0xFFFFFFFF) means
     * "not found", so the correct "found" test is `!= -1`, not
     * `>= 0` (a `>= 0` check would incorrectly treat every found FREE
     * block as a miss, since found-but-FREE values are always
     * negative too). */
    int32_t bs1f = iop_heap_query_block_size(a1);
    CHECK(bs1f != -1 && ((uint32_t)bs1f & 0x80000000u) == IOP_HEAP_FREE, "block 1 reports FREE after free");

    /* a2 (512 bytes) is still allocated at this point, so total free
     * is total0 minus a2's rounded size only - a1's own 256 bytes net
     * to zero (subtracted on alloc, added back on free). */
    uint32_t total2 = iop_heap_query_total_free();
    CHECK(total2 == total0 - 512u, "total free reflects a1 freed / a2 still allocated");

    uint32_t a3 = iop_heap_alloc(IOP_HEAP_ALLOC_FIRST, 200u, 0u);
    CHECK(a3 == a1, "re-alloc reuses the freed block (first-fit)");

    /* genuine double-free: free a3 once (succeeds), then free it
     * again (must report -1, real "cannot free a freed block"). */
    int fr2 = iop_heap_free(a3);
    CHECK(fr2 == 0, "first free of a3 succeeds");
    int fr3 = iop_heap_free(a3);
    CHECK(fr3 == -1, "genuine double-free reports -1");

    /* freeing a non-256-aligned address fails. */
    int fr4 = iop_heap_free(a2 + 1u);
    CHECK(fr4 == -1, "freeing a non-256-aligned address fails");

    /* freeing an address that was never allocated fails. */
    int fr5 = iop_heap_free(0x000FFF00u);
    CHECK(fr5 == -1, "freeing a never-allocated address fails");

    /* coalescing: free a2 (still allocated), then verify max free
     * grows to include the now-contiguous freed region (a1/a3's slot
     * + a2's slot, both free and adjacent). */
    iop_heap_free(a2);
    uint32_t maxN = iop_heap_query_max_free();
    CHECK(maxN >= 512u, "coalesced free region reports a combined max free size");

    /* exhaustion: request something larger than the whole arena -
     * must genuinely fail (return 0), not fabricate an address. */
    uint32_t huge = iop_heap_alloc(IOP_HEAP_ALLOC_FIRST, 0x00200000u, 0u);
    CHECK(huge == 0u, "over-large request genuinely fails (returns 0)");

    /* re-init resets to a clean single free block. */
    iop_heap_init();
    uint32_t total_reset = iop_heap_query_total_free();
    CHECK(total_reset == total0, "re-init restores original total free size");

    /* Round 448 (task #247): snapshot save/load round-trip, added
     * to fix the host-native checkpoint/resume test harness's
     * long-standing SIGSEGV-on-resume bug (root-caused to this
     * file's g_alloclist being the ONLY host-heap-allocated state
     * anywhere under source/ - see iop_heap.h's citation on the new
     * functions). Force enough allocations to guarantee at least one
     * table growth, so the cross-table next-pointer stitching path
     * (do_maintain()'s &next_table->list[SM_FIRST] wiring) is
     * actually exercised, not just the single-table case. */
    {
        uint32_t snap_addrs[40];
        int snap_n = 0, i;
        iop_heap_init();
        for (i = 0; i < 35; i++) {
            uint32_t a = iop_heap_alloc(IOP_HEAP_ALLOC_FIRST, 512u, 0u);
            if (a != 0u) snap_addrs[snap_n++] = a;
        }
        CHECK(snap_n == 35, "snapshot test: all 35 pre-growth allocations succeed");
        for (i = 0; i < snap_n; i += 3) {
            iop_heap_free(snap_addrs[i]);
        }

        uint32_t maxfree_before = iop_heap_query_max_free();
        uint32_t totalfree_before = iop_heap_query_total_free();

        uint32_t snap_sz = iop_heap_snapshot_size();
        CHECK(snap_sz > 0u, "snapshot size is non-zero after growth+mixed alloc/free");
        void *snap_buf = malloc(snap_sz);
        CHECK(snap_buf != NULL, "snapshot buffer allocation succeeds");
        iop_heap_snapshot_save(snap_buf);

        /* Simulate a fresh resuming process: re-init (as
         * system_init() would before load_checkpoint() runs), THEN
         * load the snapshot - the exact call order driver_r313.c
         * uses. */
        iop_heap_init();
        iop_heap_snapshot_load(snap_buf, snap_sz);

        uint32_t maxfree_after = iop_heap_query_max_free();
        uint32_t totalfree_after = iop_heap_query_total_free();
        CHECK(maxfree_after == maxfree_before, "snapshot round-trip preserves max free size");
        CHECK(totalfree_after == totalfree_before, "snapshot round-trip preserves total free size");

        int all_blocks_ok = 1;
        for (i = 0; i < snap_n; i++) {
            int32_t q = iop_heap_query_block_size(snap_addrs[i]);
            int should_be_free = (i % 3 == 0);
            if (q == -1) { all_blocks_ok = 0; break; }
            if (should_be_free && !((uint32_t)q & IOP_HEAP_FREE)) { all_blocks_ok = 0; break; }
            if (!should_be_free && ((uint32_t)q & IOP_HEAP_FREE)) { all_blocks_ok = 0; break; }
        }
        CHECK(all_blocks_ok, "every block's allocated/free state survives the snapshot round-trip");

        /* Restored chain must still be FUNCTIONAL (proves the
         * reconstructed next-pointer links work, not just the info
         * bitfields matching by coincidence). */
        uint32_t post_a = iop_heap_alloc(IOP_HEAP_ALLOC_FIRST, 256u, 0u);
        CHECK(post_a != 0u, "restored heap can still allocate after snapshot round-trip");
        int post_fr = iop_heap_free(post_a);
        CHECK(post_fr == 0, "restored heap can still free after snapshot round-trip");

        free(snap_buf);
    }

    /* final re-init leaves the module in a clean state for any
     * process-wide reuse. */
    iop_heap_init();

    printf("\n%d check(s) failed, %d passed\n", g_fail, g_pass);
    return g_fail != 0;
}
