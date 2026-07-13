/*
 * sif.c - EE-side SIF/SBUS mailbox register model. See sif.h for the
 * exact per-register semantics and PCSX2 source cross-reference.
 */
#include "core/hw/sif.h"
#include <string.h>

#define SIF_MSCOM  0x1000F200u
#define SIF_SMCOM  0x1000F210u
#define SIF_MSFLAG 0x1000F220u
#define SIF_SMFLAG 0x1000F230u
#define SIF_CTRL   0x1000F240u
#define SIF_F250   0x1000F250u
#define SIF_F260   0x1000F260u

static sif_state_t g_sif;

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
            g_sif.f260 = value;
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
        case 0x30: g_sif.smflag = value; return 1; /* plain overwrite,
                                                      * NOT an AND-clear */
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

void sif_cmd_iop_init(void)
{
    g_iop_cmd_ee_recvbuf = 0;
    g_iop_cmd_init_cmd_count = 0;
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
