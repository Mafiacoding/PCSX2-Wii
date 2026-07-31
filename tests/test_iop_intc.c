/*
 * test_iop_intc.c - host-native test for source/hw/iop_intc.c
 *
 * Verifies register semantics ported from real PCSX2
 * (ps2/Iop/IopHwWrite.cpp, IopHwRead.cpp - the HW_ISTAT/HW_IMASK/
 * HW_ICTRL cases): I_STAT's write-0-to-clear (AND) polarity, I_MASK's
 * plain read/write, I_CTRL's read-clears-the-register one-shot-latch
 * behavior, and iop_intc_raise() setting a bit as a peripheral would.
 */
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "hw/iop_intc.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else printf("ok:   %s\n", msg); \
} while (0)

int main(void)
{
    iop_intc_init();

    uint32_t v;

    /* Everything starts at zero */
    CHECK(iop_intc_mmio_read32(0x1F801070u, &v) == 1 && v == 0u, "I_STAT starts at 0");
    CHECK(iop_intc_mmio_read32(0x1F801074u, &v) == 1 && v == 0u, "I_MASK starts at 0");
    CHECK(iop_intc_mmio_read32(0x1F801078u, &v) == 1 && v == 0u, "I_CTRL starts at 0");

    /* iop_intc_raise() sets a bit, as a peripheral would */
    iop_intc_raise(2);
    iop_intc_raise(5);
    iop_intc_mmio_read32(0x1F801070u, &v);
    CHECK(v == ((1u << 2) | (1u << 5)), "iop_intc_raise() ORs the requested bits into I_STAT");

    /* I_STAT write ANDs (write 0 to clear bit 2, leave bit 5 alone:
     * write value has bit 2 = 0, all other bits = 1) */
    iop_intc_mmio_write32(0x1F801070u, ~(1u << 2));
    iop_intc_mmio_read32(0x1F801070u, &v);
    CHECK(v == (1u << 5), "I_STAT write ANDs in the value (write-0-to-clear, bit 2 cleared, bit 5 untouched)");

    /* Writing all-1s to I_STAT should clear nothing (AND with all 1s
     * is a no-op) - confirms the polarity is really AND, not OR or a
     * plain overwrite. */
    iop_intc_mmio_write32(0x1F801070u, 0xFFFFFFFFu);
    iop_intc_mmio_read32(0x1F801070u, &v);
    CHECK(v == (1u << 5), "I_STAT write of all-1s clears nothing (confirms AND polarity, not overwrite/OR)");

    /* I_MASK: plain read/write */
    iop_intc_mmio_write32(0x1F801074u, 0xDEADBEEFu);
    iop_intc_mmio_read32(0x1F801074u, &v);
    CHECK(v == 0xDEADBEEFu, "I_MASK plain read/write roundtrip");

    /* I_CTRL: write is plain, but READ clears the register as a side
     * effect (one-shot latch) */
    iop_intc_mmio_write32(0x1F801078u, 0x00000001u);
    iop_intc_mmio_read32(0x1F801078u, &v);
    CHECK(v == 0x00000001u, "I_CTRL first read returns the written value");
    iop_intc_mmio_read32(0x1F801078u, &v);
    CHECK(v == 0u, "I_CTRL second read returns 0 - the first read cleared it (one-shot latch)");

    /* Unmodeled address not claimed */
    CHECK(iop_intc_mmio_read32(0x1F801080u, &v) == 0, "unmodeled address not claimed on read");
    CHECK(iop_intc_mmio_write32(0x1F801080u, 0x1u) == 0, "unmodeled address not claimed on write");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
