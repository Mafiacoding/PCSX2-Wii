/*
 * iop_sio2.c - see include/core/hw/iop_sio2.h for scope notes and
 * full citation trail (Round 135, 175th finding).
 */
#include "core/hw/iop_sio2.h"
#include <string.h>

static uint8_t g_regs[IOP_SIO2_SIZE];

void iop_sio2_init(void)
{
    /* Real, cited reset state: all-zero (see header - CTRL's own
     * documented reset bits, no evidence of any other real power-on
     * default for this block). */
    memset(g_regs, 0, sizeof(g_regs));
}

int iop_sio2_mmio_read8(uint32_t addr, uint8_t *out)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_SIO2_BASE || phys >= IOP_SIO2_BASE + IOP_SIO2_SIZE)
        return 0;
    *out = g_regs[phys - IOP_SIO2_BASE];
    return 1;
}

int iop_sio2_mmio_write8(uint32_t addr, uint8_t value)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_SIO2_BASE || phys >= IOP_SIO2_BASE + IOP_SIO2_SIZE)
        return 0;
    g_regs[phys - IOP_SIO2_BASE] = value;
    return 1;
}

int iop_sio2_mmio_read32(uint32_t addr, uint32_t *out)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_SIO2_BASE || phys + 4u > IOP_SIO2_BASE + IOP_SIO2_SIZE)
        return 0;
    uint32_t off = phys - IOP_SIO2_BASE;
    *out = (uint32_t)g_regs[off] | ((uint32_t)g_regs[off + 1] << 8) |
           ((uint32_t)g_regs[off + 2] << 16) | ((uint32_t)g_regs[off + 3] << 24);
    return 1;
}

int iop_sio2_mmio_write32(uint32_t addr, uint32_t value)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_SIO2_BASE || phys + 4u > IOP_SIO2_BASE + IOP_SIO2_SIZE)
        return 0;
    uint32_t off = phys - IOP_SIO2_BASE;
    g_regs[off]     = (uint8_t)(value & 0xFFu);
    g_regs[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
    g_regs[off + 2] = (uint8_t)((value >> 16) & 0xFFu);
    g_regs[off + 3] = (uint8_t)((value >> 24) & 0xFFu);
    return 1;
}
