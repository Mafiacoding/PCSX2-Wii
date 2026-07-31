/*
 * mch.h - EE-side "MCH" (Memory Controller Hub) RDRAM auto-init
 * registers, MCH_RICM (0x1000F430) and MCH_DRD (0x1000F440).
 *
 * Real PCSX2's own comment on this area (pcsx2/Hw.h): "MCH area --
 * Really not sure what this area is. Information is lacking." Despite
 * that, the exact read/write logic PCSX2 (and other emulators) use to
 * satisfy the BIOS's RDRAM auto-detection sequence is well documented
 * (PS2Tek, "EE RDRAM initialization" - see docs/STATUS.md's "round 11"
 * section for the live-trace investigation that led here). Ported
 * directly from that reference, not reinvented:
 *
 * The BIOS writes a "command" to MCH_RICM (SA = bits 16-27 select
 * which sub-operation, SBC = bits 6-9), then reads MCH_DRD to get the
 * result of that command. This models a real Direct RDRAM
 * auto-initialization protocol (enumerating installed RDRAM devices
 * by SDEVID) - PS2 retail hardware has 2 RDRAM devices, so the BIOS's
 * detection loop expects to successfully enumerate SDEVID 0 and 1
 * before concluding RDRAM is present and moving on.
 *
 *   0x1000F430 MCH_RICM - write: SA=(data>>16)&0xFFF, SBC=(data>>6)&0xF;
 *                         if SA==0x21 && SBC==0x1 && !((MCH_DRD>>7)&1),
 *                         reset the SDEVID enumeration counter to 0.
 *                         Store data&~0x80000000 as the new MCH_RICM
 *                         value (bit 31 is a real "busy/strobe" bit
 *                         the BIOS sets on write and expects hardware
 *                         to clear - modeled as instantly clear, which
 *                         is what the reference logic does too).
 *                         Read: always returns 0.
 *   0x1000F440 MCH_DRD   - write: plain store (the BIOS never reads
 *                         back what it wrote here directly - only
 *                         through the MCH_RICM-command-decoded read
 *                         below).
 *                         Read: decode SOP=(MCH_RICM>>6)&0xF and
 *                         SA=(MCH_RICM>>16)&0xFFF from the last
 *                         MCH_RICM write. If SOP==0: SA==0x21 (INIT) -
 *                         if sdevid_counter < 2, increment it and
 *                         return 0x1F (a device responded); else
 *                         return 0 (no more devices). SA==0x23 (CNFGA)
 *                         returns 0x0D0D. SA==0x24 (CNFGB) returns
 *                         0x0090. SA==0x40 (SET_DEVICE_ID or similar)
 *                         returns MCH_RICM&0x1F. Anything else: 0.
 *
 * This is the root cause of the round 10 "wrong branch at
 * pc=0xBFC0088C" investigation: this project previously modeled
 * neither register (both fell through to the generic reads-as-zero
 * path), so the BIOS's SDEVID-enumeration loop saw MCH_DRD always
 * read 0 instead of 0x1F, failed its "did we actually enumerate at
 * least 2 devices" sanity check on the very first read, and returned
 * an error sentinel that sent the whole boot path down the wrong
 * branch - see docs/STATUS.md's "round 10"/"round 11" sections.
 */
#ifndef PCSX2WII_CORE_HW_MCH_H
#define PCSX2WII_CORE_HW_MCH_H

#include <stdint.h>

typedef struct mch_state_t {
    uint32_t ricm;          /* last value written to MCH_RICM (masked, bit 31 cleared) */
    uint32_t drd;           /* last value written to MCH_DRD (plain storage) */
    uint32_t sdevid_counter; /* RDRAM SDEVID enumeration progress, reset via MCH_RICM writes */
} mch_state_t;

void mch_init(void);

/* Returns 1 and fills *out if addr is MCH_RICM/MCH_DRD, 0 otherwise -
 * same convention as dma_mmio_read32/sif_mmio_read32. */
int mch_mmio_read32(uint32_t addr, uint32_t *out);
int mch_mmio_write32(uint32_t addr, uint32_t value);

mch_state_t *mch_get_state(void);

#endif
