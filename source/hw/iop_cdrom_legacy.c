/*
 * iop_cdrom_legacy.c - see include/core/hw/iop_cdrom_legacy.h for
 * scope notes and full citation trail (Round 133, 173rd finding).
 */
#include "core/hw/iop_cdrom_legacy.h"
#include <string.h>

static uint8_t g_status;

void iop_cdrom_legacy_init(void)
{
    /* Real, cited idle/reset state (psx-spx): register bank RA=0,
     * ADPBUSY=0 (not playing XA-ADPCM), PRMEMPT=1 (parameter FIFO
     * empty - the real power-on/idle state, and the specific bit
     * this project's own traced boot-time polling loop requires -
     * see header comment for the full instruction-level trace),
     * PRMWRDY=0, RSLRRDY=0 (no result pending, nothing queried yet),
     * DRQSTS=0 (no data request pending), BUSYSTS=0 (HC05 not busy -
     * no command in flight). This project models only this one
     * concrete, cited bit's real value; the other bits are left at
     * their real, equally-cited "nothing happening" defaults rather
     * than guessed. */
    g_status = IOP_CDROM_LEGACY_STATUS_PRMEMPT;
}

int iop_cdrom_legacy_mmio_read8(uint32_t addr, uint8_t *out)
{
    /* Same KUSEG/KSEG0/KSEG1-alias masking convention already
     * established for the SIF mailbox (task #165) and the PS2-native
     * CDVD page (Round 130, 170th finding) - the IOP has no MMU/TLB,
     * so all three segment forms address the same physical location. */
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_CDROM_LEGACY_BASE || phys >= IOP_CDROM_LEGACY_BASE + IOP_CDROM_LEGACY_SIZE)
        return 0;
    uint32_t off = phys - IOP_CDROM_LEGACY_BASE;

    if (off == 0u) {
        /* Offset 0: the Index/Status Register - the only offset this
         * project's own traced boot-time loop actually reads (see
         * header comment). */
        *out = g_status;
        return 1;
    }

    /* Offsets 1-3 (the bank-switched parameter/response/data byte
     * registers) are not modeled - this project has no real PS1
     * CD-ROM command/response protocol implemented, only the one
     * status bit its own boot trace depends on. Reading back 0 here
     * is the same "unimplemented, return a safe default" convention
     * used throughout this project's HLE stubs. */
    *out = 0u;
    return 1;
}

int iop_cdrom_legacy_mmio_write8(uint32_t addr, uint8_t value)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_CDROM_LEGACY_BASE || phys >= IOP_CDROM_LEGACY_BASE + IOP_CDROM_LEGACY_SIZE)
        return 0;
    /* No real command/response state machine modeled (see header) -
     * writes are accepted (so real code polling "did my write stick"
     * on offset 0's RA bits would get a truthful readback, matching
     * this project's existing NCMD-latching precedent in iop_cdvd.c)
     * but otherwise have no side effect; PRMEMPT is intentionally
     * left set rather than cleared, since this project never models
     * the parameter-byte-count state that would legitimately clear
     * it. */
    uint32_t off = phys - IOP_CDROM_LEGACY_BASE;
    if (off == 0u) {
        g_status = (g_status & ~0x03u) | (value & 0x03u); /* RA is R/W */
    }
    (void)value;
    return 1;
}
