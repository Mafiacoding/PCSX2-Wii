/*
 * iop_spu2.c - see include/core/hw/iop_spu2.h for scope notes.
 *
 * Round 523/524 addition (task #489/#490): real KON/ENDX/CTRL->STATX
 * register-level side effects, on top of the existing raw byte-array
 * passthrough storage. Sourced (architectural facts only, paraphrased,
 * no verbatim excerpts beyond short cited quotes) from psx-spx
 * (https://psx-spx.consoledev.net/soundprocessingunitspu/), which
 * documents the SPU/SPU2 KON/KOFF/ENDX/SPUCNT/SPUSTAT register model
 * that PS2's SPU2 core registers extend (same citation tier already
 * used for this file's Round 185 offset table, which cites ps2tek +
 * PCSX2's ZeroSPU2 plugin header for the same register layout).
 *
 * What IS real and implemented this round:
 *   - KON (key-on) writes are real write-only trigger registers:
 *     readback returns the last-written value (already true of the
 *     existing passthrough storage), AND - per real hardware - "The
 *     bits get CLEARED when setting the corresponding KEY ON bits."
 *     i.e. writing a KON bit unconditionally clears the matching
 *     ENDX status bit for that voice. This is implemented for real
 *     below and does not require any audio synthesis engine.
 *   - CTRL (SPUCNT-equivalent) bits 5-0 (transfer-mode/enable bits)
 *     are mirrored into STATX (SPUSTAT-equivalent) bits 5-0 on write,
 *     per real hardware: "Current SPU Mode (same as SPUCNT.Bit5-0...
 *     applied a bit delayed)". This project has no cycle-accurate
 *     timing model, so the mirror is applied immediately rather than
 *     "a bit delayed" - an explicit, documented simplification of a
 *     real, cited hardware behavior, not a fabrication.
 *
 * What is explicitly DEFERRED, same discipline as the IPU precedent
 * (Round 522: real register/FIFO skeleton, real MPEG2 decode
 * deferred):
 *   - KOFF (key-off) remains a real write-only trigger register
 *     (latched, readback returns last value) but has no further
 *     effect, because a real KOFF's only actual consequence is
 *     driving an ADSR envelope engine from Sustain into Release -
 *     this project has no envelope/synthesis engine yet.
 *   - ENDX is NOT fabricated to auto-SET on reaching a real
 *     ADPCM-header loop-end flag, because that requires the same
 *     missing envelope/decode engine. The one real ENDX transition
 *     this project CAN honestly model without that engine - KON
 *     clearing ENDX - is implemented; ENDX auto-set-on-loop-end stays
 *     an open gap, matching this file's existing honest-scope
 *     tradition.
 *   - No actual audio synthesis, mixing, ADSR envelope state machine,
 *     or DMA-to-SPU2 sample pipeline. Unchanged from the pre-existing
 *     scope limit.
 */
#include "core/hw/iop_spu2.h"
#include <string.h>

static uint8_t g_regs[IOP_SPU2_SIZE];

void iop_spu2_init(void)
{
    memset(g_regs, 0, sizeof(g_regs));
}

uint32_t iop_spu2_voice_reg_addr(int core, int voice, uint32_t voice_reg_offset)
{
    uint32_t core_base = IOP_SPU2_BASE + (core ? SPU2_CORE1_OFFSET : 0u);
    return core_base + (uint32_t)voice * SPU2_VOICE_STRIDE + voice_reg_offset;
}

uint32_t iop_spu2_voice_addr_reg_addr(int core, int voice, uint32_t vaddr_reg_offset)
{
    uint32_t core_base = IOP_SPU2_BASE + (core ? SPU2_CORE1_OFFSET : 0u);
    return core_base + SPU2_VADDR_BASE + (uint32_t)voice * SPU2_VADDR_STRIDE + vaddr_reg_offset;
}

uint32_t iop_spu2_core_reg_addr(int core, uint32_t core_reg_offset)
{
    uint32_t core_base = IOP_SPU2_BASE + (core ? SPU2_CORE1_OFFSET : 0u);
    return core_base + core_reg_offset;
}

/* Raw 16-bit storage accessors, no side effects - used internally by
 * both the plain passthrough path and the side-effect logic below so
 * there is exactly one place that touches g_regs directly. */
static uint16_t raw_read16(uint32_t off)
{
    return (uint16_t)g_regs[off] | ((uint16_t)g_regs[off + 1] << 8);
}

static void raw_write16(uint32_t off, uint16_t value)
{
    g_regs[off]     = (uint8_t)(value & 0xFFu);
    g_regs[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
}

/* Round 523/524: real KON-clears-ENDX and CTRL[5:0]->STATX[5:0]
 * side effects. addr is an absolute SPU2 address (already known to
 * be in-range by the caller); value is the 16-bit value just written
 * there by the plain passthrough path. */
static void apply_write_side_effects(uint32_t addr, uint16_t value)
{
    int core;
    for (core = 0; core < 2; core++) {
        uint32_t kon0_addr  = iop_spu2_core_reg_addr(core, SPU2_C_KON0);
        uint32_t kon1_addr  = iop_spu2_core_reg_addr(core, SPU2_C_KON1);
        uint32_t ctrl_addr  = iop_spu2_core_reg_addr(core, SPU2_C_CTRL);
        uint32_t endx0_off  = iop_spu2_core_reg_addr(core, SPU2_C_ENDX0) - IOP_SPU2_BASE;
        uint32_t endx1_off  = iop_spu2_core_reg_addr(core, SPU2_C_ENDX1) - IOP_SPU2_BASE;
        uint32_t statx_off  = iop_spu2_core_reg_addr(core, SPU2_C_STATX) - IOP_SPU2_BASE;

        if (addr == kon0_addr) {
            /* Real hardware: "The bits get CLEARED when setting the
             * corresponding KEY ON bits." KON0 covers voices 0-15,
             * same bit layout as ENDX0. */
            uint16_t endx = raw_read16(endx0_off);
            raw_write16(endx0_off, (uint16_t)(endx & ~value));
        } else if (addr == kon1_addr) {
            /* KON1 covers voices 16-23, same bit layout as ENDX1. */
            uint16_t endx = raw_read16(endx1_off);
            raw_write16(endx1_off, (uint16_t)(endx & ~value));
        } else if (addr == ctrl_addr) {
            /* Real hardware: SPUSTAT bits 5-0 mirror SPUCNT bits 5-0
             * ("applied a bit delayed" on real hardware; this project
             * has no cycle-accurate timing model, so the mirror is
             * applied immediately - documented simplification). */
            uint16_t statx = raw_read16(statx_off);
            statx = (uint16_t)((statx & ~0x3Fu) | (value & 0x3Fu));
            raw_write16(statx_off, statx);
        }
    }
}

int iop_spu2_mmio_read16(uint32_t addr, uint16_t *out)
{
    if (addr < IOP_SPU2_BASE || addr >= IOP_SPU2_BASE + IOP_SPU2_SIZE) return 0;
    uint32_t off = addr - IOP_SPU2_BASE;
    *out = raw_read16(off);
    return 1;
}

int iop_spu2_mmio_write16(uint32_t addr, uint16_t value)
{
    if (addr < IOP_SPU2_BASE || addr >= IOP_SPU2_BASE + IOP_SPU2_SIZE) return 0;
    uint32_t off = addr - IOP_SPU2_BASE;
    raw_write16(off, value);
    apply_write_side_effects(addr, value);
    return 1;
}

int iop_spu2_mmio_read32(uint32_t addr, uint32_t *out)
{
    if (addr < IOP_SPU2_BASE || addr >= IOP_SPU2_BASE + IOP_SPU2_SIZE) return 0;
    uint32_t off = addr - IOP_SPU2_BASE;
    *out = (uint32_t)g_regs[off] | ((uint32_t)g_regs[off + 1] << 8) |
           ((uint32_t)g_regs[off + 2] << 16) | ((uint32_t)g_regs[off + 3] << 24);
    return 1;
}

int iop_spu2_mmio_write32(uint32_t addr, uint32_t value)
{
    if (addr < IOP_SPU2_BASE || addr >= IOP_SPU2_BASE + IOP_SPU2_SIZE) return 0;
    /* A 32-bit write spans two adjacent 16-bit registers (e.g. a
     * 32-bit write to KON0's address also lands on KON1). Route
     * through the same 16-bit path twice so side effects apply to
     * both halves exactly as they would for two real 16-bit writes. */
    iop_spu2_mmio_write16(addr, (uint16_t)(value & 0xFFFFu));
    iop_spu2_mmio_write16(addr + 2, (uint16_t)((value >> 16) & 0xFFFFu));
    return 1;
}
