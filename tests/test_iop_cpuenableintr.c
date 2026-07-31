/*
 * test_iop_cpuenableintr.c - host-native test for iop_core.c's
 * syscall 0x08 = real CpuEnableIntr() fix (task #217, 88th finding).
 *
 * Background: real ps2sdk source (iop/system/intrman/src/intrman.c,
 * fetched and cited this round) conclusively identifies IOP syscall
 * $v0=8 as `intrman_syscall_08_CpuEnableIntr` - the real mechanism
 * backing CpuEnableIntr(). Its real handler body ORs bits 2 (IEp)
 * and 10 (IM2) into the pre-exception Status value then writes it
 * back via MTC0, and the real return path is a genuine RFE, which
 * shifts IEp into IEc - net effect once CpuEnableIntr() returns:
 * Status.IEc=1 and Status.IM2=1. This project had previously (task
 * #164) treated $v0=8 as a pure no-op HLE bypass (a reasonable
 * inference at the time, made without a citable real target), which
 * this project's own diagnostic (STATUS.md's 87th finding) proved
 * left Status permanently 0x00000000 through an entire real BIOS
 * boot. This test verifies only the syscall's now-real, cited net
 * effect - not the full push/RFE round-trip real hardware performs
 * internally (deliberately still bypassed, same as before, since the
 * real target vector isn't reachable in this project's model - see
 * the fix's own comment in iop_core.c for the full citation trail).
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
static uint32_t enc_ori(uint32_t rt, uint32_t rs, uint32_t imm16) { return (0x0Du<<26) | (rs<<21) | (rt<<16) | (imm16 & 0xFFFFu); }
static uint32_t enc_syscall(void) { return (0x0Cu); } /* SPECIAL funct 0x0C, all other fields 0 */
static uint32_t enc_break(void)   { return 0x0Du; }

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

/* $v0=2, $s0=16 */

int main(void)
{
    /* --- Test 1: syscall with $v0=8 sets IEc+IM2, does NOT vector to
     * the exception handler, and execution resumes right after the
     * syscall (matching this project's existing "intercept before
     * any real exception" bypass convention for this specific,
     * now-understood syscall - same style already established for
     * 0x10/0x14 by task #164, just with a now-real Status effect). */
    {
        bios_image_t bios;
        memset(&bios, 0, sizeof(bios));
        bios.data = memalign(32, BIOS_MAX_SIZE);
        memset(bios.data, 0, BIOS_MAX_SIZE);
        bios.size = BIOS_MAX_SIZE;
        bios.loaded = 1;
        uint8_t *p = bios.data;

        wle32(p + 0x00, enc_lui(2, 0x0000));
        wle32(p + 0x04, enc_ori(2, 2, 0x0008)); /* $v0 = 8 (CpuEnableIntr) */
        wle32(p + 0x08, enc_syscall());
        wle32(p + 0x0C, enc_ori(16, 0, 0x4444)); /* marker: MUST execute (no vectoring) */
        wle32(p + 0x10, enc_break());            /* expected halt point */
        wle32(p + 0x180, enc_break());           /* must NOT be reached (would mean it vectored) */

        iop_core_init(&bios);
        iop_state_t *st = iop_core_get_state();
        st->cop0[12] = 0x00400000u; /* real reset value: BEV=1, everything else 0 (matches iop_core_init) */

        iop_core_run();

        CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
              "core halted on the BREAK right after the marker (not vectored to 0xBFC00180)");
        CHECK(st->cop0[13] == 0u,
              "Cause register untouched - confirms no real exception/vectoring occurred for $v0=8");
        CHECK(st->gpr[16] == 0x4444u,
              "marker instruction right after the syscall DID execute (real intercept-and-continue convention)");
        CHECK(st->gpr[2] == 0u,
              "$v0 (return value) == 0, same generic default-return convention as before this fix");
        CHECK((st->cop0[12] & 0x1u) != 0u,
              "Status.IEc (bit0) is now set - the real, cited net effect of CpuEnableIntr()");
        CHECK((st->cop0[12] & 0x400u) != 0u,
              "Status.IM2 (bit10) is now set - the real, cited net effect of CpuEnableIntr()");
        CHECK((st->cop0[12] & 0x400000u) != 0u,
              "Status.BEV (bit22) is untouched (still 1) - the fix only ORs in bits 0 and 10");
    }

    /* --- Test 2: any OTHER syscall number is NOT specially
     * intercepted by this fix - it still goes through this project's
     * real, pre-existing SYSCALL exception path (Cause.ExcCode=8,
     * EPC set, vectors to 0xBFC00180 per Status.BEV), and critically,
     * this fix does NOT spuriously touch Status.IEc/IM2 for any
     * syscall number other than exactly 8. (What happens AFTER
     * vectoring - a bare/empty test BIOS's default BREAK-as-syscall-
     * fallback resumption, tasks #149/#156 - is pre-existing,
     * already-established interpreter behavior this fix doesn't
     * touch, so this test doesn't assert on it either way.) */
    {
        bios_image_t bios;
        memset(&bios, 0, sizeof(bios));
        bios.data = memalign(32, BIOS_MAX_SIZE);
        memset(bios.data, 0, BIOS_MAX_SIZE);
        bios.size = BIOS_MAX_SIZE;
        bios.loaded = 1;
        uint8_t *p = bios.data;

        wle32(p + 0x00, enc_lui(2, 0x0000));
        wle32(p + 0x04, enc_ori(2, 2, 0x002A)); /* $v0 = 0x2A - not 0x08/0x10/0x14 */
        wle32(p + 0x08, enc_syscall());
        wle32(p + 0x0C, enc_break());            /* halt point if fallback resumes here */
        wle32(p + 0x180, enc_break());            /* bootstrap vector */

        iop_core_init(&bios);
        iop_state_t *st = iop_core_get_state();
        st->cop0[12] = 0x00400000u;

        iop_core_run();

        CHECK(st->halted == 1,
              "core reached a halt one way or another (either fallback-resumption or the vector BREAK)");
        CHECK((st->cop0[12] & 0x1u) == 0u,
              "Status.IEc untouched by an unrelated syscall number (still 0, no false-positive enable)");
        CHECK((st->cop0[12] & 0x400u) == 0u,
              "Status.IM2 untouched by an unrelated syscall number (still 0, no false-positive enable)");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
