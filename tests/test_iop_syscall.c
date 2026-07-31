/*
 * test_iop_syscall.c - host-native test for iop_core.c's SYSCALL
 * exception handling (ported from PCSX2's psxException() in
 * R3000A.cpp - see the SYSCALL case in iop_core.c for full reference
 * notes and the one documented simplification: branch-delay-slot
 * detection isn't modeled).
 *
 * This was added after real BIOS testing (SCPH-10000, see
 * docs/STATUS.md) showed the IOP halting on a raw SYSCALL instruction
 * after 3+ million real instructions - previously SYSCALL just
 * halted unconditionally rather than raising a real exception.
 *
 * UPDATED (Round 29 continued, 29th change - see docs/STATUS.md's
 * 29th finding, task #149): real BIOS testing found that a genuine
 * R3000A `syscall` reaching a still-unclaimed general exception
 * vector (nothing has installed a real handler there yet - the same
 * underlying architectural gap as task #124/#132/#148) eventually
 * runs into a BREAK placeholder. This project now treats that
 * specific case (BREAK reached with Cause.ExcCode==8, Syscall, still
 * pending) the same way every other unimplemented BIOS call in this
 * project already defaults: return 0 to the caller instead of
 * halting - see iop_core.c's BREAK case for the full rationale. This
 * test's OWN scenario (a bare SYSCALL immediately followed by a BREAK
 * at the vector, with nothing else going on) is now, by construction,
 * indistinguishable from that exact real scenario - so this test was
 * split into two phases: first, single-step just past the SYSCALL to
 * verify the original vectoring assertions (Cause/EPC/PC) still hold
 * exactly as before; then continue running to verify the NEW
 * auto-return behavior fires correctly (core does NOT halt, $v0
 * becomes 0, pc resumes right after the SYSCALL) rather than
 * asserting a clean halt at the vector, which is no longer this
 * scenario's real outcome.
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

static uint32_t enc_syscall(void) { return 0x0C; } /* SPECIAL, funct 0x0C, all other fields 0 */
static uint32_t enc_break(void)   { return 0x0D; }
static uint32_t enc_nop(void)     { return 0x00000000u; }

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    uint8_t *p = bios.data;
    /* At the reset vector (0xBFC00000, offset 0): a SYSCALL, then a
     * NOP that must NOT execute (SYSCALL has no branch delay slot -
     * it's an immediate exception, unlike ordinary jumps). */
    wle32(p + 0, enc_syscall());
    wle32(p + 4, enc_nop());

    /* Place a BREAK at both possible exception vectors so the test
     * doesn't need to guess which one Status.BEV selects - after
     * init, real hardware/PCSX2 sets BEV=1, so this should land on
     * the bootstrap vector (0xBFC00180, BIOS offset 0x180). This now
     * models a genuinely still-unclaimed vector (see the file header
     * comment above), not a "handler installed" scenario. */
    wle32(p + 0x180, enc_break()); /* bootstrap vector: 0xBFC00180 */

    iop_core_init(&bios);
    iop_state_t *st = iop_core_get_state();

    CHECK((st->cop0[12] & 0x400000u) != 0u, "IOP reset correctly sets Status.BEV=1 (matches real hardware/PCSX2's psxReset())");

    /* Phase 1: single-step exactly the SYSCALL instruction, then
     * check the real exception-vectoring state before the still-
     * unclaimed vector's own BREAK ever runs. */
    iop_core_step();

    CHECK(st->halted == 0, "core has NOT halted after just the SYSCALL step itself");
    CHECK((st->cop0[13] & 0x7Fu) == 0x20u, "Cause.ExcCode was set to 8 (Syscall), pre-shifted into bits 2-6 as 0x20");
    CHECK(st->cop0[14] == 0xBFC00000u, "EPC was set to the SYSCALL instruction's own address");
    CHECK(st->pc == 0xBFC00180u, "pc vectored to the bootstrap exception vector (Status.BEV was set)");
    CHECK(st->instructions_executed == 1,
          "exactly 1 instruction counted (SYSCALL) - the NOP after SYSCALL was correctly skipped (no delay slot, no branch taken there)");

    /* Phase 2: single-step exactly the still-unclaimed vector's own
     * BREAK instruction (NOT iop_core_run() - after the new auto-
     * return below, pc resumes into a region of all-zero/NOP BIOS
     * bytes with no other halt condition, which would spin forever).
     * Per the new precedent (see the file header comment above), this
     * single BREAK step must NOT halt - it returns $v0=0 to the
     * caller, resuming right after the original SYSCALL, exactly like
     * every other unimplemented BIOS call in this project already
     * defaults. */
    iop_core_step();

    CHECK(st->halted == 0,
          "core did NOT halt on the still-unclaimed vector's BREAK - it was recognized as an unresolved Syscall (Cause.ExcCode==8) and auto-returned instead");
    CHECK(st->gpr[2] == 0u, "$v0 (gpr[2]) was set to 0 - the same default-return convention as this project's unimplemented A0/B0/C0 BIOS calls");
    CHECK(st->pc == 0xBFC00004u,
          "pc resumed right after the original SYSCALL (EPC+4 = 0xBFC00000+4), real RFE-equivalent return semantics");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
