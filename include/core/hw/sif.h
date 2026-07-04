/*
 * sif.h - EE-side SIF/SBUS mailbox registers (0x1000F200-0x1000F267)
 *
 * The SIF (Sub-system Interface) is how the EE and IOP hand-shake and
 * exchange short control messages before/around DMA transfers proper
 * (SIF0/SIF1/SIF2 DMA channels, already modeled as channel slots in
 * dma.c, move the bulk data - this file is just the mailbox/flag
 * registers used to synchronize both sides).
 *
 * Register semantics ported from real PCSX2 source
 * (pcsx2/HwWrite.cpp, pcsx2/HwRead.cpp, pcsx2/Hw.cpp - the SBUS_F2xx
 * cases), not reinvented:
 *
 *   0x1000F200 MSCOM  - EE writes a mailbox value for the IOP to read.
 *                       Plain read/write (PCSX2's write handler for
 *                       this address is a no-op comment: "performs a
 *                       standard psHu32 assignment, which is the
 *                       default action anyway").
 *   0x1000F210 SMCOM  - IOP's mailbox value, EE reads it. Modeled as
 *                       plain storage; on real hardware only the IOP
 *                       side actually writes it meaningfully. Since
 *                       iop_core.c isn't wired to a shared MMIO bus
 *                       yet (see docs/ROADMAP.md), nothing currently
 *                       writes this from the IOP side - it will
 *                       always read back whatever the EE last wrote,
 *                       which is not real hardware behavior but is
 *                       the honest current state of the emulation.
 *   0x1000F220 MSFLAG - EE sets bits (write ORs into the register);
 *                       real firmware polls this from the IOP side to
 *                       know the EE has signaled something.
 *   0x1000F230 SMFLAG - IOP sets bits, EE write ANDs them off
 *                       (write value is inverted and ANDed in - i.e.
 *                       "write 1 to clear", same idea as GS_CSR).
 *   0x1000F240 CTRL   - control/reset register. Real hardware: bit
 *                       0x40000 (1<<18) raises IOP INTC IRQ 1, bit
 *                       0x80000 (1<<19) triggers a full IOP reset,
 *                       bit 0x100 is a busy/lock flag toggled by the
 *                       write value's own bit 0x100. Reads OR in a
 *                       fixed 0xF0000102. The IRQ/reset side effects
 *                       are NOT modeled yet (no-op) since there is no
 *                       cross-CPU wiring to the IOP core yet - only
 *                       the bit-0x100 lock flag and the read-side OR
 *                       mask are real, tested behavior here.
 *   0x1000F250        - unused by PCSX2's own special-case handling;
 *                       modeled as plain read/write storage.
 *   0x1000F260        - plain read/write storage, hardware reset
 *                       value 0x1D000060 (an IOP-side address that
 *                       the EE reads to find the IOP's own SIF
 *                       register mirror - meaningless without a real
 *                       IOP-side SIF implementation, but the register
 *                       value itself is real).
 *
 * Scope: this is deliberately just the register/mailbox layer, not a
 * working EE<->IOP handshake protocol - actually driving a real boot
 * handshake needs the IOP core wired to the same bus and running
 * interleaved with the EE, which is still open (see docs/ROADMAP.md).
 */
#ifndef PCSX2_WII_SIF_H
#define PCSX2_WII_SIF_H

#include <stdint.h>

typedef struct {
    uint32_t mscom;
    uint32_t smcom;
    uint32_t msflag;
    uint32_t smflag;
    uint32_t ctrl;
    uint32_t f250;
    uint32_t f260;
} sif_state_t;

void sif_init(void);

/* Returns 1 and fills *out if addr is a modeled SIF register,
 * 0 (leaves *out untouched) otherwise - same convention as
 * dma_mmio_read32/write32. */
int sif_mmio_read32(uint32_t addr, uint32_t *out);
int sif_mmio_write32(uint32_t addr, uint32_t value);

sif_state_t *sif_get_state(void);

#endif
