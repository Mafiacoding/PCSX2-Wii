/*
 * test_iop_syscall_handler.c - host-native test for Round 29
 * continued's second fix: C(01h) EnqueueSyscallHandler(priority) and
 * B(18h) ResetEntryInt(), plus the real, hand-assembled MIPS
 * trampoline C(01h) installs.
 *
 * See include/core/hw/iop_hle_bios.h's IOP_HLE_C0_ENQUEUESYSCALLHANDLER
 * and IOP_HLE_B0_RESET_ENTRY_INT comments, and docs/STATUS.md's
 * "Round 29 continued" section, for the full citation trail: live
 * disassembly against the user's real SCPH-10000 dump found the real
 * BIOS calls these exact functions right after B(00h) succeeds, and
 * found the real dispatcher/ReturnFromException code this trampoline
 * is cross-checked against.
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

    /* --- B(18h) ResetEntryInt --- */
    {
        CHECK(iop_mem_read32(st, IOP_JMPBUF_DEFAULT_PTR_ADDR) == 0u,
              "RAM[0x7520] (jmp_buf pointer) starts at 0 before ResetEntryInt");
        st->gpr[9]  = IOP_HLE_B0_RESET_ENTRY_INT;
        st->gpr[31] = 0xBFC00050u;
        int handled = iop_hle_bios_try_handle(st, IOP_HLE_TABLE_B0);
        CHECK(handled == 1, "B(18h) ResetEntryInt was recognized and handled");
        CHECK(iop_mem_read32(st, IOP_JMPBUF_DEFAULT_PTR_ADDR) == IOP_JMPBUF_DEFAULT_STRUCT_ADDR,
              "ResetEntryInt correctly wrote RAM[0x7520] = IOP_JMPBUF_DEFAULT_STRUCT_ADDR (0x6C34)");
        CHECK(st->gpr[2] == IOP_JMPBUF_DEFAULT_STRUCT_ADDR,
              "ResetEntryInt returns the structure's address in $v0, per psx-spx");
        CHECK(hle->reset_entry_int_calls == 1, "reset_entry_int_calls incremented to 1");
        CHECK(st->pc == 0xBFC00050u, "control returned to $ra after ResetEntryInt");
    }

    /* --- C(01h) EnqueueSyscallHandler: installation + chain wiring ---
     * First, simulate the real B(00h) ExCB-table allocation that ALWAYS
     * precedes C(01h) in the real boot order (confirmed via live
     * tracing) - this claims IOP_EXCB_ARRAY_ADDR (0xE000-0xE01F) for
     * the chain-head array before any OTHER kmem_alloc() call can. */
    {
        st->gpr[9]  = IOP_HLE_B0_ALLOC_KERNEL_MEMORY;
        st->gpr[4]  = 0x20u;
        st->gpr[31] = 0xBFC00055u;
        iop_hle_bios_try_handle(st, IOP_HLE_TABLE_B0);
        CHECK(st->gpr[2] == IOP_EXCB_ARRAY_ADDR,
              "simulated B(00h) ExCB allocation claims IOP_EXCB_ARRAY_ADDR first, matching real boot order");

        CHECK(hle->syscall_handler_code_addr == 0,
              "syscall handler trampoline not yet installed before the first C(01h) call");

        st->gpr[9]  = IOP_HLE_C0_ENQUEUESYSCALLHANDLER;
        st->gpr[4]  = 0u; /* priority 0, matching real psx-spx usage */
        st->gpr[31] = 0xBFC00060u;
        int handled = iop_hle_bios_try_handle(st, IOP_HLE_TABLE_C0);
        CHECK(handled == 1, "C(01h) EnqueueSyscallHandler was recognized and handled");
        CHECK(hle->syscall_handler_code_addr != 0,
              "trampoline was installed into the real Kernel Memory bump allocator");
        CHECK(hle->syscall_handler_installs == 1, "syscall_handler_installs incremented to 1");
        CHECK(st->pc == 0xBFC00060u, "control returned to $ra after C(01h)");

        /* Sanity-check the first installed word is the expected real
         * "mfc0 $t0, $13" encoding (0x40086800) - the definitive,
         * byte-for-byte proof is the actual EXECUTION tests below,
         * which run these real bytes through the IOP interpreter and
         * check the resulting register/SR state. */
        CHECK(iop_mem_read32(st, hle->syscall_handler_code_addr) == 0x40086800u,
              "installed trampoline's first word is the real 'mfc0 $t0,$13' encoding");

        /* Priority-0 chain head must now be a real ExCB node whose
         * first-function field points at the installed trampoline. */
        uint32_t chain_head = iop_mem_read32(st, IOP_EXCB_ARRAY_ADDR + 0u * 8u);
        CHECK(chain_head != 0, "priority-0 chain head is non-null after C(01h)");
        CHECK(iop_mem_read32(st, chain_head + 0x08u) == hle->syscall_handler_code_addr,
              "the new chain node's first-function field points at the real trampoline");
        CHECK(iop_mem_read32(st, chain_head + 0x04u) == 0u,
              "the new chain node's second-function field is null (no second function)");

        /* A second C(01h) call (different priority) must reuse the SAME
         * trampoline code (installed once) but allocate a fresh node. */
        uint32_t code_addr_before = hle->syscall_handler_code_addr;
        st->gpr[9] = IOP_HLE_C0_ENQUEUESYSCALLHANDLER;
        st->gpr[4] = 3u;
        st->gpr[31] = 0xBFC00070u;
        iop_hle_bios_try_handle(st, IOP_HLE_TABLE_C0);
        CHECK(hle->syscall_handler_code_addr == code_addr_before,
              "second C(01h) call reuses the same trampoline code (installed only once)");
        CHECK(hle->syscall_handler_installs == 2, "syscall_handler_installs incremented to 2");
        uint32_t chain_head_p3 = iop_mem_read32(st, IOP_EXCB_ARRAY_ADDR + 3u * 8u);
        CHECK(chain_head_p3 != 0 && iop_mem_read32(st, chain_head_p3 + 0x08u) == code_addr_before,
              "priority-3 chain also got a real node pointing at the same trampoline");
    }

    /* --- Execute the real trampoline through the actual IOP
     * interpreter (not just check its bytes) - the strongest possible
     * verification that this is genuine, correct MIPS machine code,
     * not just plausible-looking bytes. --- */

    /* Case 1: EnterCriticalSection (saved original $a0 == 1). SR
     * starts with both bits 2 and 10 set; after running, both must be
     * cleared and $v0 must be 1 (psx-spx: "Returns 1 if both bits
     * were set"). Execution must end up at the real
     * ReturnFromException target (0xf30). */
    {
        iop_state_t *s2 = fresh_iop();
        iop_hle_bios_state_t *h2 = iop_hle_bios_get_state();
        s2->gpr[9] = IOP_HLE_B0_ALLOC_KERNEL_MEMORY; /* real boot order: ExCB first */
        s2->gpr[4] = 0x20u;
        s2->gpr[31] = 0xBFC00075u;
        iop_hle_bios_try_handle(s2, IOP_HLE_TABLE_B0);
        s2->gpr[9] = IOP_HLE_C0_ENQUEUESYSCALLHANDLER;
        s2->gpr[4] = 0u;
        s2->gpr[31] = 0xBFC00080u;
        iop_hle_bios_try_handle(s2, IOP_HLE_TABLE_C0);
        uint32_t code_addr = h2->syscall_handler_code_addr;

        /* Fake TCB context: PCB at 0x2000, current-TCB ptr -> 0x3000,
         * saved original $a0 at (TCB+8)+0x10. */
        iop_mem_write32(s2, 0x108u, 0x2000u);       /* RAM[0x108] = PCB addr */
        iop_mem_write32(s2, 0x2000u, 0x3000u);      /* PCB[0] = current TCB ptr */
        iop_mem_write32(s2, 0x3000u + 8u + 0x10u, 1u); /* saved a0 = 1 (EnterCriticalSection) */

        s2->cop0[13] = 0x20u;             /* Cause.ExcCode = Syscall(8) */
        s2->cop0[12] = 0x404u;            /* SR: bits 2 and 10 both set */
        s2->pc = code_addr;
        s2->next_pc = code_addr + 4u;

        int steps = 0;
        while (s2->pc != 0x00000f30u && steps < 64) {
            iop_core_step();
            steps++;
        }
        CHECK(s2->pc == 0x00000f30u, "EnterCriticalSection trampoline ends up at real ReturnFromException (0xf30)");
        CHECK((s2->cop0[12] & 0x404u) == 0u, "EnterCriticalSection correctly cleared SR bits 2 and 10");
        CHECK(s2->gpr[2] == 1u, "EnterCriticalSection returns 1 in $v0 (both bits were set beforehand)");
    }

    /* Case 2: ExitCriticalSection (saved original $a0 == 2). SR
     * starts with both bits clear; after running, both must be set. */
    {
        iop_state_t *s2 = fresh_iop();
        iop_hle_bios_state_t *h2 = iop_hle_bios_get_state();
        s2->gpr[9] = IOP_HLE_B0_ALLOC_KERNEL_MEMORY;
        s2->gpr[4] = 0x20u;
        s2->gpr[31] = 0xBFC00085u;
        iop_hle_bios_try_handle(s2, IOP_HLE_TABLE_B0);
        s2->gpr[9] = IOP_HLE_C0_ENQUEUESYSCALLHANDLER;
        s2->gpr[4] = 0u;
        s2->gpr[31] = 0xBFC00090u;
        iop_hle_bios_try_handle(s2, IOP_HLE_TABLE_C0);
        uint32_t code_addr = h2->syscall_handler_code_addr;

        iop_mem_write32(s2, 0x108u, 0x2000u);
        iop_mem_write32(s2, 0x2000u, 0x3000u);
        iop_mem_write32(s2, 0x3000u + 8u + 0x10u, 2u); /* saved a0 = 2 (ExitCriticalSection) */

        s2->cop0[13] = 0x20u;
        s2->cop0[12] = 0x00000000u; /* SR: bits 2 and 10 both clear */
        s2->pc = code_addr;
        s2->next_pc = code_addr + 4u;

        int steps = 0;
        while (s2->pc != 0x00000f30u && steps < 64) {
            iop_core_step();
            steps++;
        }
        CHECK(s2->pc == 0x00000f30u, "ExitCriticalSection trampoline ends up at real ReturnFromException (0xf30)");
        CHECK((s2->cop0[12] & 0x404u) == 0x404u, "ExitCriticalSection correctly set SR bits 2 and 10");
    }

    /* Case 3: not a syscall exception (Cause.ExcCode != 8) - the
     * handler must return 0 in $v0 via a plain jr $ra, matching the
     * real dispatcher's "try the next chain element" contract, and
     * must NOT touch SR at all. */
    {
        iop_state_t *s2 = fresh_iop();
        iop_hle_bios_state_t *h2 = iop_hle_bios_get_state();
        s2->gpr[9] = IOP_HLE_B0_ALLOC_KERNEL_MEMORY;
        s2->gpr[4] = 0x20u;
        s2->gpr[31] = 0xBFC00095u;
        iop_hle_bios_try_handle(s2, IOP_HLE_TABLE_B0);
        s2->gpr[9] = IOP_HLE_C0_ENQUEUESYSCALLHANDLER;
        s2->gpr[4] = 0u;
        s2->gpr[31] = 0xBFC000A0u;
        iop_hle_bios_try_handle(s2, IOP_HLE_TABLE_C0);
        uint32_t code_addr = h2->syscall_handler_code_addr;

        s2->cop0[13] = 0x00u; /* Cause.ExcCode = 0 (Interrupt, not Syscall) */
        s2->cop0[12] = 0x123u;
        s2->gpr[31]  = 0xBFC000B0u; /* $ra: where this handler should return to */
        s2->pc = code_addr;
        s2->next_pc = code_addr + 4u;

        int steps = 0;
        while (s2->pc != 0xBFC000B0u && steps < 16) {
            iop_core_step();
            steps++;
        }
        CHECK(s2->pc == 0xBFC000B0u, "non-syscall exception correctly returns via jr $ra, not RFE");
        CHECK(s2->gpr[2] == 0u, "non-syscall exception returns 0 in $v0 (let the next chain element try)");
        CHECK(s2->cop0[12] == 0x123u, "non-syscall exception leaves SR completely untouched");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
