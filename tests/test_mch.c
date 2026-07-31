/*
 * test_mch.c - host-native test for source/hw/mch.c
 *
 * Verifies the MCH_RICM/MCH_DRD RDRAM auto-init register logic ported
 * from the documented PS2Tek/PCSX2 reference (see include/core/hw/mch.h
 * for the full citation and docs/STATUS.md's "round 11" section for
 * the live-trace investigation that found this register pair missing
 * was the root cause of a wrong branch taken during real BIOS boot).
 */
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "hw/mch.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else printf("ok:   %s\n", msg); \
} while (0)

int main(void)
{
    mch_init();
    uint32_t v;

    /* MCH_RICM always reads back 0, regardless of what was written. */
    CHECK(mch_mmio_write32(0x1000F430u, 0x00210000u) == 1, "MCH_RICM write accepted");
    CHECK(mch_mmio_read32(0x1000F430u, &v) == 1 && v == 0u, "MCH_RICM always reads back 0");

    mch_init();

    /* SA=0x21 (INIT) enumeration: with SOP=0 and SA=0x21 selected via
     * MCH_RICM, the first MCH_RDRAM_DEVICES (2) reads of MCH_DRD each
     * return 0x1F (a device responded), then 0 (no more devices). */
    CHECK(mch_mmio_write32(MCH_RICM, 0x00210000u) == 1, "select SA=0x21 (INIT) via MCH_RICM");
    CHECK(mch_mmio_read32(MCH_DRD, &v) == 1 && v == 0x1Fu, "1st SDEVID enumeration read returns 0x1F");
    CHECK(mch_mmio_read32(MCH_DRD, &v) == 1 && v == 0x1Fu, "2nd SDEVID enumeration read returns 0x1F");
    CHECK(mch_mmio_read32(MCH_DRD, &v) == 1 && v == 0u, "3rd read (no more devices) returns 0");
    CHECK(mch_mmio_read32(MCH_DRD, &v) == 1 && v == 0u, "further reads keep returning 0");

    /* SA==0x21 && SBC==0x1 (bits 6-9 of the MCH_RICM write) resets the
     * enumeration counter back to 0, as long as MCH_DRD's bit 7 isn't
     * set. */
    CHECK(mch_mmio_write32(MCH_RICM, 0x00210040u) == 1, "SA=0x21,SBC=0x1 strobe write resets enumeration");
    /* The strobe write itself leaves MCH_RICM's SOP field non-zero
     * (SBC=1 was written into the same bit range SOP reads from), so
     * a read right after the strobe still sees SOP!=0 and returns 0 -
     * matches the reference logic exactly (real BIOS code re-selects
     * SA=0x21 with SBC=0 before reading results, same two-step
     * pattern as the very first enumeration above). */
    CHECK(mch_mmio_read32(MCH_DRD, &v) == 1 && v == 0u,
          "read right after the strobe itself sees SOP!=0, returns 0");
    CHECK(mch_mmio_write32(MCH_RICM, 0x00210000u) == 1, "re-select SA=0x21 with SOP=0 to actually read results");
    CHECK(mch_mmio_read32(MCH_DRD, &v) == 1 && v == 0x1Fu, "enumeration restarted from device 0");

    /* Reset gated off when MCH_DRD's bit 7 is set. */
    mch_init();
    CHECK(mch_mmio_write32(MCH_RICM, 0x00210000u) == 1, "select SA=0x21 again");
    mch_mmio_read32(MCH_DRD, &v); mch_mmio_read32(MCH_DRD, &v); /* exhaust both devices -> counter=2 */
    CHECK(mch_mmio_write32(MCH_DRD, 0x00000080u) == 1, "set MCH_DRD bit 7");
    CHECK(mch_mmio_write32(MCH_RICM, 0x00210040u) == 1, "SA=0x21,SBC=0x1 write, but bit 7 set");
    CHECK(mch_mmio_read32(MCH_DRD, &v) == 1 && v == 0u,
          "enumeration NOT reset while MCH_DRD bit 7 is set (still exhausted)");

    /* SA=0x23 (CNFGA) and SA=0x24 (CNFGB) fixed readback values. */
    mch_init();
    CHECK(mch_mmio_write32(MCH_RICM, 0x00230000u) == 1, "select SA=0x23 (CNFGA)");
    CHECK(mch_mmio_read32(MCH_DRD, &v) == 1 && v == 0x0D0Du, "CNFGA reads back 0x0D0D");
    CHECK(mch_mmio_write32(MCH_RICM, 0x00240000u) == 1, "select SA=0x24 (CNFGB)");
    CHECK(mch_mmio_read32(MCH_DRD, &v) == 1 && v == 0x0090u, "CNFGB reads back 0x0090");

    /* SA=0x40 echoes back MCH_RICM & 0x1F. */
    CHECK(mch_mmio_write32(MCH_RICM, 0x00400017u) == 1, "select SA=0x40 with low bits 0x17");
    CHECK(mch_mmio_read32(MCH_DRD, &v) == 1 && v == 0x17u, "SA=0x40 echoes MCH_RICM & 0x1F");

    /* SOP != 0 always returns 0 regardless of SA. */
    CHECK(mch_mmio_write32(MCH_RICM, 0x002100C0u) == 1, "select SA=0x21 with SOP!=0 (bits 6-9 = 0x3)");
    CHECK(mch_mmio_read32(MCH_DRD, &v) == 1 && v == 0u, "SOP!=0 always reads back 0");

    /* Non-MCH addresses are not claimed by this module. */
    CHECK(mch_mmio_read32(0x1000F420u, &v) == 0, "unrelated address not claimed (read)");
    CHECK(mch_mmio_write32(0x1000F420u, 0u) == 0, "unrelated address not claimed (write)");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
