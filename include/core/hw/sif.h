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

/* Returns how many times sif_cmd_iop_handle_init_cmd() has been
 * called so far (0 if never). Used by ee_core.c to decide when to
 * synthesize the IOP's SIF_CMD_SET_SREG(RPCINIT,1) response - see the
 * 62nd/63rd findings in docs/STATUS.md. */
uint32_t sif_cmd_iop_get_init_cmd_count(void);

/*
 * --- task #187 (docs/STATUS.md 63rd finding): real SIF_CMD_SET_SREG /
 * sregs[32] / SIF_SREG_RPCINIT grounding ---
 *
 * Fetched the FULL real ee/kernel/src/sifcmd.c (previously only had
 * fragments) and found byte-exact confirmation that this project's
 * own 57th/58th-finding "32-entry table at 0x0008C440" is REAL
 * ps2sdk's `static int sregs[32]` (exactly 32 ints = 128 bytes,
 * matching the real zero-fill loop found via ROM disassembly in the
 * 58th finding), and that `0x0008C440` (index 0) is real ps2sdk's
 * `SIF_SREG_RPCINIT`. The real EE-side dispatch mechanism
 * (`_SifCmdIntHandler()`, `sys_cmd_handlers[32]`, the `set_sreg()`
 * handler performing `cmd_data->sregs[pkt->sreg] = pkt->val`) is ALL
 * real, genuine BIOS/kernel code that this project's own EE
 * interpreter ALREADY executes correctly once invoked - nothing new
 * needs to be modeled on the EE side. The one remaining real gap is
 * the same one this project has faced since the 59th finding: no
 * real IOP-side code to actually SEND the `SIF_CMD_SET_SREG(sreg=
 * SIF_SREG_RPCINIT, val=1)` packet that real hardware's IOP sends once
 * its own SIFCMD/RPC init completes.
 *
 * `SIF_CMD_SET_SREG` / `SIF_SREG_RPCINIT` below are real, cited
 * constants (SIF_CMD_ID_SYSTEM|1, and 0 respectively, per the fetched
 * sifcmd.c and this session's earlier sifcmd-common.h citation - NOT
 * guessed). `sif_cmd_iop_send_rpcinit_ready()`'s CONTENT (the real
 * `struct sr_pkt` layout: header + sreg + val) is byte-exact from the
 * fetched source. Its TRIGGER TIMING (fired synthetically on the
 * second observed SIF_CMD_INIT_CMD send, from ee_core.c's syscall 119
 * handler) is NOT byte-exact real IOP behavior - it is an explicitly
 * labeled approximation, chosen because real IOP-side assembly
 * remains unobtainable (every fetch avenue exhausted - 61st finding).
 * See docs/STATUS.md's 63rd finding for full detail, results, and
 * honest caveats.
 */
#define SIF_CMD_SET_SREG   0x80000001u /* SIF_CMD_ID_SYSTEM | 1 */
#define SIF_SREG_RPCINIT   0u

/* task #192 (68th finding): real SIF RPC command IDs, confirmed via
 * both the fetched ee/kernel/src/sifrpc.c (user-supplied
 * ps2sdk-master.zip) and the psdevwiki-mirrored ps2tek pages the user
 * pointed to this round (israpps.github.io/ps2tek/PS2/SIF/RPC_Cmds.html,
 * PS2/SIF/RPC_System_services.html) - real, cited constants, not
 * guessed. `SIF_CMD_RPC_BIND` (0x80000009) is what this project's own
 * traced real caller past CreateSema/WaitSema (task #191's 67th
 * finding, byte-exact match to real `sceSifBindRpc()`) sends;
 * `SIF_CMD_RPC_END` (0x80000008) is what the real IOP replies with
 * once the bind completes, dispatched by the SAME real, already-
 * resident `_SifCmdIntHandler()` this project already drives for
 * SIF_CMD_SET_SREG (63rd finding) - but this time through its
 * `usr_cmd_handlers[]` path (real `sceSifAddCmdHandler()`-registered,
 * confirmed via the fetched sifcmd.c's `_SifCmdIntHandler()` dispatch
 * logic: `if (cid & SIF_CMD_ID_SYSTEM) {...sys_cmd_handlers...} else
 * {...usr_cmd_handlers...}`) rather than `sys_cmd_handlers[]`. The
 * real EE-side handler this drives, `_request_end()` (fetched
 * ee/kernel/src/sifrpc.c), reads `request->cd` (the real
 * `SifRpcClientData_t*` echoed back from the original Bind packet)
 * and calls `iSignalSema(cd->hdr.sema_id)` - genuine, already-resident
 * BIOS/kernel code, executed for real once invoked; only the INCOMING
 * REND PACKET CONTENT is synthesized here (byte-exact real
 * `SifRpcRendPkt_t` layout), same style/precedent as
 * `sif_cmd_iop_send_rpcinit_ready()` above. */
#define SIF_CMD_RPC_END    0x80000008u /* SIF_CMD_ID_SYSTEM | 8 */
#define SIF_CMD_RPC_BIND   0x80000009u /* SIF_CMD_ID_SYSTEM | 9 */
#define SIF_CMD_RPC_CALL   0x8000000Au /* SIF_CMD_ID_SYSTEM | 10 - task #195/#196 (71st finding).
                                        * Confirmed via the fetched ee/kernel/src/sifrpc.c's
                                        * sceSifCallRpc(): sends this cid via sceSifSendCmd()
                                        * with a RPC_PACKET_SIZE=64-byte SifRpcCallPkt_t header
                                        * (byte-exact match: this project's own diagnostic
                                        * observed size=64 for this send, matching
                                        * RPC_PACKET_SIZE exactly) plus a second DMA descriptor
                                        * carrying the caller's raw sendbuf payload (matching
                                        * _SifSendCmd()'s own "if (size>0) {...}" extra-payload
                                        * branch, already byte-exact confirmed in the 67th
                                        * finding). Real SifRpcCallPkt_t layout (56 bytes, from
                                        * common/include/sifrpc-common.h, all 4-byte fields):
                                        *   0x00 sifcmd (SifCmdHeader_t, 16B), 0x10 rec_id,
                                        *   0x14 pkt_addr, 0x18 rpc_id, 0x1C cd, 0x20 rpc_number,
                                        *   0x24 send_size, 0x28 recvbuf, 0x2C recv_size,
                                        *   0x30 rmode, 0x34 sd.
                                        * _request_end() (same real, already-resident handler
                                        * this project already drives for RPC_BIND) dispatches
                                        * on this cid too - for SIF_CMD_RPC_CALL it only calls
                                        * cd->end_function if set (NULL for the synchronous
                                        * calls this project has observed) then unconditionally
                                        * iSignalSema(cd->hdr.sema_id), exactly like the BIND
                                        * case - no cd->server/buf/cbuf writes for this cid, so
                                        * this project's REND reply's sd/buf/cbuf fields are
                                        * irrelevant for a CALL reply. The actual RPC RESULT
                                        * DATA is delivered separately, directly into the
                                        * caller-supplied recvbuf (a plain EE-memory write this
                                        * project performs directly, matching how a real IOP
                                        * service would DMA its own reply data there before
                                        * sending the REND "done" signal) - not part of the REND
                                        * packet's own fields at all. */

void sif_cmd_iop_handle_rpc_bind(uint32_t cd_ptr);
uint32_t sif_cmd_iop_get_rpc_bind_cd(void);
uint32_t sif_cmd_iop_get_rpc_bind_count(void);

#endif
