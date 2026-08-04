/*
 * sif.c - EE-side SIF/SBUS mailbox register model. See sif.h for the
 * exact per-register semantics and PCSX2 source cross-reference.
 */
#include "core/hw/sif.h"
#include "core/hw/ee_intc.h"
#include <string.h>

#define SIF_MSCOM  0x1000F200u
#define SIF_SMCOM  0x1000F210u
#define SIF_MSFLAG 0x1000F220u
#define SIF_SMFLAG 0x1000F230u
#define SIF_CTRL   0x1000F240u
#define SIF_F250   0x1000F250u
#define SIF_F260   0x1000F260u

/* Round 317 (task #423 continuation): real EE INTC source index 1 =
 * INTC_SBUS - same real, cited constant iop_icfg.c already defines
 * locally for its own SBUS-raising write handler (PCSX2's Dmac.h enum
 * INTCIrqs / ps2sdk's kernel.h identical list). Defined locally here
 * too, matching that file's own established style (ee_intc.h only
 * names the sources it has historically raised itself), since this
 * is a second, independent real trigger site for the same source. */
#define EE_INTC_IRQ_SBUS 1

static sif_state_t g_sif;

/* task #212 continuation (82nd/83rd findings): moved ahead of
 * sif_mmio_write32 (declaration-order fix, same class of bug as the
 * earlier g_bind_sid_table_* case in this file). */
static int g_iop_boot_completed_once;

/* Round 251 (task #411, 291st finding): same declaration-order fix,
 * for the same reason - sif_mmio_write32's SIF_SMFLAG case (below)
 * needs to see this before its own first use. See the citation above
 * sif_note_ee_loadexecps2_seen()'s declaration in sif.h for the full
 * grounding of why this flag exists and what real gap it closes. */
static int g_ee_loadexecps2_seen;

/* Round 441 (task #212): delayed BOOTEND/SIFINIT/CMDINIT reassertion
 * state - see the full grounding in sif_mmio_write32()'s SIF_SMFLAG
 * case below and sif_ee_tick()'s own comment. Modeled as a simple
 * countdown (ticked once per real EE instruction, same convention as
 * ee_timers_tick()/ee_check_rpcinit_pending() etc. - see ee_core.c's
 * two call sites) rather than an absolute due-instruction-count, so
 * this file does not need to depend on ee_core.h/ee_state_t at all -
 * consistent with this file's existing, deliberately narrow surface
 * (see sif.h's own header comment on scope). */
static int g_bootend_reassert_pending;
static int g_bootend_reassert_ticks_left;

/* Explicit, honestly-labeled approximation (not a claim of real SIF
 * cross-processor handshake latency, which is not cited/known) - see
 * sif_mmio_write32()'s SIF_SMFLAG case for the full reasoning. Chosen
 * to be clearly non-zero (so the EE kernel's own 0x8000CDF8 recheck,
 * which re-reads SMFLAG only a handful of instructions after its own
 * clearing write, reliably observes a genuine all-clear window) while
 * staying a small fraction of a real video frame's worth of EE
 * instructions (~4.9M at 60Hz/294MHz), so OSDSYS's own tight
 * 0x000820D0-0x000820E8 busy-wait (live-observed at ~8-10 instructions
 * per iteration) resolves within a handful of real iterations, not an
 * unbounded hang. */
#define SIF_BOOTEND_REASSERT_DELAY_TICKS 64

void sif_init(void)
{
    memset(&g_sif, 0, sizeof(g_sif));
    /* Real hardware/PCSX2 reset value for this register (Hw.cpp:
     * psHu32(SBUS_F260) = 0x1D000060). */
    g_sif.f260 = 0x1D000060u;
}

sif_state_t *sif_get_state(void) { return &g_sif; }

int sif_mmio_read32(uint32_t addr, uint32_t *out)
{
    switch (addr) {
        case SIF_MSCOM:  *out = g_sif.mscom;  return 1;
        case SIF_SMCOM:  *out = g_sif.smcom;  return 1;
        case SIF_MSFLAG: *out = g_sif.msflag; return 1;
        case SIF_SMFLAG: *out = g_sif.smflag; return 1;
        case SIF_CTRL:
            /* Real PCSX2 HwRead.cpp: return psHu32(SBUS_F240) | 0xF0000102 */
            *out = g_sif.ctrl | 0xF0000102u;
            return 1;
        case SIF_F250:   *out = g_sif.f250;   return 1;
        case SIF_F260:   *out = g_sif.f260;   return 1;
        default:
            return 0;
    }
}

int sif_mmio_write32(uint32_t addr, uint32_t value)
{
    switch (addr) {
        case SIF_MSCOM:
            /* Plain assignment - real hardware/PCSX2 has no special
             * case here beyond the default store. */
            g_sif.mscom = value;
            return 1;
        case SIF_SMCOM:
            /* Only meaningfully written from the IOP side on real
             * hardware; modeled as plain storage since there is no
             * IOP-side SIF writer yet (see header comment). */
            g_sif.smcom = value;
            return 1;
        case SIF_MSFLAG:
            /* Real PCSX2: psHu32(mem) |= value; (EE sets flag bits) */
            g_sif.msflag |= value;
            return 1;
        case SIF_SMFLAG:
            /* Real PCSX2: psHu32(mem) &= ~value; (EE clears flag bits
             * the IOP previously set - write-1-to-clear) */
            g_sif.smflag &= ~value;
            /* task #212 continuation (82nd/83rd findings): see the
             * full grounding/citation in sif.h above
             * sif_note_iop_boot_completed_once()'s declaration - real,
             * observed EE-side behavior clears SIF_STAT_BOOTEND
             * (0x40000) as part of a genuine _LoadExecPS2-triggered
             * reset/reload sequence; since this project's own IOP
             * module loader has ALREADY, for real, completed loading
             * every real ROMDIR module once (tracked via
             * sif_iop_boot_completed_once()), that real fact does not
             * become false just because the EE cleared its own status
             * flag - re-signal the same real bits
             * (mark_iop_boot_complete()'s own already-cited
             * SIF_STAT_BOOTEND | SIF_STAT_CMDINIT, plus SIF_STAT_SIFINIT
             * which this project's own task #165 fix already
             * established gets set for real via the genuine SIFMAN
             * handshake) immediately, rather than leaving OSDSYS
             * parked forever on a flag this project's own model is
             * fully entitled to consider still true. */
            if ((value & 0x00040000u) && g_iop_boot_completed_once) {
                /* Round 251 (task #411, 291st finding) ORIGINALLY
                 * added a `g_ee_loadexecps2_seen` guard here, because
                 * an unconditional, SYNCHRONOUS re-signal (performed
                 * inline, in the same write call that just cleared
                 * the flag) made SIF_SMFLAG's SIFINIT/CMDINIT/BOOTEND
                 * bits unobservably-cleared to a much earlier, entirely
                 * normal debounce-and-consume poll inside the EE
                 * kernel itself (0x8000CDF8: masks SMFLAG with
                 * 0x3FFFFFFF and only escapes once the masked result
                 * reads zero) - see the "0x8000CC68 wall"
                 * (Round 179/345/346/362) that guard fixed.
                 *
                 * Round 441 (task #212, live-verified Rounds 437-440):
                 * that guard over-corrected. It also permanently
                 * blocks the real, necessary re-signal OSDSYS's own
                 * LATER, REPEATING dispatcher needs every time it
                 * re-enters its BOOTEND wait (`0x00082220`'s
                 * unconditional call into the real poll at
                 * `0x000820C0`) - live-captured via real PCSX2:
                 * `SIF_SMFLAG` genuinely reads 0 at the exact entry to
                 * a confirmed-recurring invocation (`ra=0x800057ac`,
                 * 2.26 billion real cycles into a normally-rendering
                 * boot), meaning real hardware relies on BOOTEND being
                 * re-set again and again, indefinitely - not just once,
                 * post-_LoadExecPS2.
                 *
                 * The real, minimal distinction the original guard
                 * needed was never "has _LoadExecPS2 run" (an
                 * unrelated, much-later event) - it was "give the
                 * clearing side's own immediate re-check a genuine
                 * chance to observe zero before the flag comes back",
                 * i.e. a real, non-zero CROSS-PROCESSOR HANDSHAKE
                 * DELAY, not a same-instant same-call write. Real
                 * hardware's IOP is a physically separate processor
                 * that cannot possibly re-signal SIF_SMFLAG within the
                 * same EE bus cycle that just cleared it - modeling
                 * the re-signal as synchronous was the actual bug, not
                 * the fact that it fires at all. Deferring it by a
                 * short, explicit real-instruction-count delay (see
                 * `sif_ee_tick()` below) restores that real timing gap
                 * for 0x8000CDF8's own single-shot recheck (which
                 * reads SMFLAG again only a handful of instructions
                 * after its own clearing write, well inside this
                 * delay window) while still resolving OSDSYS's own
                 * tight busy-wait loop (`0x000820D0`-`0x000820E8`,
                 * live-observed spinning at ~8-10 instructions per
                 * iteration) within a small, bounded number of real
                 * iterations - not an indefinite hang. */
                g_bootend_reassert_pending = 1;
                g_bootend_reassert_ticks_left = SIF_BOOTEND_REASSERT_DELAY_TICKS;
            }
            return 1;
        case SIF_CTRL:
            /* Bits 18/19 (IOP IRQ / IOP reset trigger) are NOT
             * modeled - no cross-CPU wiring to iop_core.c exists yet.
             * Only the bit-0x100 lock flag, which is purely a
             * register-level readback concern, is modeled here,
             * matching PCSX2's:
             *   if (!(value & 0x100)) psHu32(mem) &= ~0x100;
             *   else                  psHu32(mem) |= 0x100;
             */
            if (!(value & 0x100u))
                g_sif.ctrl &= ~0x100u;
            else
                g_sif.ctrl |= 0x100u;
            return 1;
        case SIF_F250:
            g_sif.f250 = value;
            return 1;
        case SIF_F260:
            /* Round 264 (task #423, 304th finding): reactive real-
             * value response, same established pattern as SIF_SMFLAG
             * above (the g_iop_boot_completed_once-gated re-signal) -
             * when the EE writes its own real "not yet ready" sentinel
             * (0xFF - confirmed via fresh disassembly of the real
             * sceSifInit()-equivalent routine reaching this exact
             * write for the first time this project's boot trace has
             * ever executed it, cross-checked against this project's
             * own already-existing citation of that same routine
             * "clears SIF_F260 to 0xFF") AND the IOP's own real module
             * loading has already genuinely completed
             * (g_iop_boot_completed_once, the same real flag the
             * SMFLAG path above already uses), respond immediately
             * with the real, already-cited hardware default
             * (0x1D000060, sif_init()'s own reset value) instead of
             * leaving the temporary sentinel in place.
             *
             * Why this matters: Round 264 found a real EE kernel
             * routine (0x8000CEA8-0x8000CED4) reads this exact
             * register right after the SIF2-completion wait
             * (iop_module_loader.c's mark_iop_boot_complete() fix)
             * resolves, tests bit 2, and calls a real panic handler
             * ("bus error while dma transfer", 0x8000B9D0) if it's
             * still set - which it always was, since nothing in this
             * project ever updated F260 after the EE's own init wrote
             * the temporary 0xFF sentinel. Real hardware's IOP-side
             * SIF driver would publish its genuine register-mirror
             * address here once ready; this models that handshake's
             * end state using the register's own already-cited real
             * default, not a fabricated value - see
             * iop_module_loader.h's citation for the paired SIF2 fix
             * this depends on. Verified via host-native diagnostic:
             * without this fix, the SIF2 fix alone reproduces the
             * panic; with both, the EE proceeds cleanly into real,
             * repeated interrupt/exception handling (COP0 context
             * save/restore at 0x80010FA8-0x80011044) with no crash. */
            if (value == 0xFFu && g_iop_boot_completed_once) {
                g_sif.f260 = 0x1D000060u;
            } else {
                g_sif.f260 = value;
            }
            return 1;
        default:
            return 0;
    }
}

/* --- IOP-side mirror (0x1D000000-0x1D0000FF), plain/flat semantics -
 * see the header comment above sif_iop_mmio_read32/write32 for why
 * this deliberately does NOT replicate the EE side's OR/AND-on-write
 * special cases. */

int sif_iop_mmio_read32(uint32_t addr, uint32_t *out)
{
    /* Task #165 fix: the IOP has no MMU/TLB, so KUSEG (0x00000000-
     * 0x7FFFFFFF, direct), KSEG0 (0x80000000-0x9FFFFFFF, cached
     * mirror) and KSEG1 (0xA0000000-0xBFFFFFFF, uncached mirror) all
     * address the SAME physical location - real IOP code reaches this
     * mailbox window through any of the three (e.g. the boot-time
     * SIF_MSFLG poll loop uses the KSEG1 alias 0xBD000020, not the
     * bare 0x1D000020 this switch used to require). Mask off the
     * segment-select bits the same way iop_mem_ptr() already does for
     * plain RAM before doing the window check, instead of only
     * accepting the raw KUSEG-form address. */
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < 0x1D000000u || phys > 0x1D0000FFu)
        return 0;

    switch (phys & 0xFFu) {
        case 0x00: *out = g_sif.mscom;  return 1;
        case 0x10: *out = g_sif.smcom;  return 1;
        case 0x20: *out = g_sif.msflag; return 1;
        case 0x30: *out = g_sif.smflag; return 1;
        case 0x40: *out = g_sif.ctrl;   return 1; /* plain - the EE-side
                                                     * 0xF0000102 read
                                                     * mask is specific
                                                     * to HwRead.cpp's
                                                     * EE-side handler,
                                                     * not modeled for
                                                     * the IOP's flat
                                                     * array view. */
        case 0x50: *out = g_sif.f250;   return 1;
        case 0x60: *out = g_sif.f260;   return 1;
        default:   *out = 0;            return 1; /* rest of the
                                                     * 0x100-byte
                                                     * window: reads
                                                     * as 0, matching
                                                     * an all-zero
                                                     * backing array
                                                     * for the parts
                                                     * we don't model
                                                     * individually. */
    }
}

int sif_iop_mmio_write32(uint32_t addr, uint32_t value)
{
    /* Task #165 fix: same KUSEG/KSEG0/KSEG1-alias masking as the read
     * side above - see that function's comment for the full
     * rationale. */
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < 0x1D000000u || phys > 0x1D0000FFu)
        return 0;

    switch (phys & 0xFFu) {
        case 0x00: g_sif.mscom  = value; return 1;
        case 0x10: g_sif.smcom  = value; return 1;
        case 0x20: g_sif.msflag = value; return 1; /* plain overwrite,
                                                      * NOT an OR - see
                                                      * header comment */
        case 0x30: {
            /* Round 317 (task #423 continuation, following the
             * user's own relayed research pointing at real
             * sceSifSetSMFlag() behavior): real hardware's IOP-side
             * sceSifSetSMFlag() does not just store a value into
             * SMFLAG - it is THE real mechanism that raises the EE's
             * SBUS interrupt (EE_INTC_STAT bit 1), confirmed via
             * ps2sdk/community documentation ("sceSifSetSMFlag...
             * triggering the appropriate SBUS interrupt on the
             * receiving side" - the IOP kernel's own SIF1 handling
             * depends on this exact interrupt firing to know the EE
             * has been signaled). This project's real, already-cited
             * mark_iop_boot_complete() (iop_module_loader.c) already
             * performs the real SMFLAG write this event represents
             * (SIF_STAT_SIFINIT/CMDINIT/BOOTEND, citing real ps2sdk
             * SIFMAN/SIFCMD init behavior) through this exact real
             * MMIO entry point (source/core/iop/iop_core.c's generic
             * IOP MMIO dispatcher) - it just never propagated to the
             * EE-visible interrupt line before now: a genuine,
             * previously-missing piece of already-modeled real
             * behavior, not a new fabrication.
             *
             * Raised only on a genuine 0->1 transition (new bits
             * appearing that were not already set) rather than on
             * every write, matching real hardware's "a new signal has
             * arrived" semantics and avoiding a spurious re-raise on
             * a write that only restates already-pending bits. */
            uint32_t old_smflag = g_sif.smflag;
            g_sif.smflag = value;
            if ((value & ~old_smflag) != 0u)
                ee_intc_raise(EE_INTC_IRQ_SBUS);
            return 1;
        }
        case 0x40: g_sif.ctrl   = value; return 1;
        case 0x50: g_sif.f250   = value; return 1;
        case 0x60: g_sif.f260   = value; return 1;
        default:   return 1; /* rest of the window: accepted, discarded -
                               * matches an unmodeled part of a flat
                               * backing array. */
    }
}

/* --- task #186: minimal IOP-side SIFCMD consumer model - see the
 * comment block in sif.h above sif_cmd_iop_handle_init_cmd() for full
 * grounding, scope and honest caveats. */

static uint32_t g_iop_cmd_ee_recvbuf;
static uint32_t g_iop_cmd_init_cmd_count;

/* task #192 (68th finding): tracks the "cd" (SifRpcClientData_t*)
 * pointer from the real, observed SIF_CMD_RPC_BIND packet, so a
 * synthetic REND reply can echo it back exactly - see sif.h. */
static uint32_t g_iop_cmd_rpc_bind_cd;
static uint32_t g_iop_cmd_rpc_bind_count;

/* task #202 (79th finding): small fixed-size cd_ptr->sid table - see
 * the citation in sif.h above sif_cmd_iop_track_bind_sid()'s
 * declaration for the full real-protocol grounding. */
#define SIF_CMD_BIND_SID_TABLE_SIZE 8
static uint32_t g_bind_sid_table_cd[SIF_CMD_BIND_SID_TABLE_SIZE];
static uint32_t g_bind_sid_table_sid[SIF_CMD_BIND_SID_TABLE_SIZE];
static uint32_t g_bind_sid_table_next;

/* task #212 continuation (82nd/83rd findings) - see the full
 * grounding/citation in sif.h above sif_note_iop_boot_completed_once()'s
 * declaration. g_iop_boot_completed_once itself is declared earlier in
 * this file (near g_sif) so sif_mmio_write32's SIF_SMFLAG case, which is
 * defined before this point, can see it - same fix pattern already
 * applied once before in this file for g_bind_sid_table_*. */

void sif_note_iop_boot_completed_once(void)
{
    g_iop_boot_completed_once = 1;
}

int sif_iop_boot_completed_once(void)
{
    return g_iop_boot_completed_once;
}

/* Round 251 (task #411, 291st finding) - see sif.h for full grounding. */
void sif_note_ee_loadexecps2_seen(void)
{
    g_ee_loadexecps2_seen = 1;
}

int sif_ee_loadexecps2_seen(void)
{
    return g_ee_loadexecps2_seen;
}

/* Round 441 (task #212): fires the BOOTEND/SIFINIT/CMDINIT reassertion
 * scheduled by sif_mmio_write32()'s SIF_SMFLAG case, after a real,
 * explicit delay (SIF_BOOTEND_REASSERT_DELAY_TICKS) rather than
 * synchronously in the same write call - see that case's own comment
 * for the full reasoning. Must be called once per real EE instruction
 * step, same established convention as ee_timers_tick()/
 * ee_check_rpcinit_pending()/ee_check_rpc_bind_pending() etc. - see
 * ee_core.c's two call sites (both existing per-instruction epilogue
 * points, matching this project's own precedent for this class of
 * "must still fire even mid-branch-delay-slot, real hardware doesn't
 * skip a beat" tick function). */
void sif_ee_tick(void)
{
    if (!g_bootend_reassert_pending)
        return;
    if (--g_bootend_reassert_ticks_left > 0)
        return;
    g_bootend_reassert_pending = 0;
    g_sif.smflag |= 0x00010000u /* SIF_STAT_SIFINIT */
                  |  0x00020000u /* SIF_STAT_CMDINIT */
                  |  0x00040000u /* SIF_STAT_BOOTEND */;
}

void sif_cmd_iop_init(void)
{
    uint32_t i;
    g_iop_cmd_ee_recvbuf = 0;
    g_iop_cmd_init_cmd_count = 0;
    g_iop_cmd_rpc_bind_cd = 0;
    g_iop_cmd_rpc_bind_count = 0;
    for (i = 0; i < SIF_CMD_BIND_SID_TABLE_SIZE; i++) {
        g_bind_sid_table_cd[i] = 0;
        g_bind_sid_table_sid[i] = 0;
    }
    g_bind_sid_table_next = 0;
    g_iop_boot_completed_once = 0;
    g_ee_loadexecps2_seen = 0;
    g_bootend_reassert_pending = 0;
    g_bootend_reassert_ticks_left = 0;
}

void sif_cmd_iop_handle_init_cmd(uint32_t ee_recvbuf_addr)
{
    g_iop_cmd_ee_recvbuf = ee_recvbuf_addr;
    g_iop_cmd_init_cmd_count++;
}

uint32_t sif_cmd_iop_get_ee_recvbuf(void)
{
    return g_iop_cmd_ee_recvbuf;
}

uint32_t sif_cmd_iop_get_init_cmd_count(void)
{
    return g_iop_cmd_init_cmd_count;
}

void sif_cmd_iop_handle_rpc_bind(uint32_t cd_ptr)
{
    g_iop_cmd_rpc_bind_cd = cd_ptr;
    g_iop_cmd_rpc_bind_count++;
}

uint32_t sif_cmd_iop_get_rpc_bind_cd(void)
{
    return g_iop_cmd_rpc_bind_cd;
}

uint32_t sif_cmd_iop_get_rpc_bind_count(void)
{
    return g_iop_cmd_rpc_bind_count;
}

void sif_cmd_iop_track_bind_sid(uint32_t cd_ptr, uint32_t sid)
{
    uint32_t i;
    for (i = 0; i < SIF_CMD_BIND_SID_TABLE_SIZE; i++) {
        if (g_bind_sid_table_cd[i] == cd_ptr) {
            g_bind_sid_table_sid[i] = sid;
            return;
        }
    }
    g_bind_sid_table_cd[g_bind_sid_table_next] = cd_ptr;
    g_bind_sid_table_sid[g_bind_sid_table_next] = sid;
    g_bind_sid_table_next = (g_bind_sid_table_next + 1u) % SIF_CMD_BIND_SID_TABLE_SIZE;
}

uint32_t sif_cmd_iop_lookup_bind_sid(uint32_t cd_ptr)
{
    uint32_t i;
    for (i = 0; i < SIF_CMD_BIND_SID_TABLE_SIZE; i++) {
        if (g_bind_sid_table_cd[i] == cd_ptr)
            return g_bind_sid_table_sid[i];
    }
    return 0u;
}
