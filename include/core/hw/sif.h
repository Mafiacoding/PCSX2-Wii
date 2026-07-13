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

/*
 * IOP-side mirror, addresses 0x1D000000-0x1D0000FF. Real PCSX2 models
 * this IOP-side window as a flat, un-special-cased 0x100-byte array
 * (see MemoryTypes.h: "u8 Sif[0x100]; // a few special SIF/SBUS
 * registers (likely not needed)" and IopMem.h's psxSu32(mem) macro,
 * which indexes that array with `mem & 0xff` and does a plain typed
 * read/write - no OR/AND-on-write special casing at all on this
 * side). This project follows PCSX2's own lead here rather than
 * inventing stronger semantics than the reference implementation
 * actually has: IOP-side reads/writes are plain, unlike the EE-side
 * MSFLAG/SMFLAG/CTRL special cases above.
 *
 * The low byte of the IOP-side address lines up with the low byte of
 * the corresponding EE-side address (0x1D0000XX <-> 0x1000F2XX), so
 * the same underlying sif_state_t fields back both sides - this is a
 * deliberate, documented convenience of this implementation, not a
 * claim about real internal wiring.
 */
int sif_iop_mmio_read32(uint32_t addr, uint32_t *out);
int sif_iop_mmio_write32(uint32_t addr, uint32_t value);

sif_state_t *sif_get_state(void);

/*
 * --- Minimal, explicitly-labeled IOP-side SIFCMD command-consumer
 * model (task #172/#186) ---
 *
 * Scope and honesty note (docs/STATUS.md 61st/62nd findings): this
 * project obtained and byte-matched the REAL EE-side ps2sdk source
 * (ee/kernel/src/sifcmd.c, Academic Free License 2.0) for
 * sceSifInitCmd()/_SifSendCmd()/_SifCmdIntHandler(), confirming this
 * project's own traced boot packets are genuine, unmodified
 * SIF_CMD_INIT_CMD (cid=0x80000002) sends. The IOP-side counterpart
 * (ps2sdk's iop/kernel/src/sifcmd.s) is real MIPS assembly that this
 * round's fetch tools could not retrieve after multiple attempts
 * (raw.githubusercontent.com, GitHub's blob/tree pages, GitHub's
 * content API, ps2dev.github.io doxygen - all returned empty for this
 * one specific path; see the 61st finding for the full list).
 * Independent, real (though not byte-exact) corroboration of the
 * high-level protocol WAS found: "the IOP uses a software SIF
 * register to tell the EE what the IOP has stored for the EE's
 * receive buffer" (via WebSearch, a legitimate PS2 homebrew
 * documentation source), and by direct protocol symmetry with this
 * project's own real, byte-exact EE-side SIF_CMD_CHANGE_SADDR handler
 * (`cmd_data->iopbuf = pkt->buf` in the fetched sifcmd.c), the natural
 * real counterpart action for the IOP receiving SIF_CMD_INIT_CMD is
 * recording the EE's own reply/receive buffer address (the packet's
 * `ca_pkt.buf` field).
 *
 * sif_cmd_iop_handle_init_cmd() models EXACTLY that one, narrowly-
 * grounded effect - a software bookkeeping record of the EE's buffer
 * address - and NOTHING more. It does NOT claim to be a port of real
 * IOP assembly (which this project does not have), does NOT drive any
 * hardware SIF register on its own, and is NOT confirmed to be what
 * unblocks the still-open 0x0008C440 poll from the 56th/57th/59th
 * findings - that poll may instead be gated by the AddDmacHandler-
 * populated 32-entry table, whose exact real indexing convention
 * remains unconfirmed. Documented explicitly so a future round with
 * access to the real IOP assembly can verify or replace this model.
 *
 * Invoked synchronously from the EE's sceSifSetDma (syscall 119)
 * handler in ee_core.c right after a real EE-RAM-to-IOP-RAM copy,
 * rather than from a running IOP-side consumer loop, because this
 * project's IOP has already gone idle by this point in a real boot
 * (59th finding) - there is no live IOP dispatcher to trigger this
 * naturally yet. Placed in sif.c/sif.h (rather than a separate
 * translation unit) purely so every existing test that already links
 * sif.c keeps building without any test-harness changes.
 */

/* SIF_CMD_INIT_CMD = SIF_CMD_ID_SYSTEM(0x80000000) | 2, per ps2sdk's
 * common/include/sifcmd-common.h (61st finding). */
#define SIF_CMD_INIT_CMD 0x80000002u

void sif_cmd_iop_init(void);
void sif_cmd_iop_handle_init_cmd(uint32_t ee_recvbuf_addr);
uint32_t sif_cmd_iop_get_ee_recvbuf(void);

#endif
