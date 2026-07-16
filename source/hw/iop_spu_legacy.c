/*
 * iop_spu_legacy.c - see include/core/hw/iop_spu_legacy.h for scope
 * notes and full citation trail (Round 136, 177th finding).
 */
#include "core/hw/iop_spu_legacy.h"
#include <string.h>

static uint8_t g_regs[IOP_SPU_LEGACY_SIZE];

void iop_spu_legacy_init(void)
{
    memset(g_regs, 0, sizeof(g_regs));
}

int iop_spu_legacy_mmio_read16(uint32_t addr, uint16_t *out)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_SPU_LEGACY_BASE || phys + 2u > IOP_SPU_LEGACY_BASE + IOP_SPU_LEGACY_SIZE)
        return 0;
    uint32_t off = phys - IOP_SPU_LEGACY_BASE;
    *out = (uint16_t)g_regs[off] | ((uint16_t)g_regs[off + 1] << 8);
    return 1;
}

int iop_spu_legacy_mmio_write16(uint32_t addr, uint16_t value)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_SPU_LEGACY_BASE || phys + 2u > IOP_SPU_LEGACY_BASE + IOP_SPU_LEGACY_SIZE)
        return 0;
    uint32_t off = phys - IOP_SPU_LEGACY_BASE;
    g_regs[off] = (uint8_t)(value & 0xFFu);
    g_regs[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
    return 1;
}

int iop_spu_legacy_mmio_read32(uint32_t addr, uint32_t *out)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_SPU_LEGACY_BASE || phys + 4u > IOP_SPU_LEGACY_BASE + IOP_SPU_LEGACY_SIZE)
        return 0;
    uint32_t off = phys - IOP_SPU_LEGACY_BASE;
    *out = (uint32_t)g_regs[off] | ((uint32_t)g_regs[off + 1] << 8) |
           ((uint32_t)g_regs[off + 2] << 16) | ((uint32_t)g_regs[off + 3] << 24);
    return 1;
}

int iop_spu_legacy_mmio_write32(uint32_t addr, uint32_t value)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_SPU_LEGACY_BASE || phys + 4u > IOP_SPU_LEGACY_BASE + IOP_SPU_LEGACY_SIZE)
        return 0;
    uint32_t off = phys - IOP_SPU_LEGACY_BASE;
    g_regs[off]     = (uint8_t)(value & 0xFFu);
    g_regs[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
    g_regs[off + 2] = (uint8_t)((value >> 16) & 0xFFu);
    g_regs[off + 3] = (uint8_t)((value >> 24) & 0xFFu);
    return 1;
}
