/*
 * test_iop_cdvd.c - host-native test for the CDVD register scaffold
 * (ROADMAP section 7's "CDVD - disc/BIOS-boot-media emulation" item).
 *
 * See include/core/hw/iop_cdvd.h for the full design rationale and
 * citation trail (register offsets, power-on defaults, and the
 * ERROR/BREAK read-behavior quirks are all ported directly from
 * PCSX2's own pcsx2/CDVD/CDVD.cpp `cdvdReset()`/`cdvdRead()`, not
 * reinvented).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "core/hw/iop_cdvd.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    iop_cdvd_init();

    /* --- Real cdvdReset() power-on defaults (diskless boot case) --- */
    {
        uint8_t v;
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x05u, &v) && v == IOP_CDVD_DRIVE_READY,
              "N-READY (offset 0x05) defaults to CDVD_DRIVE_READY (0x40)");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x0Au, &v) && v == IOP_CDVD_STATUS_TRAY_OPEN,
              "STATUS (offset 0x0A) defaults to CDVD_STATUS_TRAY_OPEN (0x01)");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x0Bu, &v) && v == IOP_CDVD_STATUS_TRAY_OPEN,
              "STATUS STICKY (offset 0x0B) defaults to CDVD_STATUS_TRAY_OPEN too");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x0Fu, &v) && v == IOP_CDVD_TYPE_NODISC,
              "TYPE (offset 0x0F) defaults to CDVD_TYPE_NODISC (0x00)");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x08u, &v) && v == 0u,
              "INTR_STAT (offset 0x08) defaults to 0 (no pending interrupt)");
        CHECK(iop_cdvd_get_status() == IOP_CDVD_STATUS_TRAY_OPEN, "accessor: get_status matches register");
        CHECK(iop_cdvd_get_ready() == IOP_CDVD_DRIVE_READY, "accessor: get_ready matches register");
        CHECK(iop_cdvd_get_disc_type() == IOP_CDVD_TYPE_NODISC, "accessor: get_disc_type matches register");
    }

    /* --- ERROR register (offset 0x06): read-clears real behavior --- */
    {
        uint8_t v;
        CHECK(iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x06u, 0x42u), "write to ERROR register accepted");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x06u, &v) && v == 0x42u,
              "first read of ERROR returns the written value");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x06u, &v) && v == 0u,
              "second read of ERROR returns 0 - real hardware clears it on read");
    }

    /* --- BREAK register (offset 0x07): always reads 0 --- */
    {
        uint8_t v;
        iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x07u, 0xFFu);
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x07u, &v) && v == 0u,
              "BREAK register always reads back 0 regardless of what was written");
    }

    /* --- NCMD register (offset 0x04): latched + triggers a
     * plausible completion IRQ instead of staying busy forever --- */
    {
        uint8_t v;
        CHECK(iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x04u, 0x0Cu), "write NCMD=0x0C accepted");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x04u, &v) && v == 0x0Cu,
              "NCMD readback matches what was written (real hardware latches it)");
        CHECK(iop_cdvd_get_last_ncommand() == 0x0Cu, "accessor: get_last_ncommand matches");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x08u, &v) && v == IOP_CDVD_IRQ_COMMAND_COMPLETE,
              "writing NCMD sets INTR_STAT to Irq_CommandComplete (0) instead of leaving it busy");
    }

    /* --- Real hardware mirrors the byte registers across the whole
     * 4KB page (address masked to low 8 bits by PCSX2's own
     * psxHw4Read8/Write8) - verify that mirroring is replicated. --- */
    {
        uint8_t v1, v2;
        iop_cdvd_mmio_write8(IOP_CDVD_BASE + 0x05u, 0x99u);
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x105u, &v1) && v1 == 0x99u,
              "register at +0x100 mirrors the same byte as +0x00 (page mirroring)");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0xF05u, &v2) && v2 == 0x99u,
              "register at +0xF00 also mirrors the same byte (full 4KB page)");
    }

    /* --- Addresses outside the CDVD window are correctly rejected --- */
    {
        uint8_t v;
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE - 4u, &v) == 0,
              "address just below IOP_CDVD_BASE is correctly rejected (returns 0/not-handled)");
        CHECK(iop_cdvd_mmio_read8(IOP_CDVD_BASE + 0x1000u, &v) == 0,
              "address at the end of the 4KB page (+0x1000) is correctly rejected");
    }

    if (failures) {
        printf("\n%d CHECK(S) FAILED\n", failures);
        return 1;
    }
    printf("\nAll checks passed.\n");
    return 0;
}
