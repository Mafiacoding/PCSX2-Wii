/* ee_dve.h - EE-side "ba0" DVE (Digital Video Encoder) register
 * stub, physical range 0x1A000000-0x1A0000FF (KSEG1 mirror
 * 0xBA000000-0xBA0000FF).
 *
 * Round 950 (task #447/#536/#887, user-directed pivot to SCPH-50004
 * v1.90 EUR BIOS): a fresh diskless boot survey against the newly
 * uploaded SCPH-50004 BIOS found the EE parked, organically and very
 * early in real EE-kernel boot (before instr=25,000,000, well before
 * any SIF/OSDSYS activity), in a tight polling loop at
 * 0x80007CD8-0x80007CEC:
 *
 *   lhu  v0, 0(a0)          ; a0 = 0xBA000006
 *   ... (4 nops)
 *   beq  v0, zero, back-to-lhu
 *
 * Backward disassembly of the setup code immediately before the loop
 * (0x80007ca0-0x80007cd4) shows a real 3-register command sequence:
 * write 0x42 ("Read Mode") to 0xBA000002, write a sub-register index
 * to 0xBA000010, write 0x81 (Start-Execute bit 0x80 | size=1) to
 * 0xBA000000, then poll 0xBA000006 for a "ready" bit.
 *
 * This project had ZERO register model for this physical range before
 * this round (grepped 0/0 hits for "0x1A00"/"0xBA00" in source/
 * include/ and in docs/STATUS.md) - reads silently returned 0 forever,
 * so the EE kernel could never observe the "ready" bit and the boot
 * never progressed past this point on the SCPH-50004 image. This is a
 * DIFFERENT, EARLIER blocker than the SIF/OSDSYS housekeeping-loop
 * wall documented for SCPH-10000 in Rounds 943-949 - the two BIOS
 * revisions get stuck for unrelated reasons.
 *
 * The real hardware/protocol identity was confirmed, not guessed, by
 * reading the actual real PCSX2 emulator's own implementation
 * (pcsx2-master.zip, already present in this project's uploads/ from
 * Round 543's "incorporate pcsx2-master reference source" task):
 * pcsx2/Memory.cpp's ba0R16()/ba0W16() functions (search hit on
 * "0x1a000006" and "0xba00" in that file), with PCSX2's own code
 * comment: "These regs are related to DEV9 and DVE stuff, we don't
 * have to go crazy with this, but this sucks less than the original
 * code." PCSX2 itself only stubs this out (not a full real hardware
 * model) - this port replicates that same stub faithfully, including
 * its exact self-clocking "ready after 3 polls" timing and its
 * `mem & 0x1F` fallback-addressing quirk for non-special offsets,
 * rather than inventing new behavior. Reset defaults (s_ba[0xA]=1
 * "Power on", s_dve_regs[0x7e]=0x1C "status OK") are also copied
 * verbatim from pcsx2/Memory.cpp's memReset().
 *
 * Scope: this is a faithful, minimal, cited PORT of an already-shipped,
 * known-correct-enough (real PCSX2 boots real BIOSes past this same
 * point with exactly this stub) reference implementation - NOT a new
 * "magic trigger"/fabricated register the way the user's Round-949
 * MECHACON/SIO2-IP3 proposal was (see that round's STATUS.md entry for
 * why that one was declined). The difference: this address, this
 * exact poll sequence, and this exact fix are all independently
 * observed in our own disassembly AND independently confirmed against
 * a second, unrelated, real, working PS2 emulator's source code - two
 * independent lines of evidence agreeing, not one party's guess.
 *
 * Not checkpointed (no ee_dve_get_checkpoint_blob()): the state this
 * file owns is tiny and, per the real hardware protocol modeled here,
 * self-resolves (command_executing goes false) after at most 3 polls
 * - i.e. it is only ever "mid-command" for a handful of EE
 * instructions right at this one early-boot site. A checkpoint taken
 * mid-command would need this serialized to resume correctly; no
 * currently-used checkpoint in this project is taken that early, but
 * this is flagged here explicitly rather than silently assumed safe.
 */
#ifndef PCSX2WII_CORE_HW_EE_DVE_H
#define PCSX2WII_CORE_HW_EE_DVE_H

#include <stdint.h>

#define EE_DVE_BASE 0x1A000000u
#define EE_DVE_SIZE 0x100u

/* Resets all DVE stub state to the real PCSX2 memReset() defaults. */
void ee_dve_init(void);

/* addr is the physical (KSEG-stripped) address, as passed to the
 * other ee_*_mmio_read16/write16-style helpers in this project.
 * Returns 1 and fills *out / consumes val if addr is in range,
 * 0 (no-op) otherwise - same convention as sif_mmio_read32() etc. */
int ee_dve_mmio_read16(uint32_t addr, uint16_t *out);
int ee_dve_mmio_write16(uint32_t addr, uint16_t val);

#endif
