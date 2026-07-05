/*
 * mch.c - EE-side MCH_RICM/MCH_DRD (RDRAM auto-init) register model.
 * See mch.h for the exact per-register semantics and the PS2Tek/
 * PCSX2 source cross-reference this was ported from.
 */
#include "core/hw/mch.h"
#include <string.h>

#define MCH_RICM 0x1000F430u
#define MCH_DRD  0x1000F440u

/* Real PS2 retail hardware has 2 RDRAM devices - the BIOS's detection
 * loop expects to enumerate exactly this many SDEVIDs before it's
 * satisfied. Matches PCSX2's own `rdram_devices` constant. */
#define MCH_RDRAM_DEVICES 2

static mch_state_t g_mch;

void mch_init(void)
{
    memset(&g_mch, 0, sizeof(g_mch));
}

mch_state_t *mch_get_state(void) { return &g_mch; }

int mch_mmio_read32(uint32_t addr, uint32_t *out)
{
    switch (addr) {
        case MCH_RICM:
            /* Real hardware/PCSX2: MCH_RICM always reads back 0 - the
             * BIOS only ever reads results back through MCH_DRD. */
            *out = 0;
            return 1;
        case MCH_DRD: {
            uint32_t sop = (g_mch.ricm >> 6) & 0xFu;
            uint32_t sa  = (g_mch.ricm >> 16) & 0xFFFu;
            uint32_t val = 0;
            if (sop == 0) {
                switch (sa) {
                    case 0x21: /* INIT: enumerate one RDRAM SDEVID per read */
                        if (g_mch.sdevid_counter < MCH_RDRAM_DEVICES) {
                            g_mch.sdevid_counter++;
                            val = 0x1F;
                        } else {
                            val = 0;
                        }
                        break;
                    case 0x23: /* CNFGA */
                        val = 0x0D0D;
                        break;
                    case 0x24: /* CNFGB */
                        val = 0x0090;
                        break;
                    case 0x40: /* device-id readback */
                        val = g_mch.ricm & 0x1Fu;
                        break;
                    default:
                        val = 0;
                        break;
                }
            }
            *out = val;
            return 1;
        }
        default:
            return 0;
    }
}

int mch_mmio_write32(uint32_t addr, uint32_t value)
{
    switch (addr) {
        case MCH_RICM: {
            uint32_t sa  = (value >> 16) & 0xFFFu;
            uint32_t sbc = (value >> 6) & 0xFu;
            /* SA==0x21 (INIT) + SBC==0x1 (strobe), gated on MCH_DRD's
             * bit 7 being clear, restarts the SDEVID enumeration from
             * device 0 - matches the PS2Tek/PCSX2 reference exactly. */
            if (sa == 0x21u && sbc == 0x1u && ((g_mch.drd >> 7) & 1u) == 0u)
                g_mch.sdevid_counter = 0;
            /* Bit 31 is a real busy/strobe bit the BIOS sets on write
             * and expects cleared - modeled as instantly clear, same
             * as the reference logic (masked off before storing). */
            g_mch.ricm = value & ~0x80000000u;
            return 1;
        }
        case MCH_DRD:
            g_mch.drd = value;
            return 1;
        default:
            return 0;
    }
}
