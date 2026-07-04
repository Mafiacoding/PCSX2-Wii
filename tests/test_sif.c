/*
 * test_sif.c - host-native test for source/hw/sif.c
 *
 * Verifies the register-level semantics ported from real PCSX2
 * (HwWrite.cpp/HwRead.cpp SBUS_F2xx cases): MSCOM plain read/write,
 * MSFLAG OR-on-write, SMFLAG AND-NOT-on-write (write-1-to-clear),
 * CTRL's read-side 0xF0000102 OR mask and its bit-0x100 lock flag
 * behavior, and the F260 reset value.
 */
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "hw/sif.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else printf("ok:   %s\n", msg); \
} while (0)

int main(void)
{
    sif_init();

    uint32_t v;

    /* F260 reset value */
    CHECK(sif_mmio_read32(0x1000F260u, &v) == 1 && v == 0x1D000060u,
          "F260 resets to 0x1D000060");

    /* MSCOM: plain read/write */
    CHECK(sif_mmio_write32(0x1000F200u, 0x12345678u) == 1, "MSCOM write accepted");
    CHECK(sif_mmio_read32(0x1000F200u, &v) == 1 && v == 0x12345678u,
          "MSCOM plain readback");

    /* SMCOM: plain read/write (modeled, no IOP writer yet) */
    sif_mmio_write32(0x1000F210u, 0xAABBCCDDu);
    sif_mmio_read32(0x1000F210u, &v);
    CHECK(v == 0xAABBCCDDu, "SMCOM plain readback");

    /* MSFLAG: OR on write */
    sif_mmio_write32(0x1000F220u, 0x0000000Fu);
    sif_mmio_write32(0x1000F220u, 0x000000F0u);
    sif_mmio_read32(0x1000F220u, &v);
    CHECK(v == 0x000000FFu, "MSFLAG ORs successive writes together");

    /* SMFLAG: AND-NOT on write (write-1-to-clear) */
    g_sif.smflag = 0xFFu; /* simulate the IOP having set some flag bits */
    sif_mmio_write32(0x1000F230u, 0x0Fu); /* EE clears the low nibble */
    sif_mmio_read32(0x1000F230u, &v);
    CHECK(v == 0xF0u, "SMFLAG write clears (ANDs ~value) rather than overwriting");

    /* CTRL: read-side OR mask. Note this fixed mask (0xF0000102)
     * already includes bit 0x100 - so on real hardware/PCSX2, EVERY
     * read of this register shows bit 0x100 set no matter what the
     * internal lock-flag state is. This was caught while writing
     * this test: an earlier version of this test checked the bit
     * via sif_mmio_read32() after clearing it and failed, because
     * the read-side mask itself sets that bit unconditionally. The
     * internal state is still tracked correctly (see the direct
     * g_sif.ctrl checks below) - it's just not observable through
     * this read path, which matches real hardware/PCSX2 behavior. */
    sif_mmio_read32(0x1000F240u, &v);
    CHECK(v == 0xF0000102u, "CTRL reads as 0xF0000102 when register itself is 0");
    CHECK((0xF0000102u & 0x100u) == 0x100u,
          "CTRL's fixed read mask always includes bit 0x100 (real hardware quirk)");

    /* CTRL: bit 0x100 lock flag set/clear - checked against internal
     * state directly, since the read path always ORs that bit in. */
    sif_mmio_write32(0x1000F240u, 0x100u);
    CHECK((g_sif.ctrl & 0x100u) == 0x100u, "CTRL internal bit 0x100 set when written with bit set");
    sif_mmio_write32(0x1000F240u, 0x0u);
    CHECK((g_sif.ctrl & 0x100u) == 0u, "CTRL internal bit 0x100 cleared when written without bit set");

    /* F250: plain storage */
    sif_mmio_write32(0x1000F250u, 0x55AA55AAu);
    sif_mmio_read32(0x1000F250u, &v);
    CHECK(v == 0x55AA55AAu, "F250 plain readback");

    /* Unmodeled address must return 0 (not claimed) */
    CHECK(sif_mmio_read32(0x1000F270u, &v) == 0, "unmodeled address not claimed on read");
    CHECK(sif_mmio_write32(0x1000F270u, 0x1u) == 0, "unmodeled address not claimed on write");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
