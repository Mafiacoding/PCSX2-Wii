/*
 * iop_icfg.c - IOP ICFG register model. See iop_icfg.h for the full
 * real-hardware citation trail (PCSX2's IopHwWrite.cpp `case 0x450:`
 * / ps2sdk's GM_IF).
 */
#include "core/hw/iop_icfg.h"
#include "core/hw/ee_intc.h"

#define IOP_ICFG_ADDR 0x1F801450u

/* Real EE INTC source index 1 = INTC_SBUS (PCSX2's Dmac.h enum
 * INTCIrqs / ps2sdk's kernel.h identical list) - see ee_intc.h's own
 * "EE_INTC_IRQ_VBLANK_START/END" precedent for the same style of
 * real, cited source-index constant. Not defined in ee_intc.h itself
 * since ee_intc.h intentionally only names the two sources it has
 * historically raised (VBLANK) - defined locally here since this is
 * the first caller of the SBUS source specifically. */
#define EE_INTC_IRQ_SBUS 1

static uint32_t g_icfg;

void iop_icfg_init(void)
{
    g_icfg = 0;
}

int iop_icfg_mmio_read32(uint32_t addr, uint32_t *out)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys != IOP_ICFG_ADDR)
        return 0;
    *out = g_icfg;
    return 1;
}

int iop_icfg_mmio_write32(uint32_t addr, uint32_t value)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys != IOP_ICFG_ADDR)
        return 0;

    g_icfg = value;

    /* Real PCSX2 (IopHwWrite.cpp, case 0x450): "if (val & (1 << 1))
     * hwIntcIrq(INTC_SBUS);" - raise the EE's real INTC_SBUS source
     * whenever the IOP writes a value with bit 1 set here. */
    if (value & 0x2u)
        ee_intc_raise(EE_INTC_IRQ_SBUS);

    return 1;
}
