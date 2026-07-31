/*
 * test_iop_kmem_alloc.c - host-native test for Round 29's real B(00h)
 * alloc_kernel_memory(size) bump allocator (source/hw/iop_hle_bios.c)
 * and iop_excb.c's companion fix: chain_head_addr() now reads the
 * chain-head array's base address dynamically from RAM[0x100]
 * (IOP_EXCB_TABLE_ADDR) instead of assuming it is always
 * IOP_EXCB_ARRAY_ADDR.
 *
 * Background (see docs/STATUS.md's "Round 29" section for the full
 * citation trail): live Capstone disassembly against the user's real
 * SCPH-10000 BIOS dump found that the real, genuinely-executing BIOS
 * ROM code that sets up ExCB/EvCB/PCB/TCB (confirmed at ROM addresses
 * ~0xbfc4ff90-0xbfc501f8) calls B(00h) alloc_kernel_memory via a
 * thunk-table tail call (0xbfc58c80-0xbfc58d80: `addiu $t1,zero,<fn>`
 * / `addiu $t2,zero,<0xB0>` / `jr $t2`, preserving the caller's own
 * $ra). Because this project previously had no real case for B0
 * function 0, every call fell through to the generic default
 * ($v0=0, "allocation failed"), so the real BIOS's own allocation
 * logic correctly bailed out without ever writing a valid pointer
 * into RAM[0x100]. This test verifies the real bump-allocator
 * behavior now implemented, per psx-spx's BIOS RAM Map: "0000E000h
 * 2000h Kernel Memory; ExCBs, EvCBs, and TCBs allocated via B(00h)".
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

static int call_b0_alloc(iop_state_t *st, uint32_t size, uint32_t ra)
{
    st->gpr[9]  = IOP_HLE_B0_ALLOC_KERNEL_MEMORY; /* $t1 = function 0x00 */
    st->gpr[4]  = size;                            /* $a0 = size */
    st->gpr[31] = ra;                              /* $ra */
    return iop_hle_bios_try_handle(st, IOP_HLE_TABLE_B0);
}

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    iop_core_init(&bios);
    iop_state_t *st = iop_core_get_state();
    iop_hle_bios_state_t *hle = iop_hle_bios_get_state();

    /* --- Bump pointer starts at the documented Kernel Memory base --- */
    CHECK(hle->kmem_bump_next == IOP_EXCB_ARRAY_ADDR,
          "kmem_bump_next initialized to IOP_EXCB_ARRAY_ADDR (0xE000)");
    CHECK(hle->kmem_alloc_calls == 0, "kmem_alloc_calls starts at 0");
    CHECK(hle->kmem_alloc_failures == 0, "kmem_alloc_failures starts at 0");

    /* --- First real allocation: 0x20 bytes (matches the real BIOS's
     * own Table-of-Tables-sized first request in the live trace) --- */
    int handled = call_b0_alloc(st, 0x20u, 0xBFC00100u);
    CHECK(handled == 1, "B(00h) call was recognized and handled");
    CHECK(st->gpr[2] == IOP_EXCB_ARRAY_ADDR,
          "first alloc(0x20) returns the Kernel Memory region base in $v0");
    CHECK(hle->kmem_bump_next == IOP_EXCB_ARRAY_ADDR + 0x20u,
          "kmem_bump_next advanced by exactly the requested (already-aligned) size");
    CHECK(hle->kmem_alloc_calls == 1, "kmem_alloc_calls incremented to 1");
    CHECK(st->pc == 0xBFC00100u, "control returned to $ra after the real B(00h) call");

    /* --- Second allocation: unaligned size (5) rounds up to 4-byte
     * alignment (8), matching this project's own word-based struct
     * layout convention used everywhere else in this file --- */
    uint32_t expected_second = hle->kmem_bump_next;
    handled = call_b0_alloc(st, 5u, 0xBFC00200u);
    CHECK(handled == 1, "second B(00h) call was recognized and handled");
    CHECK(st->gpr[2] == expected_second,
          "second alloc returns the next bump address (0x20 bytes after the first)");
    CHECK(hle->kmem_bump_next == expected_second + 8u,
          "unaligned size 5 was rounded up to 8 (4-byte alignment)");
    CHECK(hle->kmem_alloc_calls == 2, "kmem_alloc_calls incremented to 2");

    /* --- Allocation that would overflow the documented 0x2000-byte
     * region fails cleanly (returns 0), it does not wrap or corrupt
     * unrelated memory --- */
    handled = call_b0_alloc(st, 0x2000u, 0xBFC00300u);
    CHECK(handled == 1, "oversized B(00h) call was still recognized (real hardware always returns)");
    CHECK(st->gpr[2] == 0u, "oversized alloc correctly fails with $v0=0 (real malloc-style failure)");
    CHECK(hle->kmem_alloc_failures == 1, "kmem_alloc_failures incremented to 1");
    CHECK(hle->kmem_alloc_calls == 3, "kmem_alloc_calls still counts failed calls (3 total)");

    /* --- Round 29 companion fix: chain_head_addr() now reads the
     * CURRENT RAM[0x100] value dynamically instead of hardcoding
     * IOP_EXCB_ARRAY_ADDR - simulate the real BIOS having allocated
     * the ExCB array somewhere OTHER than IOP_EXCB_ARRAY_ADDR (e.g.
     * because some earlier B(00h) call already consumed part of the
     * Kernel Memory region), and verify SysEnqIntRP correctly follows
     * that dynamic pointer rather than the old hardcoded constant. */
    {
        uint32_t moved_table = IOP_EXCB_ARRAY_ADDR + 0x40u; /* pretend allocator gave us this */
        iop_mem_write32(st, IOP_EXCB_TABLE_ADDR, moved_table);

        uint32_t node = 0x00021000u;
        iop_mem_write32(st, node + 0x08u, 0x44440000u);
        iop_excb_sys_enq_int_rp(st, 2, node); /* priority 2 */

        CHECK(iop_mem_read32(st, moved_table + 2 * 8u) == node,
              "SysEnqIntRP correctly used the DYNAMIC RAM[0x100] table address, not the hardcoded constant");
        CHECK(iop_mem_read32(st, IOP_EXCB_ARRAY_ADDR + 2 * 8u) == 0u,
              "the old hardcoded IOP_EXCB_ARRAY_ADDR location was correctly left untouched");

        iop_excb_sys_deq_int_rp(st, 2, node);
        CHECK(iop_mem_read32(st, moved_table + 2 * 8u) == 0u,
              "SysDeqIntRP also correctly followed the dynamic table address");
    }

    /* --- If RAM[0x100] is still 0 (nothing has allocated it yet),
     * chain_head_addr() falls back to IOP_EXCB_ARRAY_ADDR, preserving
     * every pre-Round-29 test's assumptions --- */
    {
        iop_mem_write32(st, IOP_EXCB_TABLE_ADDR, 0u);
        uint32_t node = 0x00022000u;
        iop_excb_sys_enq_int_rp(st, 3, node);
        CHECK(iop_mem_read32(st, IOP_EXCB_ARRAY_ADDR + 3 * 8u) == node,
              "RAM[0x100]==0 falls back to IOP_EXCB_ARRAY_ADDR (backward-compatible default)");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
