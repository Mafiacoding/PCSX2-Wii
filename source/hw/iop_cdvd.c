/*
 * iop_cdvd.c - see include/core/hw/iop_cdvd.h for scope notes.
 */
#include "core/hw/iop_cdvd.h"
#include <string.h>

/* Register offsets within the page (real hardware, ps2tek + PCSX2's
 * pcsx2/CDVD/CDVD.cpp cdvdRead/cdvdWrite switch statements). */
#define OFF_NCMD        0x04u
#define OFF_NREADY      0x05u
#define OFF_ERROR       0x06u
#define OFF_BREAK       0x07u
#define OFF_INTR_STAT   0x08u
#define OFF_STATUS      0x0Au
#define OFF_STATUS_STK  0x0Bu
#define OFF_TYPE        0x0Fu

static uint8_t g_regs[IOP_CDVD_SIZE];

void iop_cdvd_init(void)
{
    memset(g_regs, 0, sizeof(g_regs));

    /* Real cdvdReset() defaults (pcsx2/CDVD/CDVD.cpp) for the
     * diskless-boot case this scaffold targets - see the header
     * comment for the full citation. */
    g_regs[OFF_NREADY]     = IOP_CDVD_DRIVE_READY;
    g_regs[OFF_STATUS]     = IOP_CDVD_STATUS_TRAY_OPEN;
    g_regs[OFF_STATUS_STK] = IOP_CDVD_STATUS_TRAY_OPEN;
    g_regs[OFF_TYPE]       = IOP_CDVD_TYPE_NODISC;
    g_regs[OFF_ERROR]      = 0u;
    g_regs[OFF_INTR_STAT]  = 0u;
}

int iop_cdvd_mmio_read8(uint32_t addr, uint8_t *out)
{
    /* Round 130 (task #172/#196, 170th finding): the IOP has no MMU/
     * TLB, so KUSEG (0x1F402xxx), KSEG0 (0x9F402xxx) and KSEG1
     * (0xBF402xxx) all address the SAME physical CDVD register page -
     * real code frequently polls hardware status via the KSEG1
     * (uncached) alias specifically to avoid a stale cached read
     * during a busy-wait, exactly the pattern task #165 already found
     * and fixed for the SIF mailbox window (see sif.c's matching
     * comment). This check previously only accepted the bare KUSEG
     * form, so a real KSEG1-alias poll (e.g. 0xBF40200A) silently
     * fell through to unmapped/out-of-range RAM (always reading 0)
     * instead of the real register - found via live host-native
     * tracing showing the IOP polling this exact KSEG1 address in a
     * tight loop after Round 129's exception-vector fix unblocked
     * further boot progress. Masking off the segment-select bits
     * before the window check, same as sif.c's iop_mem_ptr()-style
     * convention, fixes all three aliases at once. */
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_CDVD_BASE || phys >= IOP_CDVD_BASE + 0x1000u) return 0;
    /* Real hardware mirrors these byte registers across the whole
     * 4KB page - PCSX2's psxHw4Read8/Write8 masks the address to its
     * low 8 bits before dispatching, so replicate that here rather
     * than only accepting the first IOP_CDVD_SIZE bytes of the page. */
    uint32_t off = (phys - IOP_CDVD_BASE) & 0xFFu;

    if (off == OFF_ERROR) {
        /* Real behavior: reading ERROR returns its current value and
         * clears it (pcsx2/CDVD/CDVD.cpp's cdvdRead06). */
        *out = g_regs[off];
        g_regs[off] = 0u;
        return 1;
    }
    if (off == OFF_BREAK) {
        /* Real hardware: BREAK always reads back 0 (cdvdRead07). */
        *out = 0u;
        return 1;
    }

    *out = g_regs[off];
    return 1;
}

int iop_cdvd_mmio_write8(uint32_t addr, uint8_t value)
{
    /* Round 130 (170th finding): same KUSEG/KSEG0/KSEG1 aliasing fix
     * as iop_cdvd_mmio_read8() above - see that function's comment
     * for the full citation trail. */
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_CDVD_BASE || phys >= IOP_CDVD_BASE + 0x1000u) return 0;
    uint32_t off = (phys - IOP_CDVD_BASE) & 0xFFu;

    g_regs[off] = value;

    if (off == OFF_NCMD) {
        /* Real hardware: writing NCMD kicks off the real N-command
         * state machine (seek/read/standby/stop/etc - not modeled
         * here, see header comment). This scaffold reports an
         * immediate, real completion IRQ code
         * (Irq_CommandComplete=0, per PCSX2's CdvdIrqId enum) instead
         * of leaving BUSY set forever, so a diskless boot's status
         * polling loop can observe "command done" and move on rather
         * than spin indefinitely on a command this project doesn't
         * implement the real behavior of. */
        g_regs[OFF_INTR_STAT] = IOP_CDVD_IRQ_COMMAND_COMPLETE;
    }

    return 1;
}

uint8_t iop_cdvd_get_last_ncommand(void) { return g_regs[OFF_NCMD]; }
uint8_t iop_cdvd_get_status(void)        { return g_regs[OFF_STATUS]; }
uint8_t iop_cdvd_get_ready(void)         { return g_regs[OFF_NREADY]; }
uint8_t iop_cdvd_get_disc_type(void)     { return g_regs[OFF_TYPE]; }
