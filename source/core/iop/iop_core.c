/*
 * iop_core.c - R3000A (IOP) interpreter
 *
 * Semantics ported from PCSX2's pcsx2/R3000AOpcodeTables.cpp and
 * R3000A.cpp (GPL-3.0), same approach as ee_core.c: not reinvented
 * from the MIPS manual, so behavior matches real PCSX2 for opcodes
 * covered.
 *
 * Coverage: the MIPS I integer core - ALU imm+reg, shifts, MULT/DIV,
 * HI/LO moves, branches (incl. REGIMM w/ link variants), jumps incl.
 * link register, byte/half/word load+store, and unaligned
 * LWL/LWR/SWL/SWR. Basic COP0 (MFC0/MTC0), plus a real SYSCALL
 * exception (Cause/EPC/Status updated and PC vectored to
 * 0xBFC00180/0x80000080 depending on Status.BEV, ported from PCSX2's
 * psxException() in R3000A.cpp - see the SYSCALL case below for the
 * one documented simplification: branch-delay-slot detection isn't
 * modeled). BREAK is deliberately kept as this project's own
 * clean-halt-for-testing convention rather than also raising a real
 * exception, since every test in tests/ relies on it to signal clean
 * completion.
 *
 * Wired into a shared address space with: the SIF mailbox mirror
 * (core/hw/sif.h, 0x1D000000 window), the IOP's own interrupt
 * controller (core/hw/iop_intc.h), its own DMA controller register
 * stubs (core/hw/iop_dma.h), counter/timer register stubs
 * (core/hw/iop_timers.h), a BIOS syscall trap for the classic
 * A0/B0/C0 call convention (core/hw/iop_hle_bios.h), and a module
 * registry scaffold (core/hw/iop_hle_modules.h) - see each header for
 * exact scope/caveats. Runs interleaved with the EE core via
 * source/core/system.c, wired into main.c's actual boot path.
 *
 * NOT implemented: IOP HLE module loading beyond the registry
 * scaffold (no real IRX parsing, no real module ABI), no TLB (the
 * IOP doesn't have one on real hardware either). UPDATE (task #115):
 * real hardware-interrupt exceptions (I_STAT&I_MASK) ARE now
 * delivered - see iop_check_hw_interrupt() below. UPDATE (tasks
 * #215/#216): the IOP counter/timer block now really ticks and can
 * raise IRQs (iop_timers_tick()), and a real IOP VBLANK_IN/VBLANK_OUT
 * interrupt is now modeled (iop_check_vblank()) - see each function's
 * own doc comment for citations. See docs/ROADMAP.md for the full
 * picture and docs/STATUS.md's "First real BIOS boot attempt"
 * section for what a real BIOS dump's execution against this core
 * actually looks like today.
 */

#include "core/iop/iop_core.h"
#include "core/hw/sif.h"
#include "core/hw/iop_intc.h"
#include "core/hw/iop_dma.h"
#include "core/hw/iop_timers.h"
#include "core/hw/iop_spu2.h" /* SPU2 register scaffold - task #95 */
#include "core/hw/iop_cdvd.h" /* CDVD register scaffold, no-disc boot case - ROADMAP section 7 */
#include "core/hw/iop_cdrom_legacy.h" /* Round 133 (173rd finding): PS1-legacy CD-ROM Index/Status Register - see header for full trace/citation */
#include "core/hw/iop_hle_bios.h"
#include "core/hw/iop_hle_modules.h"
#include "core/hw/iop_hle_intr.h"
#include "core/hw/iop_module_loader.h" /* real IOP module/IRX loader - task #92 */
#include "core/hw/iop_excb.h" /* real exception-handler priority chains at RAM[0x100] - Round 22 */
#include "core/hw/iop_icfg.h" /* real ICFG register / EE INTC_SBUS raise - task #214, 85th finding */
#include "core/hw/iop_elf.h" /* real ELF32/MIPS IOP loader - reused for device-table embedded images, task #221/#245 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#define IOP_RAM_SIZE (2 * 1024 * 1024)
#define IOP_RESET_VECTOR 0xBFC00000u

static iop_state_t g_iop;

iop_state_t *iop_core_get_state(void) { return &g_iop; }

static inline uint8_t *iop_mem_ptr(iop_state_t *st, uint32_t addr, uint32_t size)
{
    if (addr >= IOP_RESET_VECTOR) {
        uint32_t off = addr - IOP_RESET_VECTOR;
        if (st->bios && off + size <= st->bios->size)
            return st->bios->data + off;
        return NULL;
    }
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys + size <= st->ram_size)
        return st->ram + phys;
    return NULL;
}

/* Same little-endian-explicit approach as ee_core.c - IOP memory is
 * little-endian, our Wii/PowerPC build target is big-endian. */
uint8_t iop_mem_read8(iop_state_t *st, uint32_t addr)
{
    uint8_t cdvd_val;
    if (iop_cdvd_mmio_read8(addr, &cdvd_val))
        return cdvd_val;

    /* Round 133 (task #172/#221/#288, 173rd finding): the separate
     * PS1-legacy CD-ROM controller block (0x1F801800-0x1F801803),
     * NOT to be confused with the PS2-native CDVD page above - see
     * iop_cdrom_legacy.h for the full instruction-level trace that
     * found this project's own IOP boot path polls this exact
     * register's PRMEMPT bit. */
    uint8_t cdrom_legacy_val;
    if (iop_cdrom_legacy_mmio_read8(addr, &cdrom_legacy_val))
        return cdrom_legacy_val;

    uint8_t *p = iop_mem_ptr(st, addr, 1);
    return p ? *p : 0;
}

uint16_t iop_mem_read16(iop_state_t *st, uint32_t addr)
{
    uint16_t spu2_val;
    if (iop_spu2_mmio_read16(addr, &spu2_val))
        return spu2_val;

    /* Round 74 (114th finding, task #172 continuation): the IOP
     * interrupt controller (I_STAT/I_MASK/I_CTRL, core/hw/iop_intc.h)
     * previously had ONLY a 32-bit read/write path wired up here -
     * iop_mem_read32()/iop_mem_write32() dispatch to it, but this
     * 16-bit path never did. Live host-native tracing (scratch copy,
     * never committed) proved real BIOS-resident code genuinely
     * issues 16-bit `sh`/`lh`-class accesses to 0x1F801074 (I_MASK) -
     * observed real writes of value 0x0001 and 0x0008 following a
     * live, correctly-resolved EnableIntr(16) call (see the 113th/
     * 114th findings) - which were previously silently falling
     * through to a plain-RAM 16-bit store/load instead of reaching
     * the interrupt-controller model at all, explaining why I_MASK
     * stayed 0x00000000 even after the P/I twin-export-shadowing fix
     * (task #239) made EnableIntr's argument correct. Widening to the
     * full 32-bit read and truncating to the low 16 bits matches this
     * project's own existing "plain assignment" semantics for this
     * register (see iop_intc.c's write32 case) - every real access to
     * this register observed in tracing was 16-bit, so there is no
     * evidence of a competing 32-bit access whose upper half would
     * need preserving separately. */
    if (addr >= 0x1F801070u && addr <= 0x1F80107Bu) {
        uint32_t intc_val;
        if (iop_intc_mmio_read32(addr, &intc_val))
            return (uint16_t)intc_val;
    }

    /* Round 75 (115th finding, task #243, task #172 continuation):
     * same class of bug as the 114th finding above, this time for the
     * IOP counter/timer block (core/hw/iop_timers.h). Live host-native
     * tracing (scratch copy, never committed) of a real BIOS boot to
     * the current idle=1 steady state captured a genuine real 16-bit
     * `sh`-class write of value 0x0070 (preceded by a 0x0000 write) to
     * address 0xBF8014A4 - T5's MODE register (T5 base 0x1F8014A0 +
     * 0x04, see iop_timers.c's s_ranges[5]) - which this project's own
     * ps2sdk timrman.c citation (see iop_timers.h) says SHOULD be
     * 32-bit-only on real hardware (only T0-T2 use 16-bit access per
     * that community-reimplementation source). Per this session's
     * established discipline (trust live tracing over the ps2sdk
     * reference, which is a reimplementation, not the retail ROM),
     * the live evidence wins: real BIOS-resident code genuinely
     * issues a 16-bit access to a timer register this project
     * classified as 32-bit-only, and it was previously silently
     * falling through to a plain-RAM 16-bit load/store instead of
     * reaching the timer model at all - meaning the real MODE=0x70
     * configuration write was being dropped. iop_timers_mmio_read32()
     * internally calls find_timer(), which already masks off the
     * KUSEG/KSEG0/KSEG1 segment-select bits (phys = addr &
     * 0x1FFFFFFFu) before matching - so passing the raw addr through
     * here (unlike the intc check above, which needed its own literal
     * range gate) correctly resolves the observed KSEG1-aliased
     * 0xBF8014A4 form. No narrower per-timer width restriction is
     * applied since the evidence directly contradicts the assumption
     * that would have motivated one. */
    {
        uint32_t timer_val;
        if (iop_timers_mmio_read32(addr, &timer_val))
            return (uint16_t)timer_val;
    }

    uint8_t *p = iop_mem_ptr(st, addr, 2);
    if (!p) return 0;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t iop_mem_read32(iop_state_t *st, uint32_t addr)
{
    /* IOP-side SIF mailbox mirror (0x1D000000-0x1D0000FF) - see
     * core/hw/sif.h. Checked before the RAM/BIOS path since it's
     * outside IOP RAM's range anyway, but explicit is better than
     * relying on that fact silently. */
    uint32_t sif_val;
    if (sif_iop_mmio_read32(addr, &sif_val))
        return sif_val;
    uint32_t intc_val;
    if (iop_intc_mmio_read32(addr, &intc_val))
        return intc_val;
    uint32_t dma_val;
    if (iop_dma_mmio_read32(addr, &dma_val))
        return dma_val;
    uint32_t timer_val;
    if (iop_timers_mmio_read32(addr, &timer_val))
        return timer_val;
    uint32_t spu2_val;
    if (iop_spu2_mmio_read32(addr, &spu2_val))
        return spu2_val;
    uint32_t icfg_val;
    if (iop_icfg_mmio_read32(addr, &icfg_val))
        return icfg_val;

    uint8_t *p = iop_mem_ptr(st, addr, 4);
    if (!p) return 0;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void iop_mem_write8(iop_state_t *st, uint32_t addr, uint8_t val)
{
    if (iop_cdvd_mmio_write8(addr, val))
        return;

    /* Round 133 (173rd finding) - see iop_mem_read8()'s matching
     * comment above. */
    if (iop_cdrom_legacy_mmio_write8(addr, val))
        return;

    uint8_t *p = iop_mem_ptr(st, addr, 1);
    if (p) *p = val;
}

void iop_mem_write16(iop_state_t *st, uint32_t addr, uint16_t val)
{
    if (iop_spu2_mmio_write16(addr, val))
        return;

    /* Round 74 (114th finding) - see iop_mem_read16()'s matching
     * comment above for the full citation/evidence trail. Zero-
     * extending the 16-bit value into iop_intc_mmio_write32() mirrors
     * every real access to this register this round's tracing
     * observed (all 16-bit, none 32-bit), and matches this project's
     * own existing "plain assignment" write semantics for I_MASK/
     * I_STAT/I_CTRL. */
    if (addr >= 0x1F801070u && addr <= 0x1F80107Bu) {
        if (iop_intc_mmio_write32(addr, (uint32_t)val))
            return;
    }

    /* Round 75 (115th finding, task #243) - see iop_mem_read16()'s
     * matching comment above for the full citation/evidence trail.
     * Zero-extending mirrors the intc fix's treatment and this
     * project's existing "plain assignment" write semantics for
     * COUNT/MODE/TARGET (iop_timers_mmio_write32's own cases). */
    {
        if (iop_timers_mmio_write32(addr, (uint32_t)val))
            return;
    }

    uint8_t *p = iop_mem_ptr(st, addr, 2);
    if (!p) return;
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
}

void iop_mem_write32(iop_state_t *st, uint32_t addr, uint32_t val)
{
    if (sif_iop_mmio_write32(addr, val))
        return;
    if (iop_intc_mmio_write32(addr, val))
        return;
    if (iop_dma_mmio_write32(addr, val))
        return;
    if (iop_timers_mmio_write32(addr, val))
        return;
    if (iop_spu2_mmio_write32(addr, val))
        return;
    if (iop_icfg_mmio_write32(addr, val))
        return;

    uint8_t *p = iop_mem_ptr(st, addr, 4);
    if (!p) return;
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
    p[2] = (uint8_t)((val >> 16) & 0xFF);
    p[3] = (uint8_t)((val >> 24) & 0xFF);
}

int iop_core_init(const bios_image_t *bios)
{
    memset(&g_iop, 0, sizeof(g_iop));

    iop_intc_init(); /* IOP interrupt controller register block - see core/hw/iop_intc.h */
    iop_dma_init();  /* IOP DMA controller register block - see core/hw/iop_dma.h */
    iop_timers_init(); /* IOP counter/timer register stub - see core/hw/iop_timers.h */
    iop_spu2_init(); /* SPU2 register scaffold - task #95, see core/hw/iop_spu2.h */
    iop_cdvd_init(); /* CDVD register scaffold, no-disc boot case - see core/hw/iop_cdvd.h */
    iop_cdrom_legacy_init(); /* PS1-legacy CD-ROM Index/Status Register - Round 133, see core/hw/iop_cdrom_legacy.h */
    iop_hle_bios_init(); /* IOP BIOS syscall trap (A0/B0/C0) - see core/hw/iop_hle_bios.h */
    iop_hle_modules_init(); /* IOP module registry scaffold - see core/hw/iop_hle_modules.h */
    iop_hle_intr_init(); /* Round 109: clean-room RegisterIntrHandler/RegisterExceptionHandler HLE table - see core/hw/iop_hle_intr.h */
    iop_module_loader_reset(); /* real module/IRX boot sequencer - see core/hw/iop_module_loader.h */
    iop_icfg_init(); /* real ICFG register - task #214, 85th finding */

    g_iop.ram = memalign(32, IOP_RAM_SIZE);
    if (!g_iop.ram)
        return -1;
    memset(g_iop.ram, 0, IOP_RAM_SIZE);
    g_iop.ram_size = IOP_RAM_SIZE;

    g_iop.bios = bios;
    g_iop.pc = IOP_RESET_VECTOR;
    g_iop.next_pc = IOP_RESET_VECTOR + 4;

    /* Real hardware/PCSX2 (R3000A.cpp's psxReset()) initializes
     * Status.BEV (bit 22, "use bootstrap exception vectors") to 1 on
     * reset - real BIOS boot code relies on this to route early
     * exceptions to 0xBFC00180 before it has set up RAM-resident
     * handlers and cleared this bit itself. Without this, our SYSCALL
     * exception handling (above) would incorrectly default to the
     * "normal" vector (0x80000080) from the very start. */
    g_iop.cop0[12] = 0x00400000u;

    /* Round 59 (91st finding, task #219), REVERTED Round 60 (93rd
     * finding, task #222): real PCSX2's psxReset() (pcsx2/R3000A.cpp,
     * the EXACT SAME function already cited two lines above for
     * Status.BEV) sets `psxRegs.CP0.n.PRid = 0x0000001f;` right next
     * to that Status line - this project's Status.BEV init carried
     * that over correctly but never the PRid half, leaving IOP COP0
     * register 15 at its zero-initialized default from the memset()
     * above. Real ps2sdk boot code (intrman.c/timrman.c/sifman.c/
     * udnl.c/modload.c/igreeting.c's real `_start()` functions, all
     * independently reading `get_mips_cop_reg(0, COP0_REG_PRId)`)
     * uses this value to decide P/I module-twin residency
     * (INTRMANP/I, TIMEMANP/I, ...) - `prid >= 16` selects the "I"
     * twin, matching real retail PS2 hardware rather than PS1-BC
     * mode.
     *
     * Round 59 set this to the real 0x1f value - correct and cited,
     * but empirically (live-traced, 92nd finding) it also changes
     * the real BIOS ROM's OWN early boot control flow (BEFORE this
     * project's module loader ever gets a chance to run - see
     * iop_module_loader_boot()'s own header comment: it only
     * activates when real ROM code's PC tries to escape into memory
     * this project doesn't model as fetchable, which never happens
     * on the corrected-PRId path since it stays inside real, valid
     * ROM content the whole way). With the real PRId, the ROM walks
     * into a real device/driver descriptor validation routine at
     * 0xBFC4A340+ (disassembled via the live PCSX2 reference
     * debugger) that this project has no data for, hits a genuine
     * dead-end/panic loop (write status byte 2 to IOP RAM address 0,
     * spin forever, confirmed via live diagnostic: neither MIPS
     * exception vector is ever entered from there), and NEVER
     * reaches this project's own synthesized module-loading system
     * at all - meaning all of tasks #86-217's boot-progress work
     * (SYSMEM through THREADMAN and the whole SIF/RPC/OSDSYS chain)
     * becomes unreachable, a real net regression toward the
     * project's actual goal (a visible splash screen), confirmed via
     * a controlled isolation test (reverting only this one line
     * while keeping every other Round 59 change restores the
     * familiar pre-Round-59 idle=1/pc~0x8000CFCC boot state exactly).
     *
     * Deliberately reverted to 0 for now - the real value is still
     * correctly cited above for whenever a future round manages to
     * model that early ROM device-table validation well enough not
     * to dead-end on it (tracked as task #222); until then, 0 is the
     * practically useful value, and `iop_module_loader.c`'s P/I
     * export-visibility fix (Round 59/60) has been made conditional
     * on this exact value so it automatically does the right thing
     * either way, with no further change needed here when this does
     * get revisited. */
    g_iop.cop0[15] = 0x00000000u;

    /* Round 22: real RAM[0x100] exception-handler priority-chain
     * table + array, all-empty (no handler registered) - see
     * include/core/hw/iop_excb.h. Must run after g_iop.ram is
     * allocated (iop_excb_init() writes through iop_mem_write32()). */
    iop_excb_init(&g_iop);

    return 0;
}

static void halt(const char *reason)
{
    g_iop.halted = 1;
    strncpy(g_iop.halt_reason, reason, sizeof(g_iop.halt_reason) - 1);
}

/* Real IOP hardware-interrupt bit position, per the public psx-spx
 * reference (https://psx-spx.consoledev.net/interrupts/, "COP0
 * Interrupt Handling" / "PSX specific COP0 Notes" - explicitly noted
 * on that same page as applying to the PS2 IOP too: "The PS2's IOP
 * has the same interrupt controller as the PS1 but with more
 * channels"). Unlike the EE's 8 independent Cause.IP0-IP7 lines (see
 * ee_core.c's EE_CAUSE_IP7), the real IOP/PS1 architecture routes
 * EVERY peripheral IRQ (VBLANK, DMA, timers, etc - all of I_STAT/
 * I_MASK) through this ONE single CPU interrupt line, bit 10 of both
 * Cause (IP2) and Status (IM2): "If one or more interrupts are
 * requested and enabled, ie. if (I_STAT AND I_MASK)=nonzero, then
 * cop0r13.bit10 gets set, and when cop0r12.bit10 and cop0r12.bit0 are
 * set, too, then the interrupt gets executed." Also unlike the EE's
 * timer interrupt (Cause.IP7, a real sticky software-cleared latch),
 * this line is explicitly documented as NON-latching: "cop0r13.bit10
 * is NOT a latch, ie. it gets automatically cleared as soon as
 * (I_STAT AND I_MASK)=zero" - so this project recomputes it fresh
 * every step rather than latching it once and waiting for software to
 * clear it. */
#define IOP_CAUSE_IP2  0x400u
#define IOP_STATUS_IM2 0x400u

/* Round 22 (see docs/STATUS.md): confirmed, while investigating why
 * Status.IEc never has any observable effect, that this check simply
 * didn't exist anywhere in this file - iop_intc.c's own scope comment
 * already flagged it ("NOT modeled: actually raising a CPU interrupt/
 * exception in iop_core.c when I_STAT & I_MASK becomes nonzero").
 * Implements exactly the two-step real hardware behavior quoted in
 * the comment above: first, Cause.IP2 mirrors (I_STAT & I_MASK) live,
 * non-latching; second, if Cause.IP2 AND Status.IM2 AND Status.IEc
 * (bit 0) are all set, a real Interrupt exception (Cause.ExcCode=0)
 * is raised, vectored exactly like the existing SYSCALL case (Status.
 * BEV-dependent vector, same KU/IE mode-stack push formula). Same
 * documented simplification as SYSCALL: this project's IOP
 * interpreter doesn't track branch-delay-slot state at all, so EPC is
 * always set to the next not-yet-executed instruction's own address,
 * never this_pc-4/Cause.BD - a real interrupt landing exactly on a
 * branch's delay slot is not modeled, same honest gap already
 * documented for SYSCALL. */
/* Task #216 (splash-screen blocker investigation, continued from
 * #214/#215): real IOP hardware exposes its own VBLANK interrupt,
 * distinct from the EE's already-modeled VBLANK (see ee_core.c's
 * ee_check_vblank() comment for the EE side). Real PCSX2's
 * IopCounters.cpp confirms this directly and was fetched/cited this
 * round: psxVBlankStart() calls iopIntcIrq(0), psxVBlankEnd() calls
 * iopIntcIrq(11) - independently corroborated by allkern/iris's
 * src/iop/intc.h, which names the exact same bit positions
 * IOP_INTC_VBLANK_IN (0x00000001, bit 0) and IOP_INTC_VBLANK_OUT
 * (0x00000800, bit 11), and calls them from its GS vsync-start/-end
 * handlers alongside the equivalent EE-side EE_INTC_VBLANK_IN/OUT
 * raises - two independent real sources agreeing on both bit
 * numbers. This is a real, continuously-firing, hardware-driven
 * interrupt line, independent of what the IOP CPU itself is doing -
 * the same property that made the timer-tick mechanism (#215) a
 * candidate for waking a genuinely `idle`-parked IOP (see
 * iop_core.h's `idle` field doc comment) - VBLANK is an even more
 * certain real-hardware source of periodic activity than a
 * boot-configured timer, since it requires no prior MODE-register
 * setup by any module at all.
 *
 * Timing: reuses this project's own already-established, already-
 * cited EE_CYCLES_PER_FRAME_NTSC (ee_core.c: 4921488, from the real
 * 294.912 MHz EE clock / 59.94 Hz NTSC refresh), divided by the real
 * ~8:1 EE:IOP clock ratio this project already documents and targets
 * (source/core/system.c's own header comment: ~294 MHz EE vs ~33-36
 * MHz IOP) rather than computing an independent IOP-clock figure, to
 * stay consistent with the one ratio this project has already
 * committed to elsewhere. VBLANK_END's offset reuses the same 1/12-
 * of-frame approximation ee_check_vblank() already uses and
 * documents at length (real NTSC vertical blanking is roughly 8.5%
 * of a frame). Ticked here off `instructions_executed`, the same
 * 1-instruction-=1-cycle simplification iop_timers_tick() and
 * ee_check_vblank() both already use and document - no new timing
 * model invented for this. */
#define IOP_CYCLES_PER_FRAME_NTSC  (4921488u / 8u)
#define IOP_CYCLES_VBLANK_DURATION (IOP_CYCLES_PER_FRAME_NTSC / 12u)
#define IOP_INTC_IRQ_VBLANK_START  0
#define IOP_INTC_IRQ_VBLANK_END    11

static void iop_check_vblank(iop_state_t *st)
{
    uint64_t phase = st->instructions_executed % IOP_CYCLES_PER_FRAME_NTSC;
    if (phase == 0)
        iop_intc_raise(IOP_INTC_IRQ_VBLANK_START);
    else if (phase == IOP_CYCLES_VBLANK_DURATION)
        iop_intc_raise(IOP_INTC_IRQ_VBLANK_END);
}

static void iop_check_hw_interrupt(iop_state_t *st, uint32_t next_pc)
{
    iop_intc_state_t *intc = iop_intc_get_state();

    /* Round 112 (task #172/#267/#268, 152nd finding's honestly-
     * documented gap): Cause.IP2 must also reflect the real 32-63
     * "soft" irq range (istat_hi/imask_hi - see iop_intc.h), not just
     * the 32-bit I_STAT/I_MASK MMIO registers. On real hardware both
     * ranges are still genuinely "an IOP interrupt is pending" from
     * the CPU's point of view - the split between memory-mapped and
     * INTRMAN-internal is purely about HOW the source is identified/
     * multiplexed, not whether it drives the same physical Cause.IP2
     * line. */
    if ((intc->istat & intc->imask) || (intc->istat_hi & intc->imask_hi))
        st->cop0[13] |= IOP_CAUSE_IP2;
    else
        st->cop0[13] &= ~IOP_CAUSE_IP2;

    if (!(st->cop0[13] & IOP_CAUSE_IP2))
        return;
    if (!(st->cop0[12] & IOP_STATUS_IM2))
        return;
    if (!(st->cop0[12] & 0x1u)) /* Status.IEc */
        return;

    st->cop0[13] = (st->cop0[13] & ~0x7Fu); /* Cause.ExcCode = 0 (Interrupt) */
    st->cop0[14] = next_pc; /* EPC */
    st->cop0[12] = (st->cop0[12] & ~0x3Fu) | ((st->cop0[12] & 0x0Fu) << 2); /* Status stack push */
    st->exception_pending = 1; /* task #156 - see iop_core.h's field comment */

    /* Round 109 (task #172/#247/#249 continuation, following the
     * user's "lets fix this now and maybe the sony docs have some
     * info" directive): before falling back to this project's
     * pre-existing fixed-vector behavior, give the clean-room
     * RegisterIntrHandler table (core/hw/iop_hle_intr.h) a chance to
     * redirect straight into a REAL, module-registered handler for
     * whichever specific IRQ is actually firing - real MIPS/R3000A
     * hardware convention: the lowest-numbered set bit among
     * currently pending+unmasked sources is serviced first (same
     * priority-by-bit-number convention this project's own IOP_INTC_
     * IRQ_VBLANK_START/END constants already rely on being bit 0/11
     * respectively). Falls through to the unmodified default
     * (fixed-vector) behavior below when nothing is registered for
     * that bit - per the 135th finding's own explicit design
     * requirement, point (b). */
    {
        uint32_t pending = intc->istat & intc->imask;
        if (pending) {
            uint32_t irq = 0;
            while (!(pending & 1u)) { pending >>= 1; irq++; }
            if (iop_hle_intr_dispatch_interrupt(st, irq))
                return;
        } else {
            /* Round 112: the real 0-31 hardware range takes priority
             * (numerically lower irq numbers are serviced first on
             * real MIPS/R3000A hardware - same convention already
             * relied on above), so the soft 32-63 range is only
             * consulted when nothing is pending in the hardware
             * range. Nothing in this project raises into istat_hi
             * yet (no DMA-completion hardware model exists - see
             * iop_intc.h), so this is presently dead code in
             * practice, exactly like iop_intc_raise_soft() itself -
             * it exists so the dispatch SIDE of this mechanism is no
             * longer artificially capped at 32 irqs, closing the gap
             * the 152nd finding documented, ready for whichever
             * future hardware model raises the first real soft irq. */
            uint32_t soft_pending = intc->istat_hi & intc->imask_hi;
            if (soft_pending) {
                uint32_t bit = 0;
                while (!(soft_pending & 1u)) { soft_pending >>= 1; bit++; }
                if (iop_hle_intr_dispatch_interrupt(st, 32u + bit))
                    return;
            }
        }
    }

    uint32_t vector = (st->cop0[12] & 0x400000u) ? 0xBFC00180u : 0x80000080u; /* Status.BEV */
    st->pc = vector;
    st->next_pc = vector + 4u;
}

static int iop_step(void)
{
    iop_state_t *st = &g_iop;
    uint32_t pc = st->pc;

    /* IOP BIOS syscall trap (0xA0/0xB0/0xC0) - see core/hw/iop_hle_bios.h.
     * If this is one of the three trap addresses, the "instruction"
     * there is not really interpreted at all - the call is handled
     * natively and control is redirected straight to the return
     * address, so this step is complete without any real MIPS
     * instruction being fetched/decoded. */
    if (iop_hle_bios_try_handle(st, pc)) {
        st->instructions_executed++;
        return 0;
    }

    /* Round 109 (task #172/#247/#249 continuation): the clean-room
     * RegisterIntrHandler/RegisterExceptionHandler handler-
     * registration table's own 5 call gates and its "handler
     * finished" return trampoline - see core/hw/iop_hle_intr.h for
     * the full design. Checked in the same "intercept before fetch"
     * spot as the A0/B0/C0 table just above. */
    if (iop_hle_intr_try_handle(st, pc)) {
        st->instructions_executed++;
        return 0;
    }

    /* Real IOP module/IRX boot sequencer trampoline (task #92) -
     * see core/hw/iop_module_loader.h. Checked right after the A0/
     * B0/C0 BIOS trap, same "intercept before fetch" convention. */
    if (iop_module_loader_try_handle(st, pc)) {
        st->instructions_executed++;
        return st->halted ? 1 : 0;
    }

    /* Round 129 (task #172/#196, 169th finding, real fix): synthetic
     * default/spurious-interrupt-return stub. Real MIPS/R3000A
     * hardware fixed-vectors EVERY exception to 0x80000080 (or
     * 0xBFC00180 when Status.BEV=1) - real kernel software installs
     * a genuine dispatcher there very early in boot. This project
     * doesn't model that installation (and, per this project's
     * standing clean-room convention, never will by transcribing
     * real BIOS bytes to reconstruct one) - previously, whenever
     * iop_hle_intr_dispatch_interrupt() (just above, in the
     * interrupt-raise path) found no module-registered handler for
     * the firing IRQ and fell through to this fixed vector, the CPU
     * fetched whatever incidental RAM content happened to be sitting
     * at 0x80000080 - traced this round to stale bytes left over from
     * an earlier, unrelated real cache-flush instruction sequence
     * (see docs/STATUS.md's 169th finding for the full write-history
     * trace), eventually halting on a genuinely garbage word one
     * unrelated module's own store instruction had scribbled there.
     * Since the real, module-registered-handler path is ALREADY tried
     * first (immediately above, in the code that jumps here),
     * reaching this exact fixed address always means "no handler is
     * registered for this specific IRQ" - precisely the case a
     * minimal, clean-room default handler exists for. Modeled the
     * same way this project's other synthetic HLE stubs are (A0/B0/
     * C0 BIOS traps, etc.): acknowledge and RFE-equivalent return,
     * rather than attempting to reconstruct real kernel dispatcher
     * bytes. Deliberately NOT applied to the BEV=1 ROM vector
     * (0xBFC00180) - that address is genuine ROM content already
     * executed correctly by this project; the gap is specific to the
     * RAM-resident BEV=0 vector, which nothing ever populates. */
    if (pc == 0x80000080u && st->exception_pending) {
        /* Round 131 (task #172/#196/#286, real fix): the guard above
         * only ever checked pc/exception_pending, never Cause.ExcCode
         * - so it also caught genuine SYSCALL/BREAK/Trap exceptions
         * that fall through to this exact same fixed vector (this
         * project's boot model has no real dispatcher installed here
         * for those either, same root gap as the interrupt case
         * Round 129 fixed). Those exception classes are NOT
         * restartable the way interrupts are: real MIPS semantics
         * (universal ISA behavior, not BIOS-specific - any R3000A/
         * MIPS I reference) require EPC+4 on return from a
         * synchronous, software-triggered exception, since EPC points
         * AT the triggering instruction itself and simply re-running
         * it would refire the identical exception forever. This
         * project's OWN code already established exactly this
         * EPC+4-plus-$v0=0 convention for the sibling "SYSCALL fell
         * through to an unclaimed BREAK trap-stub" case above (Round
         * 29/task #124) - this fix applies the identical, already-
         * proven convention here instead of inventing a new one.
         * Diagnosed via host-native instrumentation (scratch copy,
         * real repo untouched during diagnosis): a real SYSCALL
         * (raw instruction word 0x0000000C, funct=0x0C) landing at
         * this vector was being resumed at bare EPC by the Round 129
         * stub, infinite-looping on the same SYSCALL forever - the
         * exact cause of the IOP's Round 130 "resting point"
         * (pc=0x8003ECF4) never actually being a resolved wait, just
         * an unresolved exception refiring every 2 instructions. */
        uint32_t exc_code = (st->cop0[13] & 0x7Cu) >> 2u;
        st->cop0[12] = (st->cop0[12] & ~0x0Fu) | ((st->cop0[12] & 0x3Cu) >> 2); /* RFE-equivalent Status stack pop */
        st->exception_pending = 0;
        uint32_t epc = st->cop0[14];
        if (exc_code == 0x08u || exc_code == 0x09u || exc_code == 0x0Du) {
            /* Syscall / Breakpoint / Trap - synchronous, non-
             * restartable: skip past the triggering instruction and
             * return the same generic "unimplemented, default value"
             * result this project already uses for every other
             * unclaimed BIOS/syscall call site. */
            st->gpr[2] = 0; /* $v0 = 0 */
            st->pc = epc + 4u;
            st->next_pc = epc + 8u;
        } else {
            /* Interrupt (ExcCode==0) or any other restartable class -
             * unchanged Round 129 behavior: resume the interrupted
             * instruction itself. */
            st->pc = epc;
            st->next_pc = epc + 4u;
        }
        st->instructions_executed++;
        return 0;
    }

    /* Guard against PC escaping into memory this project doesn't
     * model as real, fetchable code (round 14 finding: a live-traced
     * real BIOS boot path executes a genuine JALR $s1 whose target
     * looks like a cross-address-space pointer - plausible as an
     * EE-RAM module image location a real IOP module/IRX loader
     * would DMA-copy locally before jumping to it - but this
     * project's iop_hle_modules.c deliberately doesn't implement
     * real module loading, so no such code is ever actually present).
     * Before this check, an out-of-range fetch silently read back 0
     * (a NOP) forever, letting execution "wander" through effectively
     * unmapped memory for tens of millions of steps until it
     * coincidentally hit a non-zero MMIO register value and halted on
     * a confusing, unrelated-looking illegal-opcode message. Detecting
     * the escape immediately and halting with a clear, honest
     * diagnostic is far more useful - see docs/STATUS.md's
     * "round 14" section for the full trace. */
    {
        int pc_is_fetchable;
        if (pc >= IOP_RESET_VECTOR) {
            uint32_t off = pc - IOP_RESET_VECTOR;
            pc_is_fetchable = (st->bios && off + 4 <= st->bios->size);
        } else {
            uint32_t phys = pc & 0x1FFFFFFFu;
            pc_is_fetchable = (phys + 4 <= st->ram_size);
        }
        if (!pc_is_fetchable) {
            /* Task #92: before halting, give the real IOP module/
             * IRX loader (core/hw/iop_module_loader.h) exactly one
             * chance to take over - this is precisely the round-14
             * wall it was built to resolve (a real BIOS module-
             * loading JALR whose target only a real loader would
             * ever populate). If it can't find what it needs (e.g.
             * no real BIOS is loaded, as in most synthetic-BIOS
             * tests), it returns 0 immediately and this falls
             * through to the original halt below, unchanged. */
            if (iop_module_loader_boot(st)) {
#ifdef IOP_MODLOADER_DEBUG
                fprintf(stderr, "[modloader] boot succeeded, redirected pc=0x%08x at instr=%llu\n", st->pc, (unsigned long long)st->instructions_executed);
#endif
                return 0;
            }

            /* Kept short and %lX-formatted (not %X) on purpose: this
             * message is copied into halt_reason[128] by halt()'s
             * strncpy, and uint32_t is a `long` on this project's
             * PowerPC/Wii build target - a plain %X here mismatches
             * the promoted argument type and warns under devkitPPC's
             * gcc (caught by this round's "0 warnings" Wii rebuild
             * check, not by the host-native test suite, which uses a
             * 32-bit-int-width host where the mismatch is silent). */
            static char msg[96];
            snprintf(msg, sizeof(msg),
                     "PC escaped to unfetchable addr 0x%08lX (unloaded IOP module - see STATUS.md round 14)",
                     (unsigned long)pc);
            halt(msg);
            return 1;
        }
    }

    uint32_t instr = iop_mem_read32(st, pc);

    uint32_t op    = (instr >> 26) & 0x3F;
    uint32_t rs    = (instr >> 21) & 0x1F;
    uint32_t rt    = (instr >> 16) & 0x1F;
    uint32_t rd    = (instr >> 11) & 0x1F;
    uint32_t sa    = (instr >> 6)  & 0x1F;
    int32_t  imm   = (int16_t)(instr & 0xFFFF);
    uint32_t uimm  = instr & 0xFFFF;
    uint32_t funct = instr & 0x3F;

    uint32_t this_pc = pc;
    uint32_t fallthrough_pc = st->next_pc;
    st->pc = fallthrough_pc;
    st->next_pc = fallthrough_pc + 4;

    uint32_t rs32 = st->gpr[rs];
    uint32_t rt32 = st->gpr[rt];

#define GPR(x) st->gpr[x]
#define BRANCH_TO(target) do { st->next_pc = (target); } while (0)
#define LINK(reg) do { GPR(reg) = this_pc + 8; } while (0)

    switch (op) {
    case 0x00: /* SPECIAL */
        switch (funct) {
        case 0x00: /* SLL */  if (rd) GPR(rd) = rt32 << sa; break;
        case 0x02: /* SRL */  if (rd) GPR(rd) = rt32 >> sa; break;
        case 0x03: /* SRA */  if (rd) GPR(rd) = (uint32_t)((int32_t)rt32 >> sa); break;
        case 0x04: /* SLLV */ if (rd) GPR(rd) = rt32 << (rs32 & 0x1F); break;
        case 0x06: /* SRLV */ if (rd) GPR(rd) = rt32 >> (rs32 & 0x1F); break;
        case 0x07: /* SRAV */ if (rd) GPR(rd) = (uint32_t)((int32_t)rt32 >> (rs32 & 0x1F)); break;
        case 0x08: /* JR */   BRANCH_TO(GPR(rs)); break;
        case 0x09: /* JALR */ {
            uint32_t tgt = GPR(rs);
            /* Round 77 (117th finding, task #221/#245): these two
             * exact addresses are the real ROM's own two device-table-
             * slot "call the real init function now" sites (see the
             * caller's disassembly in the 116th/117th findings -
             * 0xBFC4A39C for slot 1, 0xBFC4A44C for slot 2). $rs at
             * each of these sites holds a raw, NEVER-RELOCATED field
             * read straight out of the still-in-ROM ELF header (the
             * real ROM code assumes this device's image was already
             * ELF-loaded into RAM by an earlier step this project
             * doesn't model) - live tracing confirmed this resolves
             * to tiny, clearly-wrong addresses (0x890, 0x30) that
             * either read as zero-filled RAM (a stream of SLL $0,$0,0
             * NOPs) or immediately return, falling straight into the
             * "should never get here" panic trap this project's 92nd/
             * 116th findings already documented. Since we now know
             * the real raw entry (remembered from the matching JAL to
             * 0xBFC4A600 above) is a genuine IRX-format ELF image,
             * really ELF-load it here (reusing iop_elf.c's existing,
             * already-tested loader - the exact same machinery
             * iop_module_loader.c uses for every ROMDIR module) and
             * redirect the jump to its real, correctly-relocated
             * entry point instead. Fixed, generous load addresses
             * (0x001C0000/0x001C8000) were chosen well clear of both
             * the ROMDIR module loader's own bump-allocated region
             * (observed peak 0x00145CA0 in a full 29-module boot) and
             * the stack (0x001FFF00) - see the 117th finding for the
             * measurement. On any load failure, falls back to the
             * real ROM's own (buggy-today) value rather than risk a
             * worse outcome. Only active at PRId=0x1f - dead code at
             * the shipped default PRId=0. Verified via live host-
             * native tracing this DOES change behavior (IOP escapes
             * the exact literal panic-loop PC for the first time),
             * but also reveals a further, deeper blocker - see the
             * 117th finding's honest "not yet a working boot path"
             * section before treating this as more than a genuine
             * partial step. */
            if (st->cop0[15] == 0x1fu &&
                (this_pc == 0xBFC4A39Cu || this_pc == 0xBFC4A44Cu) &&
                st->devtable_pending_image != 0) {
                uint32_t raw = st->devtable_pending_image;
                st->devtable_pending_image = 0;
                if (raw >= 0xBFC00000u) {
                    uint32_t rom_off = raw - 0xBFC00000u;
                    if (st->bios && rom_off < st->bios->size) {
                        uint32_t image_size = st->bios->size - rom_off;
                        if (image_size > 0x8000u) image_size = 0x8000u;
                        uint32_t load_addr = (this_pc == 0xBFC4A39Cu)
                                             ? 0x001C0000u : 0x001C8000u;
                        iop_elf_load_result_t res;
                        const char *err = NULL;
                        int rc = iop_elf_load(st, st->bios->data + rom_off,
                                               image_size, load_addr, &res, &err);
                        if (rc == 0) {
                            tgt = res.entry;
                        }
                    }
                }
            }
            if (rd) LINK(rd); BRANCH_TO(tgt);
        } break;
        case 0x0C: /* SYSCALL - raises a real R3000A exception instead
             * of halting, ported from PCSX2's psxException()
             * (R3000A.cpp): Cause.ExcCode=8 (Syscall, pre-shifted
             * into bits 2-6 as 0x20), EPC=the SYSCALL instruction's
             * own address, PC vectors to 0xBFC00180 (bootstrap) or
             * 0x80000080 (normal) depending on Status.BEV (bit 22),
             * and the 3-level interrupt-enable/kernel-mode bit stack
             * (Status bits 0-5) shifts left by 2 (current->previous,
             * previous->old). NOTE: real hardware/PCSX2 also handles
             * the case where SYSCALL itself executes in a branch
             * delay slot (EPC=pc-4, Cause.BD=1 set) - not modeled
             * here, since this interpreter doesn't track per-step
             * delay-slot state; EPC is always set to the SYSCALL's
             * own address. A real SYSCALL landing in a delay slot is
             * rare in practice, but this is a known, documented
             * simplification, not an oversight. Unlike BREAK (below),
             * this does NOT halt the core - it's a real, successful
             * step, matching real hardware's actual behavior for this
             * instruction (BREAK is kept as this project's own
             * clean-halt-for-testing convention, not changed here). */
        {
            /* Round 29 continued (task #164): real IOP kernel syscall
             * numbers 0x10 and 0x08 - see docs/STATUS.md's 43rd
             * finding for the full derivation. Live-traced (this
             * project's own emulator, cross-checked register state at
             * the exact fault point) for every one of the 13 modules
             * that previously hit is_unconditional_trap_stub()
             * (SSBUSC, DMACMAN, THREADMAN, VBLANK, IOMAN, MODLOAD,
             * SIFCMD, CDVDMAN, SIFINIT: v0=0x10, a0=pointer to the
             * calling module's own local struct, a1=a fixed address
             * 0x00100030 or 0xBF801528 - consistent with a real
             * RegisterLibraryEntries-style kernel call; REBOOT,
             * LOADFILE, CDVDFSV, FILEIO: v0=0x08, a0=3 always).
             *
             * This project's exception vector (0x80000080/0xBFC00180)
             * has no real installed dispatcher for these - it's just
             * a default fallback stub baked into an early module's
             * own ELF segment data (42nd finding), which ends in an
             * unconditional TGE trap. Previously, this project's
             * module loader recognized that trap pattern and
             * abandoned the CALLING module's remaining execution
             * entirely, jumping to the NEXT module in the boot list
             * (is_unconditional_trap_stub()/advance_to_next_module()).
             * That bypass is kept for any OTHER syscall number that
             * still falls through to this same dead end, but for
             * these two specific, now-understood numbers, this
             * project instead applies the EXACT SAME precedent
             * already established for the A0/B0/C0 BIOS-table
             * convention (iop_hle_bios.c) and the BREAK-as-syscall-
             * fallback (tasks #149/#156): intercept BEFORE any real
             * exception is raised, return the same generic default
             * value (0) already used throughout this project for
             * unimplemented real kernel calls, and resume the CALLING
             * module's OWN code at the instruction right after the
             * syscall - so its real init can continue past this call
             * instead of being abandoned. No Cause/EPC/Status field is
             * touched for this path (matching the A0/B0/C0 HLE
             * convention: a pure software intercept, not a real
             * CPU-level exception at all - safe precisely because
             * these are scratch/kernel-internal fields the caller
             * never observes either way). Any OTHER syscall number is
             * completely unaffected and still raises the real
             * exception exactly as before. */
            uint32_t syscall_num = st->gpr[2]; /* $v0 - real IOP kernel syscall-number convention */
            /* Round 29 continued (task #164 continued): after adding
             * 0x10/0x08 handling above, live-traced (this project's
             * own emulator) every one of the remaining 12
             * trap-stub-bypassed modules and found ALL of them now
             * advance past their first syscall to a SECOND real
             * syscall, number 0x14 (a0=0 always; a1 varies per module
             * - a small index/priority-like value for most, one
             * larger address-like value for IOMAN - real semantics
             * not yet identified, e.g. could be a real
             * RegisterIntrHandler/CpuEnableIntr-style kernel call).
             * Same precedent, same generic default-return convention. */
            /* Task #217 (88th finding): real ps2sdk source
             * (iop/system/intrman/src/intrman.c, fetched and cited
             * this round) conclusively identifies syscall 0x08 as
             * `intrman_syscall_08_CpuEnableIntr` - the exact real
             * mechanism backing CpuEnableIntr(). Its real assembly
             * body (`syscall_handler_08_CpuEnableIntr` in the same
             * file's inline exception_system_handler_code) is:
             *   lw   $t0, 0x408($zero)   ; pre-exception Status,
             *                            ; saved by the generic ROM
             *                            ; exception-entry stub
             *   ori  $t0, $t0, 0x404     ; set bit2 (IEp) + bit10
             *                            ; (IM2, this project's
             *                            ; IOP_STATUS_IM2 - see
             *                            ; iop_check_hw_interrupt())
             *   mtc0 $t0, $12
             * ...followed by a real RFE at return (same file's
             * `syscall_handler_00_return_from_exception`, ends in
             * `.word 0x42000010` - COP0 RFE, exactly this project's
             * own already-implemented IOP RFE, task #113), which
             * shifts IEp into IEc. Net, real, documented effect once
             * CpuEnableIntr() returns: Status.IEc=1 and Status.IM2=1.
             *
             * This project's own diagnostic (STATUS.md's 87th
             * finding) proved Status stays 0x00000000 through the
             * entire real BIOS boot specifically because this exact
             * syscall was, until now, treated as a pure no-op by the
             * bypass below (inherited from task #164, which correctly
             * identified the CALLING convention - v0=8, module-
             * agnostic - but not yet this real target semantic).
             * Live-traced (this project's own module-completion
             * tracing, this round): THREADMAN's real _start() runs to
             * completion via a natural return and its own real last
             * action before returning is exactly this call
             * (ps2sdk's iop/system/threadman/src/thcommon.c _start(),
             * last line before `return MODULE_RESIDENT_END;`) -
             * confirming this is reachable, real, exercised code in
             * this project's own boot, not a hypothetical.
             *
             * 0x10 (CpuSuspendIntr) and 0x14 (CpuResumeIntr) are also
             * now known precisely (same file: 0x10 ANDs the saved
             * status with 0x414 as its return value then clears bits
             * 2/4/10; 0x14 clears those same bits then ORs in the
             * caller-supplied prior state) but are deliberately left
             * as before (pure no-ops, no Status effect) - real code
             * always pairs them 1:1 around a critical section, so
             * leaving both inert cannot itself cause a net IEc/IM2
             * change, and implementing only one half in an unproven
             * way risked *introducing* a spurious re-disable after
             * this fix's real CpuEnableIntr() runs. Not applying an
             * unverified fix without a proven failure is this
             * project's own established discipline (see docs/
             * STATUS.md's repeated "not fabricated without citation"
             * notes). */
            if (syscall_num == 0x08u) {
                st->cop0[12] |= (0x1u | 0x400u); /* real net effect of CpuEnableIntr(): IEc (bit0) + IM2 (bit10) */
                st->gpr[2] = 0;
                st->pc = this_pc + 4u;
                st->next_pc = this_pc + 8u;
                break;
            }
            if (syscall_num == 0x10u || syscall_num == 0x14u) {
                st->gpr[2] = 0; /* same generic default-return convention as iop_hle_bios.c / task #149/#156 */
                st->pc = this_pc + 4u;
                st->next_pc = this_pc + 8u;
                break;
            }
            st->cop0[13] = (st->cop0[13] & ~0x7Fu) | 0x20u; /* Cause.ExcCode = 8 (Syscall) */
            st->cop0[14] = this_pc; /* EPC */
            uint32_t vector = (st->cop0[12] & 0x400000u) ? 0xBFC00180u : 0x80000080u; /* Status.BEV */
            st->pc = vector;
            st->next_pc = vector + 4;
            st->cop0[12] = (st->cop0[12] & ~0x3Fu) | ((st->cop0[12] & 0x0Fu) << 2); /* Status stack push */
            st->exception_pending = 1; /* task #156 - see iop_core.h's field comment */
        }
        break;
        case 0x0D: /* BREAK */
            /* Round 29 continued (29th change): if this exact BREAK
             * is reached because a genuine R3000A hardware `syscall`
             * instruction (Cause.ExcCode==8) vectored to the general
             * exception handler and fell through its still-unclaimed
             * default chain (see docs/STATUS.md's 29th finding - this
             * is the SAME underlying architectural gap as task
             * #124/#132/#148: a later module hasn't yet installed a
             * real handler for this kernel-level syscall number,
             * because this project's loader runs one module's ELF
             * and entry point at a time), this project applies the
             * EXACT SAME precedent it already established throughout
             * iop_hle_bios.c for unimplemented A0/B0/C0 BIOS-table
             * calls: return a generic default value (0) to the
             * caller instead of halting. This is NOT a new pattern -
             * it's that same "unimplemented call returns 0" default,
             * extended from the BIOS-table mechanism to the real
             * hardware-level syscall/exception mechanism. Unlike
             * task #148's module-jump bypass, resuming after a
             * syscall is well-defined, ordinary MIPS exception-return
             * semantics (EPC+4, matching a real RFE-terminated
             * handler) - not a jump to a different, unrelated module,
             * so this carries none of that mechanism's own scoping
             * caveats.
             *
             * Any OTHER BREAK (Cause.ExcCode != 8 - e.g. this
             * project's own test suite's long-established direct
             * clean-halt-for-testing convention, which never first
             * executes a real syscall) is completely unaffected and
             * still halts exactly as before.
             *
             * Task #156 fix: this check now ALSO requires
             * exception_pending (see iop_core.h's field comment) -
             * i.e. that this Cause.ExcCode==8 reflects a syscall
             * exception that hasn't been handled (RFE'd) yet, not a
             * stale leftover value from an EARLIER syscall whose
             * handler already ran RFE and returned. Without this,
             * a genuinely-real, distinct BREAK reached later (e.g.
             * this project's own test suite's clean-halt convention,
             * or any real BREAK downstream of an unrelated, already-
             * completed syscall) was wrongly treated as "still
             * unhandled" and resumed at the OLD, already-stale EPC+4
             * instead of halting - a real, reproducible infinite loop
             * (see tests/test_iop_rfe.c: SYSCALL, then RFE at the
             * bootstrap vector, then BREAK - Cause still read 8 since
             * RFE never touches Cause, so this fired incorrectly and
             * resumed execution back near the reset vector's own
             * zeroed/NOP-equivalent memory, which never halts). */
            if (st->exception_pending && (st->cop0[13] & 0x7Cu) == 0x20u) { /* Cause.ExcCode == 8 (Syscall), not yet RFE'd */
                uint32_t epc = st->cop0[14];
                st->gpr[2] = 0; /* $v0 = 0 - same default-return convention as iop_hle_bios.c's unimplemented A0/B0/C0 calls */
                /* RFE-equivalent Status stack pop, identical formula
                 * to this file's own real RFE (COP0 CO-format
                 * funct=0x10) implementation above. */
                st->cop0[12] = (st->cop0[12] & ~0x0Fu) | ((st->cop0[12] & 0x3Cu) >> 2);
                st->exception_pending = 0; /* task #156 - this exception is now considered handled */
                st->pc = epc + 4u;
                st->next_pc = epc + 8u;
                break;
            }
            halt("BREAK");
            return 1;
        case 0x10: /* MFHI */ if (rd) GPR(rd) = st->hi; break;
        case 0x11: /* MTHI */ st->hi = GPR(rs); break;
        case 0x12: /* MFLO */ if (rd) GPR(rd) = st->lo; break;
        case 0x13: /* MTLO */ st->lo = GPR(rs); break;
        case 0x18: /* MULT */ {
            int64_t res = (int64_t)(int32_t)rs32 * (int64_t)(int32_t)rt32;
            st->lo = (uint32_t)(res & 0xFFFFFFFFu);
            st->hi = (uint32_t)(res >> 32);
        } break;
        case 0x19: /* MULTU */ {
            uint64_t res = (uint64_t)rs32 * (uint64_t)rt32;
            st->lo = (uint32_t)(res & 0xFFFFFFFFu);
            st->hi = (uint32_t)(res >> 32);
        } break;
        case 0x1A: /* DIV */
            if (rt32 != 0) {
                st->lo = (uint32_t)((int32_t)rs32 / (int32_t)rt32);
                st->hi = (uint32_t)((int32_t)rs32 % (int32_t)rt32);
            }
            break;
        case 0x1B: /* DIVU */
            if (rt32 != 0) {
                st->lo = rs32 / rt32;
                st->hi = rs32 % rt32;
            }
            break;
        case 0x20: /* ADD */
        case 0x21: /* ADDU */ if (rd) GPR(rd) = rs32 + rt32; break;
        case 0x22: /* SUB */
        case 0x23: /* SUBU */ if (rd) GPR(rd) = rs32 - rt32; break;
        case 0x24: /* AND */  if (rd) GPR(rd) = rs32 & rt32; break;
        case 0x25: /* OR */   if (rd) GPR(rd) = rs32 | rt32; break;
        case 0x26: /* XOR */  if (rd) GPR(rd) = rs32 ^ rt32; break;
        case 0x27: /* NOR */  if (rd) GPR(rd) = ~(rs32 | rt32); break;
        case 0x2A: /* SLT */  if (rd) GPR(rd) = ((int32_t)rs32 < (int32_t)rt32) ? 1 : 0; break;
        case 0x2B: /* SLTU */ if (rd) GPR(rd) = (rs32 < rt32) ? 1 : 0; break;
        case 0x30: /* TGE - Trap if Greater or Equal (signed). Task #150:
             * this opcode was first observed as an "unimplemented
             * SPECIAL funct 0x30" halt at pc=0x800000AC, reached after
             * task #149's syscall-return fix let a second real syscall
             * fall through the still-unclaimed general exception
             * vector and re-walk that low-memory region. Real MIPS
             * trap semantics (condition-taken raises a Trap exception,
             * ExcCode=13 pre-shifted to bits 2-6 as 0x34; condition-
             * not-taken is a pure no-op with no side effects at all -
             * not even implicitly falling through like a branch, since
             * there's no delay slot for trap instructions). Delivery
             * mirrors this file's own existing SYSCALL exception path
             * exactly (EPC=this instruction's own address, PC vectors
             * to 0xBFC00180/0x80000080 depending on Status.BEV, same
             * Status KU/IE stack left-shift-by-2 push) - the same
             * real R3000A exception-delivery mechanism, just a
             * different ExcCode and trigger condition. */
            if ((int32_t)rs32 >= (int32_t)rt32) {
                st->cop0[13] = (st->cop0[13] & ~0x7Fu) | 0x34u; /* Cause.ExcCode = 13 (Trap) */
                st->cop0[14] = this_pc; /* EPC */
                uint32_t vector = (st->cop0[12] & 0x400000u) ? 0xBFC00180u : 0x80000080u; /* Status.BEV */
                st->pc = vector;
                st->next_pc = vector + 4;
                st->cop0[12] = (st->cop0[12] & ~0x3Fu) | ((st->cop0[12] & 0x0Fu) << 2); /* Status stack push */
                st->exception_pending = 1; /* task #156 - see iop_core.h's field comment */
            }
            break;
        default:
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "unimplemented SPECIAL funct 0x%02X (pc=0x%08X)",
                     (unsigned int)funct, (unsigned int)this_pc);
            halt(buf);
            return 1;
        }
        }
        break;

    case 0x01: /* REGIMM */
        switch (rt) {
        case 0x00: /* BLTZ */   if ((int32_t)GPR(rs) < 0)  BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        case 0x01: /* BGEZ */   if ((int32_t)GPR(rs) >= 0) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        case 0x10: /* BLTZAL */ LINK(31); if ((int32_t)GPR(rs) < 0)  BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        case 0x11: /* BGEZAL */ LINK(31); if ((int32_t)GPR(rs) >= 0) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        default:
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "unimplemented REGIMM opcode 0x%02X (pc=0x%08X)",
                     (unsigned int)rt, (unsigned int)this_pc);
            halt(buf);
            return 1;
        }
        }
        break;

    case 0x02: /* J */   BRANCH_TO((this_pc & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2)); break;
    case 0x03: /* JAL */ {
        uint32_t jal_tgt = (this_pc & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2);
        /* Round 77 (117th finding, task #221/#245): 0xBFC4A600 is the
         * real ROM's device-table-entry classifier (see the 116th/
         * 117th findings) - it takes the raw device-table entry
         * pointer in $a0. That raw entry turned out to be a genuine
         * embedded ELF32/MIPS IOP module image (same "IRX" format
         * iop_elf.c already loads for ROMDIR-listed modules - real
         * e_machine=8, vendor e_type=0xFF80, PT_MIPS_IOPMOD segment).
         * Remember it here so the matching JALR call site below (the
         * real ROM code's own "call this device's real init function"
         * step) can ELF-load it for real instead of jumping to
         * whatever raw, unrelocated header field the ROM's un-loaded
         * pointer happens to contain. Only active when PRId is the
         * real retail value - completely inert (zero behavior change)
         * at the default PRId=0, since this ROM code path is never
         * even reached then (see g_iop.cop0[15]'s own header comment). */
        if (st->cop0[15] == 0x1fu && jal_tgt == 0xBFC4A600u) {
            st->devtable_pending_image = GPR(4);
        }
        LINK(31); BRANCH_TO(jal_tgt);
    } break;
    case 0x04: /* BEQ */  if (GPR(rs) == GPR(rt)) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
    case 0x05: /* BNE */  if (GPR(rs) != GPR(rt)) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
    case 0x06: /* BLEZ */ if ((int32_t)GPR(rs) <= 0) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
    case 0x07: /* BGTZ */ if ((int32_t)GPR(rs) > 0)  BRANCH_TO(this_pc + 4 + (imm << 2)); break;

    case 0x08: /* ADDI */
    case 0x09: /* ADDIU */ if (rt) GPR(rt) = rs32 + (uint32_t)imm; break;
    case 0x0A: /* SLTI */  if (rt) GPR(rt) = ((int32_t)GPR(rs) < imm) ? 1 : 0; break;
    case 0x0B: /* SLTIU */ if (rt) GPR(rt) = (GPR(rs) < (uint32_t)imm) ? 1 : 0; break;
    case 0x0C: /* ANDI */  if (rt) GPR(rt) = GPR(rs) & uimm; break;
    case 0x0D: /* ORI */   if (rt) GPR(rt) = GPR(rs) | uimm; break;
    case 0x0E: /* XORI */  if (rt) GPR(rt) = GPR(rs) ^ uimm; break;
    case 0x0F: /* LUI */   if (rt) GPR(rt) = uimm << 16; break;

    case 0x10: /* COP0 */
        switch (rs) {
        case 0x00: /* MFC0 */ if (rt) GPR(rt) = st->cop0[rd]; break;
        case 0x04: /* MTC0 */ st->cop0[rd] = rt32; break;
        case 0x10: /* CO-format (bit 25 set, rs=0x10, rest of rs
             * field zero in the real encoding) - real R3000A/R5900
             * "cofun" ops selected by the low 6 bits (funct). Only
             * RFE (funct=0x10) is real R3000A architecture (no TLB
             * on the IOP, unlike the EE, so TLBR/TLBWI/TLBWR/TLBP
             * are not applicable here and are correctly left
             * unimplemented). This was a genuine, confirmed gap
             * found while investigating why Status.IEc never
             * becomes 1 (see docs/STATUS.md "Round 22"): every real
             * MIPS I exception handler ends in RFE to restore the
             * pre-exception KU/IE mode stack (and re-enable
             * interrupts if they were enabled beforehand) before
             * returning via JR - without this, any real handler
             * that actually completes and returns would have hit
             * this same "unimplemented COP0 sub-opcode" halt below,
             * silently masking whatever the real RAM[0x100] handler
             * chain does after being fixed. Ported from PCSX2's
             * R3000A.cpp psxException()'s RFE case:
             * `Status = (Status & ~0xF) | ((Status & 0x3C) >> 2)`
             * - shifts the 2-bit-pair KU/IE "previous" and "old"
             * fields down into "current"/"previous", leaving the
             * top "old" pair (bits 4-5) untouched, mirroring the
             * exception-entry push this project already implements
             * (`(cop0[12] & 0x0F) << 2` into bits 2-5, clearing 0-1). */
            switch (funct) {
            case 0x10: /* RFE */
                st->cop0[12] = (st->cop0[12] & ~0x0Fu) | ((st->cop0[12] & 0x3Cu) >> 2);
                /* Task #156: this exception is now considered handled
                 * - see iop_core.h's exception_pending field comment
                 * and BREAK's own updated check below. */
                st->exception_pending = 0;
                break;
            default:
            {
                char buf[96];
                snprintf(buf, sizeof(buf), "unimplemented COP0 CO-format op (funct=0x%02X, pc=0x%08X)",
                         (unsigned int)funct, (unsigned int)this_pc);
                halt(buf);
                return 1;
            }
            }
            break;
        default:
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "unimplemented COP0 sub-opcode (rs=0x%02X, pc=0x%08X)",
                     (unsigned int)rs, (unsigned int)this_pc);
            halt(buf);
            return 1;
        }
        }
        break;

    case 0x20: /* LB */  if (rt) GPR(rt) = (uint32_t)(int32_t)(int8_t)iop_mem_read8(st, rs32 + imm); else iop_mem_read8(st, rs32 + imm); break;
    case 0x21: /* LH */  if (rt) GPR(rt) = (uint32_t)(int32_t)(int16_t)iop_mem_read16(st, rs32 + imm); else iop_mem_read16(st, rs32 + imm); break;
    case 0x23: /* LW */  if (rt) GPR(rt) = iop_mem_read32(st, rs32 + imm); else iop_mem_read32(st, rs32 + imm); break;
    case 0x24: /* LBU */ if (rt) GPR(rt) = iop_mem_read8(st, rs32 + imm); else iop_mem_read8(st, rs32 + imm); break;
    case 0x25: /* LHU */ if (rt) GPR(rt) = iop_mem_read16(st, rs32 + imm); else iop_mem_read16(st, rs32 + imm); break;

    case 0x22: /* LWL */ {
        uint32_t addr = rs32 + imm;
        uint32_t shift = (addr & 3) << 3;
        uint32_t mem = iop_mem_read32(st, addr & ~3u);
        if (rt) GPR(rt) = (rt32 & (0x00FFFFFFu >> shift)) | (mem << (24 - shift));
    } break;
    case 0x26: /* LWR */ {
        uint32_t addr = rs32 + imm;
        uint32_t shift = (addr & 3) << 3;
        uint32_t mem = iop_mem_read32(st, addr & ~3u);
        if (rt) GPR(rt) = (rt32 & (0xFFFFFF00u << (24 - shift))) | (mem >> shift);
    } break;

    case 0x28: /* SB */ iop_mem_write8(st, rs32 + imm, (uint8_t)GPR(rt)); break;
    case 0x29: /* SH */ iop_mem_write16(st, rs32 + imm, (uint16_t)GPR(rt)); break;
    case 0x2B: /* SW */ iop_mem_write32(st, rs32 + imm, GPR(rt)); break;

    case 0x2A: /* SWL */ {
        uint32_t addr = rs32 + imm;
        uint32_t shift = (addr & 3) << 3;
        uint32_t mem = iop_mem_read32(st, addr & ~3u);
        iop_mem_write32(st, addr & ~3u, (rt32 >> (24 - shift)) | (mem & (0xFFFFFF00u << shift)));
    } break;
    case 0x2E: /* SWR */ {
        uint32_t addr = rs32 + imm;
        uint32_t shift = (addr & 3) << 3;
        uint32_t mem = iop_mem_read32(st, addr & ~3u);
        iop_mem_write32(st, addr & ~3u, (rt32 << shift) | (mem & (0x00FFFFFFu >> (24 - shift))));
    } break;

    /* Round 128 (task #172/#196, 168th finding): CACHE (primary
     * opcode 0x2F) - standard MIPS I/II instruction (public ISA,
     * independent of any BIOS-specific data), real IOP kernel code
     * issues it routinely (e.g. around DMA buffer boundaries) to
     * invalidate/writeback cache lines. This project models no real
     * instruction/data cache at all for either core (already-
     * established, honestly-scoped simplification used throughout -
     * same rationale as the "no cycle-accurate timing" scope), so
     * CACHE is architecturally correct as a pure no-op here: with no
     * cache to act on, there is nothing for it to do. Found via
     * host-native instrumentation showing the IOP halting on this
     * exact opcode immediately after Round 127's timing fix unblocked
     * further boot progress. */
    case 0x2F: /* CACHE */ break;

    default:
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "unimplemented primary opcode 0x%02X (pc=0x%08X)",
                 (unsigned int)op, (unsigned int)this_pc);
        halt(buf);
        return 1;
    }
    }

#undef GPR
#undef BRANCH_TO
#undef LINK

    st->gpr[0] = 0;

    /* Round 22: real hardware-interrupt delivery, checked at the end
     * of every real (non-HLE-trap) instruction step - see
     * iop_check_hw_interrupt()'s own comment above for the full
     * citation trail. Uses st->pc (already advanced to the next
     * not-yet-executed instruction by this function's own prologue,
     * and possibly redirected by a branch/jump this same step) as
     * EPC, matching this project's existing SYSCALL exception's
     * "no delay-slot tracking" simplification. */
    iop_check_hw_interrupt(st, st->pc);

    st->instructions_executed++;
    return 0;
}

/* Public single-instruction step - see ee_core_step()'s comment in
 * ee_core.c for why this exists (source/core/system.c's interleaved
 * scheduler). */
int iop_core_step(void)
{
    if (g_iop.halted)
        return 1;

    /* Task #214/#215 continuation (85th/86th findings): real IOP
     * counters/timers run off the system clock, independent of
     * whatever the CPU itself is doing - this is called
     * unconditionally, even while `idle` below, precisely because
     * that's the real mechanism that lets a genuinely idle IOP
     * thread scheduler wake back up (a periodic timer-tick interrupt
     * fires regardless of CPU activity). See iop_timers.h's own
     * extensive citation trail for the full real-hardware grounding
     * and this project's honestly-scoped-down subset of it. */
    iop_timers_tick();

    /* Task #216 continuation: real IOP VBLANK, same unconditional-
     * even-while-idle rationale as iop_timers_tick() above - see
     * iop_check_vblank()'s own doc comment for the full citation
     * trail. */
    iop_check_vblank(&g_iop);

    /* Task #179 continued: real IOP hardware never halts after boot -
     * see the `idle` field's own doc comment in iop_core.h for the
     * full citation trail. While idle, skip real fetch/decode/execute
     * entirely (nothing genuine to run - the trampoline address isn't
     * real code) and only re-run the same hardware-interrupt check
     * every other instruction step already gets. If that check finds
     * a real pending interrupt, it vectors pc/next_pc into the normal
     * exception vector itself (see iop_check_hw_interrupt() above) -
     * clear `idle` so the very next call resumes genuine fetch/decode/
     * execute there, running whatever real handler code the modules
     * installed before returning. */
    if (g_iop.idle) {
        uint8_t pending_before = g_iop.exception_pending;
        iop_check_hw_interrupt(&g_iop, g_iop.pc);
        if (!pending_before && g_iop.exception_pending)
            g_iop.idle = 0; /* a real interrupt just vectored us - resume real execution next call */
        return 0;
    }

    return iop_step();
}

void iop_core_run(void)
{
    while (!g_iop.halted) {
        if (iop_step())
            break;
    }

    printf("\n[!] IOP core halted after %llu instructions at pc=0x%08lX\n",
           (unsigned long long)g_iop.instructions_executed, (unsigned long)g_iop.pc);
    printf("    reason: %s\n", g_iop.halt_reason[0] ? g_iop.halt_reason : "(unknown)");
}

void iop_core_shutdown(void)
{
    if (g_iop.ram) {
        free(g_iop.ram);
        g_iop.ram = NULL;
    }
}
