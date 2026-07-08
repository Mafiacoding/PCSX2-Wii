/*
 * test_iop_hook_entry_int.c - host-native test for Round 29 continued's
 * 5th finding/fix: A(13h) setjmp(buf) and B(19h) HookEntryInt(addr).
 *
 * See include/core/hw/iop_hle_bios.h's IOP_HLE_A0_SETJMP and
 * IOP_HLE_B0_HOOK_ENTRY_INT comments, and docs/STATUS.md's "Round 29
 * continued (5th finding)" section, for the full citation trail: live
 * call-tracing against the user's real SCPH-10000 dump found the real
 * BIOS calls A(13h) setjmp(buf) immediately followed by B(19h)
 * HookEntryInt(addr) using the SAME address in both calls
 * (a0=0x8004fd50 in both, confirmed live), to install its own fallback
 * recovery point instead of leaving the kernel's default struct
 * (0x00006C34) installed. Before this fix, RAM[0x7520] was left
 * pointing at the default struct forever; after this fix, it correctly
 * ends up pointing at the caller's own buf.
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

static iop_state_t *fresh_iop(void)
{
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;
    iop_core_init(&bios);
    return iop_core_get_state();
}

int main(void) {
    iop_state_t *st = fresh_iop();
    iop_hle_bios_state_t *hle = iop_hle_bios_get_state();

    /* --- A(13h) setjmp(buf): saves real ra/sp/fp/s0-7/gp --- */
    {
        const uint32_t buf = 0x00008000u; /* arbitrary scratch RAM address */
        st->gpr[31] = 0x8003e1f4u; /* $ra */
        st->gpr[29] = 0x001ff320u; /* $sp */
        st->gpr[30] = 0x001fff40u; /* $fp/$s8 */
        for (int i = 0; i < 8; i++) st->gpr[16 + i] = 0x11110000u + (uint32_t)i; /* $s0..$s7 */
        st->gpr[28] = 0x0010a910u; /* $gp */

        uint32_t saved_ra = st->gpr[31]; /* setjmp's own return address is $ra at call time */
        st->gpr[4]  = buf;   /* $a0 = buf */
        st->gpr[9]  = IOP_HLE_A0_SETJMP;
        /* Keep $ra as the caller's real return address - setjmp must
         * jump back there after saving, exactly like every other
         * table-thunk call in this file. */
        int handled = iop_hle_bios_try_handle(st, IOP_HLE_TABLE_A0);
        CHECK(handled == 1, "A(13h) setjmp was recognized and handled");
        CHECK(iop_mem_read32(st, buf + IOP_JMPBUF_OFF_RA) == saved_ra,
              "setjmp saved $ra into buf[0]");
        CHECK(iop_mem_read32(st, buf + IOP_JMPBUF_OFF_SP) == 0x001ff320u,
              "setjmp saved $sp into buf[1]");
        CHECK(iop_mem_read32(st, buf + IOP_JMPBUF_OFF_FP) == 0x001fff40u,
              "setjmp saved $fp into buf[2]");
        int s_ok = 1;
        for (int i = 0; i < 8; i++) {
            uint32_t got = iop_mem_read32(st, buf + IOP_JMPBUF_OFF_S0 + (uint32_t)i * 4u);
            if (got != 0x11110000u + (uint32_t)i) s_ok = 0;
        }
        CHECK(s_ok, "setjmp saved $s0..$s7 into buf[3..10]");
        CHECK(iop_mem_read32(st, buf + IOP_JMPBUF_OFF_GP) == 0x0010a910u,
              "setjmp saved $gp into buf[11]");
        CHECK(st->gpr[2] == 0u, "setjmp returns 0 on the direct call, per C setjmp semantics");
        CHECK(st->pc == saved_ra, "control returned to $ra after setjmp");
        CHECK(hle->setjmp_calls == 1, "setjmp_calls incremented to 1");
    }

    /* --- B(19h) HookEntryInt(addr): installs a caller-chosen fallback --- */
    {
        const uint32_t addr = 0x00008000u; /* same buf as above, matching real usage */
        CHECK(iop_mem_read32(st, IOP_JMPBUF_DEFAULT_PTR_ADDR) == 0u,
              "RAM[0x7520] starts at 0 before HookEntryInt (fresh core)");
        st->gpr[4]  = addr;
        st->gpr[9]  = IOP_HLE_B0_HOOK_ENTRY_INT;
        st->gpr[31] = 0xBFC00060u;
        int handled = iop_hle_bios_try_handle(st, IOP_HLE_TABLE_B0);
        CHECK(handled == 1, "B(19h) HookEntryInt was recognized and handled");
        CHECK(iop_mem_read32(st, IOP_JMPBUF_DEFAULT_PTR_ADDR) == addr,
              "HookEntryInt wrote RAM[0x7520] = the caller's own addr, NOT the kernel default");
        CHECK(st->gpr[2] == addr,
              "HookEntryInt returns the installed address in $v0 (mirrors ResetEntryInt's convention)");
        CHECK(hle->hook_entry_int_calls == 1, "hook_entry_int_calls incremented to 1");
        CHECK(st->pc == 0xBFC00060u, "control returned to $ra after HookEntryInt");
    }

    /* --- Real usage sequence: setjmp(buf) then HookEntryInt(buf) with
     * the SAME address must leave RAM[0x7520] pointing at buf, not at
     * the kernel default struct - this is the exact real-BIOS pairing
     * this fix targets (see file header comment). --- */
    {
        iop_state_t *st2 = fresh_iop();
        iop_hle_bios_state_t *hle2 = iop_hle_bios_get_state();
        const uint32_t buf = 0x00009000u;

        st2->gpr[4]  = buf;
        st2->gpr[9]  = IOP_HLE_A0_SETJMP;
        st2->gpr[31] = 0x8003e1f4u;
        iop_hle_bios_try_handle(st2, IOP_HLE_TABLE_A0);

        st2->gpr[4]  = buf; /* same address, matching the real live-traced call pair */
        st2->gpr[9]  = IOP_HLE_B0_HOOK_ENTRY_INT;
        st2->gpr[31] = 0x8003e1f4u;
        iop_hle_bios_try_handle(st2, IOP_HLE_TABLE_B0);

        CHECK(iop_mem_read32(st2, IOP_JMPBUF_DEFAULT_PTR_ADDR) == buf,
              "setjmp+HookEntryInt pairing (same addr) leaves RAM[0x7520] pointing at the caller's buf");
        CHECK(iop_mem_read32(st2, IOP_JMPBUF_DEFAULT_PTR_ADDR) != IOP_JMPBUF_DEFAULT_STRUCT_ADDR,
              "RAM[0x7520] no longer points at the kernel default struct after this pairing");
        (void)hle2;
    }

    /* --- ResetEntryInt and HookEntryInt remain independent: calling
     * ResetEntryInt after HookEntryInt must still reset RAM[0x7520]
     * back to the kernel default, exactly as psx-spx documents. --- */
    {
        iop_state_t *st3 = fresh_iop();
        st3->gpr[4]  = 0x00009000u;
        st3->gpr[9]  = IOP_HLE_B0_HOOK_ENTRY_INT;
        st3->gpr[31] = 0xBFC00070u;
        iop_hle_bios_try_handle(st3, IOP_HLE_TABLE_B0);
        CHECK(iop_mem_read32(st3, IOP_JMPBUF_DEFAULT_PTR_ADDR) == 0x00009000u,
              "HookEntryInt installed the custom addr first");

        st3->gpr[9]  = IOP_HLE_B0_RESET_ENTRY_INT;
        st3->gpr[31] = 0xBFC00074u;
        iop_hle_bios_try_handle(st3, IOP_HLE_TABLE_B0);
        CHECK(iop_mem_read32(st3, IOP_JMPBUF_DEFAULT_PTR_ADDR) == IOP_JMPBUF_DEFAULT_STRUCT_ADDR,
              "ResetEntryInt still correctly resets RAM[0x7520] back to the kernel default afterwards");
    }

    if (failures) {
        printf("\n%d CHECK(S) FAILED\n", failures);
        return 1;
    }
    printf("\nAll checks passed.\n");
    return 0;
}
