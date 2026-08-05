/*
 * iop_spu_legacy.c - see include/core/hw/iop_spu_legacy.h for scope
 * notes and full citation trail (Round 136, 177th finding).
 *
 * Round 523/524 addition (task #490): real KON->ENDX-clear and
 * CTRL[5:0]->STATUS[5:0] mirror side effects, on top of the existing
 * raw byte-array passthrough storage - same real, cited semantics
 * (psx-spx) and same honest scope split (real trigger/status register
 * behavior implemented; no ADSR envelope engine, no audio synthesis,
 * no fabricated ENDX-set-on-loop-end) as this round's sibling change
 * to iop_spu2.c. See that file's header comment for the full citation
 * and scope writeup - identical reasoning applies here since this is
 * the same real register model (PS1 SPU) that PS2's SPU2 extends.
 *
 * KON/KOFF/ENDX are real 32-bit-wide fields (psx-spx: "Voice 0..23
 * Flags (six 1bit flags per voice)", bits 0-23 = voices 0-23, one
 * register per flag type - unlike most other SPU registers, which
 * are 16-bit). This project's existing 16-bit-primary accessors treat
 * such a 32-bit field as two adjacent 16-bit halves; the low half
 * naturally covers voices 0-15 and the high half's low byte covers
 * voices 16-23, so the side-effect logic below reuses the exact same
 * "low half / high half" split already used for iop_spu2.c's
 * KON0/KON1 vs ENDX0/ENDX1 (just applied to a single register pair
 * instead of per-core register pairs, since this legacy block has no
 * Core0/Core1 split).
 */
#include "core/hw/iop_spu_legacy.h"
#include <string.h>

static uint8_t g_regs[IOP_SPU_LEGACY_SIZE];

void iop_spu_legacy_init(void)
{
    memset(g_regs, 0, sizeof(g_regs));
}

static uint16_t raw_read16(uint32_t off)
{
    return (uint16_t)g_regs[off] | ((uint16_t)g_regs[off + 1] << 8);
}

static void raw_write16(uint32_t off, uint16_t value)
{
    g_regs[off]     = (uint8_t)(value & 0xFFu);
    g_regs[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
}

/* Round 523/524: real KON-clears-ENDX (per psx-spx: "The bits get
 * CLEARED when setting the corresponding KEY ON bits") and real
 * CTRL[5:0]->STATUS[5:0] mirror (per psx-spx: "Current SPU Mode
 * (same as SPUCNT.Bit5-0... applied a bit delayed)" - applied
 * immediately here since this project has no cycle-accurate timing
 * model, a documented simplification of a real, cited behavior).
 * off is the register-file offset (from IOP_SPU_LEGACY_BASE) of the
 * 16-bit half just written; value is what was written there. */
static void apply_write_side_effects(uint32_t off, uint16_t value)
{
    if (off == SPU_LEGACY_KON) {
        uint16_t endx = raw_read16(SPU_LEGACY_ENDX);
        raw_write16(SPU_LEGACY_ENDX, (uint16_t)(endx & ~value));
    } else if (off == SPU_LEGACY_KON + 2u) {
        uint16_t endx = raw_read16(SPU_LEGACY_ENDX + 2u);
        raw_write16(SPU_LEGACY_ENDX + 2u, (uint16_t)(endx & ~value));
    } else if (off == SPU_LEGACY_CTRL) {
        uint16_t status = raw_read16(SPU_LEGACY_STATUS);
        status = (uint16_t)((status & ~0x3Fu) | (value & 0x3Fu));
        raw_write16(SPU_LEGACY_STATUS, status);
    }
}

int iop_spu_legacy_mmio_read16(uint32_t addr, uint16_t *out)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_SPU_LEGACY_BASE || phys + 2u > IOP_SPU_LEGACY_BASE + IOP_SPU_LEGACY_SIZE)
        return 0;
    uint32_t off = phys - IOP_SPU_LEGACY_BASE;
    *out = raw_read16(off);
    return 1;
}

int iop_spu_legacy_mmio_write16(uint32_t addr, uint16_t value)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < IOP_SPU_LEGACY_BASE || phys + 2u > IOP_SPU_LEGACY_BASE + IOP_SPU_LEGACY_SIZE)
        return 0;
    uint32_t off = phys - IOP_SPU_LEGACY_BASE;
    raw_write16(off, value);
    apply_write_side_effects(off, value);
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
    /* Route through the 16-bit path twice, same reasoning as
     * iop_spu2.c's write32: a 32-bit write spans two adjacent 16-bit
     * halves, and side effects must apply to both exactly as they
     * would for two real 16-bit writes. */
    iop_spu_legacy_mmio_write16(addr, (uint16_t)(value & 0xFFFFu));
    iop_spu_legacy_mmio_write16(addr + 2u, (uint16_t)((value >> 16) & 0xFFFFu));
    return 1;
}
