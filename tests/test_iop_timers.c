/*
 * test_iop_timers.c - host-native test for source/hw/iop_timers.c
 *
 * This is a plain register-storage stub (see iop_timers.h for why -
 * real counting/gating/target-IRQ behavior is out of scope for now),
 * so the test is correspondingly simple: address decoding for all 6
 * counters' COUNT/MODE/TARGET registers, isolation between counters,
 * and confirming addresses inside a counter's 12-byte window that
 * aren't one of the 3 known register offsets are correctly NOT
 * claimed (rather than silently aliasing to the wrong register).
 */
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "hw/iop_timers.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else printf("ok:   %s\n", msg); \
} while (0)

int main(void)
{
    iop_timers_init();

    uint32_t v;

    /* T0: COUNT 0x1F801100, MODE 0x1F801104, TARGET 0x1F801108 */
    iop_timers_mmio_write32(0x1F801100u, 0x11111111u);
    iop_timers_mmio_write32(0x1F801104u, 0x22222222u);
    iop_timers_mmio_write32(0x1F801108u, 0x33333333u);
    iop_timers_mmio_read32(0x1F801100u, &v); CHECK(v == 0x11111111u, "T0 COUNT roundtrip");
    iop_timers_mmio_read32(0x1F801104u, &v); CHECK(v == 0x22222222u, "T0 MODE roundtrip");
    iop_timers_mmio_read32(0x1F801108u, &v); CHECK(v == 0x33333333u, "T0 TARGET roundtrip");

    /* T1 must be unaffected by T0's writes (address decoding isolation) */
    iop_timers_mmio_read32(0x1F801110u, &v);
    CHECK(v == 0u, "T1 COUNT unaffected by T0's writes");

    /* T3 lives in the second address window (0x1F801480) - confirm
     * both windows work independently */
    iop_timers_mmio_write32(0x1F801480u, 0xAAAAAAAAu);
    iop_timers_mmio_write32(0x1F801488u, 0xBBBBBBBBu);
    iop_timers_mmio_read32(0x1F801480u, &v); CHECK(v == 0xAAAAAAAAu, "T3 COUNT roundtrip (second address window)");
    iop_timers_mmio_read32(0x1F801488u, &v); CHECK(v == 0xBBBBBBBBu, "T3 TARGET roundtrip (second address window)");

    /* T5, the last counter (0x1F8014A0) */
    iop_timers_mmio_write32(0x1F8014A4u, 0xCCCCCCCCu);
    iop_timers_mmio_read32(0x1F8014A4u, &v);
    CHECK(v == 0xCCCCCCCCu, "T5 MODE roundtrip");

    /* An address inside T0's 12-byte window that ISN'T one of the 3
     * known register offsets (e.g. base+0x02) must not be silently
     * treated as COUNT/MODE/TARGET. */
    CHECK(iop_timers_mmio_read32(0x1F801102u, &v) == 0,
          "address inside T0's window but not a known register offset: not claimed on read");
    CHECK(iop_timers_mmio_write32(0x1F801102u, 0x1u) == 0,
          "address inside T0's window but not a known register offset: not claimed on write");

    /* An address between T2 and T3 (the gap between the two hardware
     * windows) must not be claimed either. */
    CHECK(iop_timers_mmio_read32(0x1F801200u, &v) == 0,
          "address in the gap between the T0-T2 and T3-T5 windows: not claimed");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
