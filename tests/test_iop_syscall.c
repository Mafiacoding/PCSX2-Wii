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
     * the bootstrap vector (0xBFC00180, BIOS offset 0x180). */
    wle32(p + 0x180, enc_break()); /* bootstrap vector: 0xBFC00180 */

    iop_core_init(&bios);
    iop_state_t *st = iop_core_get_state();

    CHECK((st->cop0[12] & 0x400000u) != 0u, "IOP reset correctly sets Status.BEV=1 (matches real hardware/PCSX2's psxReset())");

    iop_core_run();

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core reached and halted on BREAK at the bootstrap exception vector (0xBFC00180)");
    CHECK((st->cop0[13] & 0x7Fu) == 0x20u, "Cause.ExcCode was set to 8 (Syscall), pre-shifted into bits 2-6 as 0x20");
    CHECK(st->cop0[14] == 0xBFC00000u, "EPC was set to the SYSCALL instruction's own address");
    /* NOTE: BREAK's own halt() path returns before the trailing
     * instructions_executed++ (same as every other halt() call in
     * this codebase - a pre-existing, consistent convention, not
     * something new here), so only the SYSCALL step itself gets
     * counted. The important thing this checks is that it's exactly
     * 1, not 2 - if SYSCALL incorrectly had a branch delay slot, the
     * NOP at BIOS offset 4 would have executed too and this would
     * read 2 instead. */
    CHECK(st->instructions_executed == 1,
          "exactly 1 instruction counted (SYSCALL) before halting on BREAK at the vector - the NOP after SYSCALL was correctly skipped (no delay slot)");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
