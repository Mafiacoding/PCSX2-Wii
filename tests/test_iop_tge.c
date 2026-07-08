/*
 * test_iop_tge.c - host-native test for iop_core.c's TGE (Trap if
 * Greater or Equal, SPECIAL funct 0x30) implementation, added in
 * Round 29 continued's task #150.
 *
 * Background (see docs/STATUS.md's 29th finding, task #149): after
 * fixing the BREAK@0x00000018 halt (an unresolved real syscall
 * falling through to the still-unclaimed general exception vector),
 * a SECOND real syscall deeper in INTRMANP's init falls through that
 * same still-unclaimed vector down a different path and reaches a
 * genuine TGE instruction at pc=0x800000A8, which this interpreter
 * previously halted on as "unimplemented SPECIAL funct 0x30".
 *
 * Real MIPS trap semantics: if the signed comparison rs >= rt holds,
 * a Trap exception is raised (Cause.ExcCode=13 "Tr", pre-shifted into
 * bits 2-6 as 0x34); EPC is set to the TGE instruction's own address;
 * PC vectors to 0xBFC00180 (bootstrap) or 0x80000080 (normal)
 * depending on Status.BEV, exactly like this file's existing SYSCALL
 * exception delivery. If the condition does NOT hold, TGE is a pure
 * no-op - execution just falls through to the next instruction, with
 * no exception, no delay slot, and no other side effect.
 *
 * This test covers both outcomes: phase 1 verifies the trap-taken
 * path (5 >= 3) delivers the exception exactly like SYSCALL does,
 * just with ExcCode=13 instead of 8; phase 2 verifies the trap-NOT-
 * taken path (3 >= 5 is false) behaves as a true no-op by single-
 * stepping past it and confirming a following instruction executed
 * normally with no exception state touched.
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

static uint32_t enc_lui(uint32_t rt, uint32_t imm16)             { return (0x0Fu<<26) | (rt<<16) | (imm16 & 0xFFFFu); }
static uint32_t enc_ori(uint32_t rt, uint32_t rs, uint32_t imm16){ return (0x0Du<<26) | (rs<<21) | (rt<<16) | (imm16 & 0xFFFFu); }
static uint32_t enc_tge(uint32_t rs, uint32_t rt)                { return (rs<<21) | (rt<<16) | 0x30u; }
static uint32_t enc_break(void)                                  { return 0x0Du; }

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

static void build_bios(bios_image_t *bios) {
    memset(bios, 0, sizeof(*bios));
    bios->data = memalign(32, BIOS_MAX_SIZE);
    memset(bios->data, 0, BIOS_MAX_SIZE);
    bios->size = BIOS_MAX_SIZE;
    bios->loaded = 1;
}

int main(void) {
    const uint32_t T0 = 8, T1 = 9, T2 = 10;

    /* ---- Phase 1: trap-taken path (5 >= 3) ---- */
    {
        bios_image_t bios;
        build_bios(&bios);
        uint8_t *p = bios.data;

        /* reset vector 0xBFC00000: t0=5, t1=3, TGE t0,t1 */
        wle32(p + 0x000, enc_lui(T0, 0));
        wle32(p + 0x004, enc_ori(T0, T0, 5));
        wle32(p + 0x008, enc_lui(T1, 0));
        wle32(p + 0x00C, enc_ori(T1, T1, 3));
        wle32(p + 0x010, enc_tge(T0, T1));

        iop_core_init(&bios);
        iop_state_t *st = iop_core_get_state();

        CHECK((st->cop0[12] & 0x400000u) != 0u, "reset sets Status.BEV=1");

        iop_core_step(); /* lui t0 */
        iop_core_step(); /* ori t0,t0,5 */
        iop_core_step(); /* lui t1 */
        iop_core_step(); /* ori t1,t1,3 */
        CHECK(st->gpr[T0] == 5u && st->gpr[T1] == 3u, "t0=5, t1=3 set up correctly before TGE");

        iop_core_step(); /* TGE t0,t1 : 5 >= 3 -> trap taken */

        CHECK(st->halted == 0, "core did NOT halt on trap-taken TGE (it raises a real exception, not a halt)");
        CHECK((st->cop0[13] & 0x7Fu) == 0x34u, "Cause.ExcCode set to 13 (Trap), pre-shifted into bits 2-6 as 0x34");
        CHECK(st->cop0[14] == 0xBFC00010u, "EPC set to the TGE instruction's own address");
        CHECK(st->pc == 0xBFC00180u, "pc vectored to the bootstrap exception vector (Status.BEV was set)");
    }

    /* ---- Phase 2: trap-NOT-taken path (3 >= 5 is false) - pure no-op ---- */
    {
        bios_image_t bios;
        build_bios(&bios);
        uint8_t *p = bios.data;

        /* reset vector 0xBFC00000: t0=3, t1=5, TGE t0,t1 (not taken), then t2=1 marker */
        wle32(p + 0x000, enc_lui(T0, 0));
        wle32(p + 0x004, enc_ori(T0, T0, 3));
        wle32(p + 0x008, enc_lui(T1, 0));
        wle32(p + 0x00C, enc_ori(T1, T1, 5));
        wle32(p + 0x010, enc_tge(T0, T1));
        wle32(p + 0x014, enc_ori(T2, 0, 1)); /* marker: t2 = 1 if we reach here normally */
        wle32(p + 0x018, enc_break());

        iop_core_init(&bios);
        iop_state_t *st = iop_core_get_state();

        iop_core_step(); /* lui t0 */
        iop_core_step(); /* ori t0,t0,3 */
        iop_core_step(); /* lui t1 */
        iop_core_step(); /* ori t1,t1,5 */
        CHECK(st->gpr[T0] == 3u && st->gpr[T1] == 5u, "t0=3, t1=5 set up correctly before TGE");

        uint32_t cause_before = st->cop0[13];
        uint32_t epc_before = st->cop0[14];

        iop_core_step(); /* TGE t0,t1 : 3 >= 5 is false -> pure no-op */

        CHECK(st->halted == 0, "core did NOT halt on trap-NOT-taken TGE");
        CHECK(st->pc == 0xBFC00014u, "pc fell through to the very next instruction - no exception, no delay slot");
        CHECK(st->cop0[13] == cause_before, "Cause register completely untouched by a not-taken TGE");
        CHECK(st->cop0[14] == epc_before, "EPC register completely untouched by a not-taken TGE");

        iop_core_step(); /* ori t2,zero,1 */
        CHECK(st->gpr[T2] == 1u, "execution continued normally past the not-taken TGE (marker instruction ran)");

        iop_core_run();
        CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
              "core reached and cleanly halted on the trailing BREAK, confirming no stray trap fired");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
