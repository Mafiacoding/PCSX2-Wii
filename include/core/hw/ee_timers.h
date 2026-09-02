#ifndef PCSX2WII_EE_TIMERS_H
#define PCSX2WII_EE_TIMERS_H

#include <stdint.h>

#define EE_TIMERS_COUNT 4

/*
 * ee_timers.h - EE peripheral counter/timer register block
 * (T0_COUNT/T0_MODE/T0_COMP/T0_HOLD .. T3_COUNT/T3_MODE/T3_COMP) +
 * real tick/IRQ model.
 *
 * Round 87 (task #172 continuation): this project had NO EE
 * peripheral-timer model at all before this - only the COP0
 * Count/Compare mechanism (ee_check_timer_interrupt() in ee_core.c,
 * Cause.IP7) was ever implemented. These are a completely separate,
 * real piece of EE hardware (pcsx2/Hw.h's EERegisterAddresses T0_COUNT
 * etc., pcsx2/Counters.cpp's vSyncInfo/EE counters), confirmed
 * unimplemented by grepping this codebase (zero hits) and by
 * ee_mem_read32/write32's own pre-existing comment in ee_core.c
 * explicitly stating "timers, INTC, SIO, GIF/VIF/IPU control regs
 * still fall through to the silent-no-op ... path". Found while
 * investigating why real BIOS code (per the 127th finding's
 * INTC_MASK=0x00001002 live capture) unmasks EE_INTC bit 12
 * (TIMER3) specifically - a source that could never fire while this
 * peripheral didn't exist.
 *
 * Real PS2 EE counter base addresses (pcsx2/Hw.h EERegisterAddresses,
 * T0_COUNT=0x10000000/T0_MODE=0x10000010/T0_COMP=0x10000020/
 * T0_HOLD=0x10000030, each subsequent timer offset by +0x800 from the
 * previous: T1=0x10000800, T2=0x10001000, T3=0x10001800). Real
 * hardware: only T0/T1 implement HOLD (used for gate-mode external
 * counter capture); T2/T3 do not have a HOLD register at all.
 *
 * Modeled here, following this project's established "1 instruction
 * = 1 cycle" simplification (already used for COP0 Count/Compare and
 * for iop_timers.c - see that file's own header comment for the
 * identical rationale) and the same honestly-scoped-down approach:
 *   - Real free-running COUNT increment, one per EE cycle
 *     (ee_timers_tick(), called once per instruction step exactly
 *     like iop_timers_tick() already is for the IOP side).
 *   - Real target-match (COMP) and overflow-triggered IRQ delivery,
 *     gated on the real MODE bits (CUE count-enable, CMPE
 *     compare-irq-enable, per pcsx2/Counters.cpp's rcntUpdate()/
 *     rcntWmode() real bit semantics), raised via the existing
 *     ee_intc_raise() hook (EE_INTC bits 9-12 = TIMER0-3, per
 *     ee_intc.h's real 10-source EERegisterAddresses citation).
 *   - Real EQUF/OVFF flag bits set on match/overflow (read-only
 *     status, cleared by software rewriting MODE - same convention
 *     iop_timers.c already uses for its own TARGET_FLAG/OVERFLOW_FLAG).
 *
 * Round 127 (task #172/#247, 167th finding): CLKS (bits 0-1, clock
 * source select) is now modeled - see ee_timers_tick()'s own comment
 * in ee_timers.c for the full citation trail and rationale. Found via
 * live host-native instrumentation of this project's own interpreter
 * (observing what MODE value the real BIOS itself configures - not
 * transcribed BIOS bytes, just an observed runtime register value)
 * that real boot code configures Timer3 with CLKS=3 (HBLNK), which
 * this project's original 1-instruction=1-tick model silently treated
 * identically to CLKS=0 (BUSCLK) - causing Timer3 to reach its
 * COMP=0xFFFF target roughly 18,743x too fast relative to real
 * elapsed time, firing its interrupt long before real kernel
 * bootstrap code could have reached the same point of execution.
 *
 * Still explicitly NOT modeled (matching iop_timers.h's own scope
 * disclaimer): GATE modes, ZRET's exact zero-return timing edge cases
 * beyond a plain wrap-on-match, and T0/T1's HOLD-latch-on-external-
 * signal behavior (HOLD is modeled as a plain read/write register
 * with no real gate-triggered latching).
 */

typedef struct {
    uint32_t count;
    uint32_t mode;
    uint32_t comp;
    uint32_t hold; /* only meaningful for T0/T1 on real hardware; T2/T3 keep it inert */
} ee_timer_t;

typedef struct {
    ee_timer_t t[EE_TIMERS_COUNT];
} ee_timers_state_t;

/* Real EE T*_MODE bit layout - CORRECTED Round 751 (task #733
 * continuation). The bit positions below Round 87/127 originally used
 * (CMPE=bit6, OVFE=bit7, CUE=bit10, EQUF=bit11, OVFF=bit12, plus
 * invented REPEAT_IRQ=bit8/TOGGLE_IRQ=bit9 bits that don't exist on
 * real hardware at all) were WRONG - never cross-checked against the
 * real bitfield layout, just guessed from the register names alone.
 * Found this round while root-causing a permanent stall: the real
 * PAL BIOS (30004R V6) writes T0_MODE=0x83 during a GT3 disc-boot
 * checkpoint-chain run, then busy-waits (pc=0x9FC41048, disassembled
 * this round with tools/round655-ee-disasm) for T0_COUNT to change -
 * but under the OLD (wrong) bit mapping, 0x83's bit7 landed on
 * EE_CNT_MODE_OVF_ENABLE, not CUE (bit10, unset by this write), so
 * ee_timers_tick()'s `if (!(t->mode & EE_CNT_MODE_CUE)) continue;`
 * guard kept the counter permanently stopped and the busy-wait spun
 * forever (confirmed directly: a checkpoint dump showed T0
 * count=0/mode=0x83/CUE=0 after 3.12B instructions of chaining).
 *
 * Real layout confirmed directly from real PCSX2 source
 * (pcsx2-master.zip, pcsx2/Counters.h's EECNT_MODE bitfield struct,
 * cross-checked against Counters.cpp's rcntWmode() - its write mask
 * `value & 0x3ff` for the configurable low bits and
 * `value & 0xc00` for the write-1-to-clear TargetReached/
 * OverflowReached flags is exactly the bit split below):
 *   bit0-1  (0x003): ClockSource (CLKS)
 *   bit2    (0x004): EnableGate (GATE_ENABLE)
 *   bit3    (0x008): GateSource (0=hblank type,1=vblank type) - not modeled
 *   bit4-5  (0x030): GateMode - not modeled
 *   bit6    (0x040): ZeroReturn (ZRET)
 *   bit7    (0x080): IsCounting (CUE) - THE bit this round's stall needed
 *   bit8    (0x100): TargetInterrupt (CMPE)
 *   bit9    (0x200): OverflowInterrupt (OVFE)
 *   bit10   (0x400): TargetReached (EQUF) - write-1-to-clear
 *   bit11   (0x800): OverflowReached (OVFF) - write-1-to-clear
 * There is no separate real "repeat vs one-shot IRQ" bit anywhere in
 * this register - real _cpuTestTarget()/_cpuTestOverflow() (Counters.cpp)
 * gate re-firing purely on the TargetReached/OverflowReached flag
 * (only cleared by software rewriting MODE with that bit set to 1),
 * never by disabling TargetInterrupt/OverflowInterrupt themselves.
 * ee_timers_tick() (ee_timers.c) is fixed to match this exactly this
 * round - see that function's own updated comment. */
#define EE_CNT_MODE_CLKS        0x0003u /* bits 0-1, clock source: 0=BUSCLK, 1=BUSCLK/16, 2=BUSCLK/256, 3=HBLNK - modeled since Round 127 */
#define EE_CNT_MODE_GATE_ENABLE 0x0004u /* bit2, not modeled */
#define EE_CNT_MODE_GATE_SOURCE 0x0008u /* bit3, not modeled */
#define EE_CNT_MODE_GATE_MODE   0x0030u /* bits 4-5, not modeled */
#define EE_CNT_MODE_ZERO_RETURN 0x0040u /* bit6 (ZRET) - corrected Round 751, was bit5 */
#define EE_CNT_MODE_CUE         0x0080u /* bit7 (IsCounting/count-enable) - corrected Round 751, was bit10 */
#define EE_CNT_MODE_CMP_ENABLE  0x0100u /* bit8 (CMPE: compare-match IRQ enable) - corrected Round 751, was bit6 */
#define EE_CNT_MODE_OVF_ENABLE  0x0200u /* bit9 (OVFE: overflow IRQ enable) - corrected Round 751, was bit7 */
#define EE_CNT_MODE_EQUF        0x0400u /* bit10 (TargetReached, compare-match flag) - corrected Round 751, was bit11 */
#define EE_CNT_MODE_OVFF        0x0800u /* bit11 (OverflowReached, overflow flag) - corrected Round 751, was bit12 */

void ee_timers_init(void);

/* Returns 1 and fills *out if addr (already KSEG0/1-masked by the
 * caller, same convention as dma_mmio_read32/ee_intc_mmio_read32) is
 * one of the 4 timers' COUNT/MODE/COMP/HOLD registers, 0 otherwise. */
int ee_timers_mmio_read32(uint32_t addr, uint32_t *out);
int ee_timers_mmio_write32(uint32_t addr, uint32_t value);

/* Advances every counter by one tick and delivers any real
 * compare-match/overflow interrupt this causes via ee_intc_raise()
 * (bits 9-12 = TIMER0-3). Must be called unconditionally once per EE
 * instruction step, same placement/rationale as ee_check_vblank()/
 * ee_check_gs_vsync() in ee_core.c. */
void ee_timers_tick(void);

ee_timers_state_t *ee_timers_get_state(void);

/* Round 715 (task #693/#694, per the user's explicit "focus only on
 * this timer issue" instruction): per-timer real IRQ-raise hit
 * counter (compare-match OR overflow, whichever actually calls
 * ee_intc_raise() - see ee_timers_tick()'s own doc comment), mirroring
 * this project's established hit-counter diagnostic convention
 * (iop_dma_get_sif2_transfer_count(), dispatch_ncmd() count, etc.).
 * Lets a boot survey empirically confirm real IRQ cadence (does a
 * given timer fire once, or does it keep firing forever as a real
 * periodic heartbeat would) - independent of guessing from register
 * snapshots alone, which can't distinguish "never armed" from "armed,
 * fired once, then silently suppressed" from "armed and firing
 * correctly but too slowly to have fired again yet". */
uint32_t ee_timers_get_irq_count(int idx);

#endif
