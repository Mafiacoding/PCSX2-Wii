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
