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
#include "core/hw/iop_asyncio.h" /* Round 153 (task #307): real async I/O queue - see header for full trace/citation */
#include "core/hw/iop_sio2.h" /* Round 135 (175th finding): SIO2 controller/memory-card serial interface - see header for full trace/citation */
#include "core/hw/iop_spu_legacy.h" /* Round 136 (177th finding): PS1-legacy SPU register block - see header for full trace/citation */
#include "core/hw/iop_hle_bios.h"
#include "core/hw/iop_hle_events.h"
#include "core/hw/iop_hle_modules.h"
#include "core/hw/iop_hle_intr.h"
#include "core/hw/iop_hle_thread.h" /* Round 389: real THREADMAN thread scheduler/semaphore HLE */
#include "core/hw/iop_hle_heap.h" /* Round 421: real SYSMEM heap-export sentinel gates */
#include "core/hw/iop_module_loader.h" /* real IOP module/IRX loader - task #92 */
#include "core/hw/iop_excb.h" /* real exception-handler priority chains at RAM[0x100] - Round 22 */
#include "core/hw/iop_icfg.h" /* real ICFG register / EE INTC_SBUS raise - task #214, 85th finding */
#include "core/hw/iop_heap.h" /* Round 401: real SYSMEM free-list heap allocator port */
#include "core/hw/iop_elf.h" /* real ELF32/MIPS IOP loader - reused for device-table embedded images, task #221/#245 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#define IOP_RAM_SIZE (2 * 1024 * 1024)
/* Round 417 (task #152): real LOADCORE init code (disassembly-
 * confirmed, docs/STATUS.md Round 417) computes its OWN kernel stack
 * pointer directly from boot_info's real RAM_MB field:
 * `sp = (RAM_MB) << 20` - a genuine, deliberate real MIPS-kernel
 * convention (megabytes-to-bytes shift), not a bug, and not
 * something this project's own dispatcher-assigned INITIAL_SP (see
 * iop_module_loader.c) has any influence over, since LOADCORE
 * unconditionally overwrites $sp with this computed value the moment
 * its own entry code runs. With this project's honestly-reported
 * `BOOT_INFO_RAM_MB=2`, that computation is exactly 0x00200000 -
 * this project's own modeled RAM's exact top boundary, leaving real,
 * disassembly-confirmed kernel code (this specific registration-list
 * function's own nested stack frame) zero headroom to work with.
 * Rather than under-report guest-visible RAM (which would be
 * observable/incorrect for any other real code path that legitimately
 * queries total memory), this project's own BACKING allocation is
 * widened by a small, guest-invisible guard region - the reported
 * "2MB total memory" boundary guest code sees via boot_info/SYSMEM's
 * $a0 is unchanged, but this project's own `st->ram`/`st->ram_size`
 * now provide real backing bytes slightly past that boundary, so a
 * real kernel stack computed to sit exactly at the reported top of
 * RAM (as real LOADCORE's own code legitimately does) doesn't
 * silently fail on every read/write the instant it needs any stack
 * space at all. */
#define IOP_RAM_GUARD_SIZE 0x00004000u
#define IOP_RAM_BACKING_SIZE (IOP_RAM_SIZE + IOP_RAM_GUARD_SIZE)
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

    /* Round 135 (task #172/#292, 175th finding): SIO2 - see
     * iop_sio2.h for the full citation trail. */
    uint8_t sio2_val;
    if (iop_sio2_mmio_read8(addr, &sio2_val))
        return sio2_val;

    uint8_t *p = iop_mem_ptr(st, addr, 1);
    return p ? *p : 0;
}

uint16_t iop_mem_read16(iop_state_t *st, uint32_t addr)
{
    uint16_t spu2_val;
    if (iop_spu2_mmio_read16(addr, &spu2_val))
        return spu2_val;
    /* Round 136 (177th finding): PS1-legacy SPU - see
     * iop_spu_legacy.h for the full citation trail. */
    uint16_t spu_legacy_val16;
    if (iop_spu_legacy_mmio_read16(addr, &spu_legacy_val16))
        return spu_legacy_val16;

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
    uint32_t spu_legacy_val32;
    if (iop_spu_legacy_mmio_read32(addr, &spu_legacy_val32))
        return spu_legacy_val32;
    uint32_t icfg_val;
    if (iop_icfg_mmio_read32(addr, &icfg_val))
        return icfg_val;
    /* Round 135 (175th finding): SIO2 - see iop_sio2.h. */
    uint32_t sio2_val32;
    if (iop_sio2_mmio_read32(addr, &sio2_val32))
        return sio2_val32;

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

    /* Round 135 (175th finding): SIO2 - see iop_sio2.h. */
    if (iop_sio2_mmio_write8(addr, val))
        return;

    uint8_t *p = iop_mem_ptr(st, addr, 1);
    if (p) *p = val;
}

void iop_mem_write16(iop_state_t *st, uint32_t addr, uint16_t val)
{
    if (iop_spu2_mmio_write16(addr, val))
        return;
    /* Round 136 (177th finding): PS1-legacy SPU - see
     * iop_spu_legacy.h for the full citation trail. */
    if (iop_spu_legacy_mmio_write16(addr, val))
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
    if (iop_spu_legacy_mmio_write32(addr, val))
        return;
    if (iop_icfg_mmio_write32(addr, val))
        return;
    /* Round 135 (175th finding): SIO2 - see iop_sio2.h. */
    if (iop_sio2_mmio_write32(addr, val))
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
    iop_asyncio_init(); /* Round 153: real async I/O queue/channel dispatch - see core/hw/iop_asyncio.h */
    iop_sio2_init(); /* SIO2 controller/memory-card interface - Round 135, see core/hw/iop_sio2.h */
    iop_spu_legacy_init(); /* PS1-legacy SPU register block - Round 136, see core/hw/iop_spu_legacy.h */
    iop_hle_bios_init(); /* IOP BIOS syscall trap (A0/B0/C0) - see core/hw/iop_hle_bios.h */
    iop_hle_events_init(); /* Round 142: real B0-table Event subsystem - see core/hw/iop_hle_events.h */
    iop_hle_modules_init(); /* IOP module registry scaffold - see core/hw/iop_hle_modules.h */
    iop_hle_intr_init(); /* Round 109: clean-room RegisterIntrHandler/RegisterExceptionHandler HLE table - see core/hw/iop_hle_intr.h */
    iop_hle_thread_init(); /* Round 389: real THREADMAN thread scheduler/semaphore HLE - see core/hw/iop_hle_thread.h */
    iop_module_loader_reset(); /* real module/IRX boot sequencer - see core/hw/iop_module_loader.h */
    iop_icfg_init(); /* real ICFG register - task #214, 85th finding */
    iop_heap_init(); /* Round 401: real SYSMEM free-list heap allocator port - see core/hw/iop_heap.h */

    g_iop.ram = memalign(32, IOP_RAM_BACKING_SIZE);
    if (!g_iop.ram)
        return -1;
    memset(g_iop.ram, 0, IOP_RAM_BACKING_SIZE);
    g_iop.ram_size = IOP_RAM_BACKING_SIZE; /* Round 417 - see IOP_RAM_BACKING_SIZE's comment above; guest-visible reported size (BOOT_INFO_RAM_MB, SYSMEM_ENTRY_TOP_OF_MEMORY) is unchanged */

    iop_dma_bind_iop_ram(g_iop.ram, g_iop.ram_size); /* Round 199: lets the SIF0 CHCR-kick path read real IOP RAM source bytes - see core/hw/iop_dma.h */

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

    /* Round 172 (task #337, 212th finding): EAGERLY invoke the real
     * IOP module/IRX loader (core/hw/iop_module_loader.h, task #92)
     * here, unconditionally, instead of relying solely on
     * iop_step()'s lazy "PC escaped to unfetchable memory" fallback
     * trigger (still present below, unchanged, as a safety net for
     * synthetic/test BIOS images that don't have a valid ROMDIR).
     *
     * WHY: host-native diagnostic this round (scan_state.c/
     * eager_boot.c, not committed - see STATUS.md's 212th finding)
     * conclusively confirmed the Round 59/91st-finding comment two
     * screens above is exactly right and still true today - with a
     * real SCPH-10000 BIOS + real Tekken Tag Tournament (Europe)
     * (Demo) disc, running system_run_interleaved() from a freshly-
     * inited state for 45,000,000+ IOP instructions NEVER once
     * triggers the lazy fallback (modules_attempted stayed 0 the
     * entire time) - the interpreted real ROM bootstrap code always
     * settles into already-resident, real, fetchable RAM content
     * (the long-documented 0x00032C58-0x00032D50 poll region) without
     * its PC ever actually escaping to unmapped memory, because
     * whatever real mechanism the ROM uses to copy each module's
     * bytes into RAM in the first place isn't modeled by this
     * project's interpreted-ROM-bootstrap path. The SAME diagnostic
     * showed that calling iop_module_loader_boot() eagerly, before
     * any ROM bootstrap instruction ever executes, successfully
     * loads all 29 real ROMDIR/IOPBTCONF modules (355/355 imports
     * resolved, 0 unresolved - identical, already-tested machinery
     * to task #92, just invoked earlier), and that running forward
     * from there reaches Status.IEc=1 for the very first time ever
     * in this trajectory (CpuEnableIntr, task #217's 88th finding,
     * finally gets called for real by real, genuinely-loaded module
     * code) and a brand-new IOP halt wall (unimplemented primary
     * opcode 0x3F at pc=0x8000041C) further than the old lazy-only
     * path has ever reached.
     *
     * HONEST SCOPE NOTE (shortcut, not exact hardware emulation):
     * this skips interpreting whatever real ROM hardware-bring-up
     * instructions normally execute between the reset vector and the
     * ROM's own module-loading phase (this project doesn't currently
     * model the real bytes-into-RAM copy mechanism that phase relies
     * on anyway, so those instructions were never doing anything
     * beyond driving this project's own already-known-incomplete
     * memory/MMIO model). This mirrors the same, already-established
     * project convention as Round 171's EE game-ELF-entry shortcut
     * and iso_loader.c's mount_iso() - jump straight to a REAL,
     * genuinely-verified-correct state (real modules, real ELF
     * loading/relocation/import resolution, real entry-point
     * execution) rather than waiting on a not-yet-modeled exact
     * mechanism. iop_module_loader_boot() itself is unchanged,
     * already-tested code (task #92); only the call site/timing is
     * new. Safe for synthetic/test BIOS images: iop_module_loader_
     * boot() returns 0 immediately (leaving st->pc/next_pc untouched)
     * whenever ROMDIR/IOPBTCONF/the first listed module can't be
     * found, exactly as its existing header comment documents - the
     * four existing tests that manage this loader's one-shot state
     * explicitly (test_iop_module_loader_bootinfo.c,
     * test_iop_loadcore_panic_bypass.c,
     * test_iop_registration_walk_panic_bypass.c,
     * test_iop_trap_stub_bypass.c) already call iop_module_loader_
     * reset() themselves immediately after iop_core_init(), which
     * clears the one-shot "attempted" flag this eager call sets, so
     * their own explicit iop_module_loader_boot() calls are
     * unaffected. */
    iop_module_loader_boot(&g_iop);

    return 0;
}

static void halt(const char *reason)
{
    g_iop.halted = 1;
    /* Round 340 (incidental, found while rebuilding for the interrupt-
     * priority fix above): same real category of fix as Round 332's
     * iop_module_loader.c one - strncpy's own semantics can't
     * statically prove null-termination when the source string might
     * be exactly (or longer than) the destination size, which is
     * exactly what triggered a real -Wstringop-truncation warning
     * here. Unlike Round 332's case there was no separate explicit
     * terminating write already guaranteeing safety, so this is a
     * genuine (if low-probability in practice, since call sites pass
     * short fixed string literals) latent non-termination risk, not
     * just a cosmetic warning - fixed properly by explicitly writing
     * the terminator after a bounded copy, rather than just silencing
     * the warning. */
    strncpy(g_iop.halt_reason, reason, sizeof(g_iop.halt_reason) - 1);
    g_iop.halt_reason[sizeof(g_iop.halt_reason) - 1] = '\0';
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
     * requirement, point (b).
     *
     * Round 174 (task #339, 214th finding): live host-native tracing
     * against the real Round 172/173 boot trajectory found a genuine,
     * previously-hidden gap in this priority scheme, not a missing
     * handler. iop_dma_signal_channel_done() (iop_dma.c, Round 113/
     * 114) already raises BOTH the shared real hardware line
     * (iop_intc_raise(3) - the one real IOP_IRQ_DMA master line, bit
     * 3) AND the corresponding per-channel soft line (Round 112's
     * istat_hi/imask_hi range - here bit 42, IOP_IRQ_DMA_SIF0) for
     * the exact same real completion event - see iop_intc.h's own
     * citation that this split is "purely about HOW the source is
     * identified/multiplexed, not whether it drives the same
     * physical Cause.IP2 line". A live trace confirmed no module ever
     * calls RegisterIntrHandler(3, ...)/registers an ExCB chain for
     * the raw shared line (plausibly because real INTRMAN keeps that
     * master-line demux internal to its own un-executed real C code,
     * per this project's clean-room scope), so every dispatch for
     * irq=3 fell through to the fixed default vector - and, because
     * the old code only ever consulted the soft range in an `else`
     * branch (reached only when the LOW range had nothing pending at
     * all), the already-real, already-registered handler at
     * `intr_handler_addr[42]` (confirmed nonzero, =0x00117cb4, in the
     * same trace) was never given a chance, even though it was
     * simultaneously pending. Over thousands of these missed
     * dispatches the module loader's own "no more real code to
     * resume" bypass (is_unconditional_trap_stub(), iop_module_
     * loader.c) kept re-matching stale bytes sitting at the shared
     * default-vector address and re-entering its "module boot
     * sequence complete" path every cycle, which is what actually
     * produced the freeze/crawl pattern this round's own earlier
     * diagnostic mis-attributed to a THREADMAN thread-context/TCB
     * gap (213th finding) - a real, honest correction, not a
     * fabricated new mechanism: the underlying data (THREADMAN's
     * irq=16 Timer5 handler dispatching cleanly) was real, but the
     * NEW wall immediately afterward has a different, now-identified
     * cause. Fix: after a low-range irq's dispatch attempts both
     * fail, also try the soft range if it is independently pending,
     * before falling through to the default vector - same "give a
     * real registered handler a chance first" precedent already
     * applied to the low range above, just no longer gated on the
     * low range being completely empty. */
    {
        /* Round 340 (task #423 continuation, direct extension of the
         * already-shipped Round 174 precedent): host-native tracing
         * against the restored real BIOS/disc scratch driver (Round
         * 335-339's investigation chain) caught a genuine, previously
         * hidden priority-starvation gap, one level deeper than the
         * low-range-vs-soft-range boundary Round 174 already fixed.
         * The OLD code below only ever computed the SINGLE lowest
         * pending+unmasked low-range bit and, if BOTH real dispatch
         * mechanisms (RegisterIntrHandler table, then the older ExCB
         * chain) failed to find a real handler for that one bit, gave
         * up on the entire low range and fell straight through to the
         * soft range / fixed-vector default - even when a DIFFERENT,
         * higher-numbered low-range bit (lower real priority, but
         * still pending+unmasked) already had a real, ready,
         * previously-registered handler.
         *
         * Directly observed: CDVDMAN's own real IRQ2 handler
         * (registered via a real RegisterIntrHandler(2,...) call
         * during CDVDMAN's own one-time real init - see Round 338)
         * sat ready and non-zero for the entire remainder of a
         * 90-million-instruction fine-grained trace, while VBLANK_START
         * (bit 0, real, always the numerically-lowest pending bit
         * whenever a VBLANK is pending, which this project's own
         * already-correct iop_check_vblank() raises unconditionally
         * every real frame - see this file's own IOP_INTC_IRQ_VBLANK_
         * START citation above) had NO real handler registered yet at
         * this point in the trace and kept re-falling-through to the
         * generic fixed-vector default stub every single pass, which
         * (correctly, per its own Round 129/131 design) resumes
         * execution at EPC without ever touching intc->istat - so the
         * same VBLANK_START bit simply stays pending forever, and the
         * OLD code's "only try the single lowest bit" logic meant
         * IRQ2's already-ready handler was never even attempted,
         * confirmed via direct instrumentation (0 dispatches to
         * 0x00120d60 across the entire trace despite a real,
         * correctly-set-up handler sitting there).
         *
         * Fix: mirror Round 174's own already-shipped, already-proven
         * "give the next candidate its own independent chance rather
         * than blocking on an earlier one's failure" design, applied
         * one level deeper - try EVERY pending+unmasked low-range bit,
         * in ascending (real-hardware priority) order, until one
         * actually dispatches via either real mechanism, rather than
         * stopping after the first (numerically lowest) bit's
         * dispatch attempt fails. This changes nothing about real
         * priority ORDER (a lower-numbered bit with a real handler
         * still always wins over a higher-numbered one, exactly as
         * before) - it only stops a real, ready handler for a
         * higher-numbered bit from being starved forever by a lower-
         * numbered bit that has no real handler at all yet. */
        uint32_t pending = intc->istat & intc->imask;
        while (pending) {
            uint32_t lowest_bit = pending & (~pending + 1u); /* isolate lowest set bit */
            uint32_t irq = 0;
            uint32_t probe = lowest_bit;
            while (!(probe & 1u)) { probe >>= 1; irq++; }
            if (iop_hle_intr_dispatch_interrupt(st, irq))
                return;
            /* Round 168 (see core/hw/iop_excb.h's "ROUND 168 UPDATE"
             * comment for the full citation trail): real module code
             * can register a handler via the OLDER SysEnqIntRP/ExCB
             * mechanism instead of the newer RegisterIntrHandler API
             * just checked above - both are real, coexisting IOP
             * kernel APIs. A live, host-native trace (208th finding)
             * proved this project's own CD-ROM driver does exactly
             * that (3 real SysEnqIntRP calls, zero RegisterIntrHandler
             * calls, for the entire boot) - so this second fallback is
             * not speculative, it fixes an observed, real gap. */
            if (iop_excb_dispatch_interrupt(st, irq))
                return;
            pending &= ~lowest_bit; /* Round 340: this bit had no real handler either way - try the next-lowest pending bit instead of giving up on the whole low range */
        }
        /* Round 112 (soft-range consultation) + Round 174 (no longer
         * gated on `!pending` - see this block's own doc comment
         * above for the full citation trail): the real 0-31 hardware
         * range still takes numeric priority when it actually
         * resolves to a real handler (unchanged - the early `return`
         * above still wins whenever the low range dispatches
         * successfully), but a low-range irq that fails BOTH
         * dispatch mechanisms no longer blocks the soft range from
         * getting its own, independent chance at the same tick. */
        {
            uint32_t soft_pending = intc->istat_hi & intc->imask_hi;
            if (soft_pending) {
                uint32_t bit = 0;
                while (!(soft_pending & 1u)) { soft_pending >>= 1; bit++; }
                if (iop_hle_intr_dispatch_interrupt(st, 32u + bit))
                    return;
                if (iop_excb_dispatch_interrupt(st, 32u + bit)) /* Round 168 - see above */
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

    /* Round 389: real THREADMAN thread scheduler/semaphore HLE - see
     * core/hw/iop_hle_thread.h. Same "intercept before fetch" spot as
     * every other HLE table above; this one can also perform a full
     * context switch (a plain register-file struct copy - see that
     * file's own header comment) before returning, so st->pc may end
     * up pointing at a completely different thread's own resumed
     * code, not just the syscall's own $ra. */
    if (iop_hle_thread_try_handle(st, pc)) {
        st->instructions_executed++;
        return 0;
    }

    /* Round 421 (task #160, docs/STATUS.md Round 420 root cause):
     * real SYSMEM heap-management export gates (AllocSysMemory/
     * FreeSysMemory/QueryMemSize/QueryMaxFreeMemSize/
     * QueryTotalFreeMemSize) - see core/hw/iop_hle_heap.h. Same
     * "intercept before fetch" spot as every other HLE table above;
     * redirects real module heap calls to this project's own
     * already-tested synthetic heap model (iop_heap.c) instead of
     * real, un-coordinated SYSMEM ROM code whose heap arena collides
     * with this project's own separate module-loading bump_alloc()
     * arena. */
    if (iop_hle_heap_try_handle(st, pc)) {
        st->instructions_executed++;
        return 0;
    }

    /* Round 168: real ExCB-chain dispatch return trampoline - see
     * core/hw/iop_excb.h's "ROUND 168 UPDATE" comment. Same
     * "intercept before fetch" spot as the RegisterIntrHandler
     * trampoline just above. */
    if (iop_excb_try_handle(st, pc)) {
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
        if (exc_code == 0x08u || exc_code == 0x09u || exc_code == 0x0Du || exc_code == 0x0Au) {
            /* Syscall / Breakpoint / Trap / Reserved Instruction
             * (Round 175, task #340) - all synchronous, non-
             * restartable: skip past the triggering instruction and
             * return the same generic "unimplemented, default value"
             * result this project already uses for every other
             * unclaimed BIOS/syscall call site. Reserved Instruction
             * joins this list for the identical reason Syscall/BP/
             * Trap already do - re-running the same instruction would
             * refire the identical exception forever. */
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

    /* Round 173 (task #338, 213th finding): a second, narrower class
     * of "PC wandered into memory this project never populates" -
     * distinct from Round 14's guard above (which only catches
     * addresses genuinely OUTSIDE modeled RAM/ROM). This one catches
     * addresses INSIDE modeled IOP RAM that are nonetheless never
     * populated by this project's current boot model: real IOP RAM
     * below 0x00100000 (iop_module_loader.c's own BUMP_BASE - every
     * real module this project ever loads is placed at or above this
     * address, by construction) is genuine, real "kernel reserved
     * workspace" on actual hardware (low kernel data, including the
     * real Thread Control Block table THREADMAN's own real scheduler
     * uses for context switches - see ps2sdk's iop/system/threadman
     * sources) - but since Round 172 made real module code (and,
     * transitively, real IOP interrupt delivery) reachable for the
     * first time, THREADMAN's real Timer5 scheduler-tick ISR
     * (RegisterIntrHandler+EnableIntr(16), confirmed via host-native
     * tracing this round - see docs/STATUS.md's 213th finding) now
     * genuinely runs, correctly, thousands of times in a row (a real,
     * positive validation of Round 172's fix) - until it eventually
     * performs what is very likely a genuine thread-context load,
     * reading this project's never-populated (all-zero) low-memory
     * TCB-equivalent area as if it were valid data. This project has
     * no real multi-threading/TCB model (out of scope for a single
     * round - a substantial undertaking), so PC free-running through
     * that all-zero region (executing each zero word as a literal
     * NOP, exactly as Round 14's own header comment already
     * describes for the analogous out-of-range case) is the honest,
     * unavoidable result. Detecting the escape immediately - instead
     * of letting it execute dozens of incidental NOPs before
     * coincidentally hitting non-zero bytes and halting on a
     * confusing, unrelated-looking "unimplemented opcode" message
     * (exactly Round 14's own precedent for why an immediate,
     * clearly-labeled halt beats silent wandering) - is a small,
     * strictly-diagnostic-only change: it does not alter behavior on
     * any currently-succeeding path (real module code always runs at
     * pc >= BUMP_BASE; the handful of already-modeled low sentinel/
     * trampoline addresses below BUMP_BASE are excluded below), only
     * the clarity of an already-terminal failure. */
    {
        static uint64_t s_zero_run = 0;
        /* Must compare the KSEG-masked PHYSICAL address, not the raw
         * pc - real IOP code (like this same low-memory crawl) is
         * routinely reached via KSEG0 (0x80000000-based) or KSEG1
         * addresses, never bare physical ones. Same masking
         * convention as this function's own pc_is_fetchable check
         * immediately above (`phys = pc & 0x1FFFFFFFu`). */
        uint32_t phys_pc = pc & 0x1FFFFFFFu;
        int in_modeled_low_region =
            (phys_pc < 0x00000100u) ||                    /* project HLE trap/trampoline sentinels (A0/B0/C0, 0xE4/0xE8/0xEC/0xF0 - see iop_hle_bios.h/iop_hle_intr.h/iop_excb.h) */
            (phys_pc >= 0x0000E000u && phys_pc < 0x00010000u);  /* IOP_EXCB_ARRAY_ADDR / real "Kernel Memory" region - see iop_excb.h */
        if (phys_pc < 0x00100000u && !in_modeled_low_region) {
            uint32_t word = iop_mem_read32(st, pc);
            if (word == 0u) {
                s_zero_run++;
                if (s_zero_run >= 8u) {
                    static char msg2[160];
                    snprintf(msg2, sizeof(msg2),
                             "PC wandered into unpopulated low IOP kernel memory 0x%08lX (real thread-context gap - STATUS.md round 173)",
                             (unsigned long)pc);
                    halt(msg2);
                    return 1;
                }
            } else {
                s_zero_run = 0;
            }
        } else {
            s_zero_run = 0;
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
            /* Round 752 (task #735): real R3000A/MIPS I hardware never
             * stops executing just because it decoded a BREAK - it
             * raises a genuine Breakpoint exception (ExcCode 9) and
             * vectors through the normal exception path, exactly like
             * every other trap-like class already implemented in this
             * file (SYSCALL above, TGE/Trap and Reserved Instruction
             * below). This project's own EE core already made this
             * exact fix (see ee_core.c's SPECIAL funct 0x0D case,
             * task #178) after finding a real, intentional BREAK
             * physically present in the BIOS image - the unconditional
             * halt() here was the same kind of pragmatic placeholder,
             * predating real IOP exception delivery, that task #178
             * already identified and fixed on the EE side.
             *
             * Round 751's major finding motivates fixing this now: a
             * real, ordinary divide-by-zero guard idiom in genuine IOP
             * kernel/module code (0x00119650-0x0011977C, reached deep
             * in the GT3 checkpoint chain past 6B instructions) hits
             * this BREAK when the divisor is zero. Real hardware would
             * vector to the kernel's installed Breakpoint handler
             * (which may resume past it, matching common real debug-
             * trap semantics) rather than permanently killing the IOP
             * core - which is exactly what was silently happening
             * here, deadlocking every EE-side SBUS mailbox-flag poll
             * loop that depends on the IOP staying alive.
             *
             * Mirrors this file's own established general-exception-
             * vector mechanism exactly (EPC=this instruction's own
             * address, PC vectors to 0xBFC00180/0x80000080 depending
             * on Status.BEV, same Status KU/IE stack left-shift-by-2
             * push) - identical in form to the SYSCALL/TGE/Reserved-
             * Instruction cases above/below, just ExcCode=9 instead. */
            st->cop0[13] = (st->cop0[13] & ~0x7Fu) | 0x24u; /* Cause.ExcCode = 9 (Breakpoint) */
            st->cop0[14] = this_pc; /* EPC */
            {
                uint32_t vector = (st->cop0[12] & 0x400000u) ? 0xBFC00180u : 0x80000080u; /* Status.BEV */
                st->pc = vector;
                st->next_pc = vector + 4;
            }
            st->cop0[12] = (st->cop0[12] & ~0x3Fu) | ((st->cop0[12] & 0x0Fu) << 2); /* Status stack push */
            st->exception_pending = 1; /* task #156 - see iop_core.h's field comment */
            break;
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
            /* Round 175 (task #340, 215th finding): real R3000A/MIPS
             * I hardware does not halt when it decodes an undefined
             * SPECIAL funct - it raises a genuine Reserved
             * Instruction exception (ExcCode 0x0A, a universal, publicly
             * documented base MIPS I architecture feature - System V
             * ABI MIPS Processor Supplement / any public R3000A
             * reference - not specific to any BIOS content). Delivery
             * mirrors this file's own existing SYSCALL/TRAP exception
             * paths exactly (same EPC/vector/Status-stack-push
             * mechanism, already implemented and tested above) - the
             * only difference is the ExcCode value and trigger
             * condition. Reached via real, genuinely-executing module
             * code after Round 174's interrupt-dispatch fix let
             * execution reach further than ever before (pc=0x00117CC4,
             * per docs/STATUS.md's 215th finding) - rather than assume
             * what SHOULD be at this address (which would require
             * transcribing/guessing at real BIOS content, against this
             * project's standing clean-room convention), this project
             * now does what real hardware actually does: delivers a
             * real exception and lets whatever real handler chain is
             * already installed (or this project's own already-
             * existing default-vector fallback, extended below to
             * treat ExcCode 0x0A as synchronous/non-restartable, same
             * as SYSCALL/BREAK/Trap) decide what happens next. This
             * replaces an immediate, uninformative halt with the
             * architecturally-correct real behavior, and may reveal
             * further genuine progress or a cleaner subsequent wall
             * instead of guessing opcode-by-opcode. */
            st->cop0[13] = (st->cop0[13] & ~0x7Fu) | 0x28u; /* Cause.ExcCode = 10 (Reserved Instruction) */
            st->cop0[14] = this_pc; /* EPC */
            {
                uint32_t vector = (st->cop0[12] & 0x400000u) ? 0xBFC00180u : 0x80000080u; /* Status.BEV */
                st->pc = vector;
                st->next_pc = vector + 4;
            }
            st->cop0[12] = (st->cop0[12] & ~0x3Fu) | ((st->cop0[12] & 0x0Fu) << 2); /* Status stack push */
            st->exception_pending = 1; /* task #156 - see iop_core.h's field comment */
            break;
        }
        break;

    case 0x01: /* REGIMM */
        switch (rt) {
        case 0x00: /* BLTZ */   if ((int32_t)GPR(rs) < 0)  BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        case 0x01: /* BGEZ */   if ((int32_t)GPR(rs) >= 0) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        case 0x10: /* BLTZAL */ LINK(31); if ((int32_t)GPR(rs) < 0)  BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        case 0x11: /* BGEZAL */ LINK(31); if ((int32_t)GPR(rs) >= 0) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        default:
            /* Round 175 (task #340, 215th finding) - same real
             * Reserved Instruction exception (ExcCode 0x0A) as the
             * SPECIAL default case above; see that case's own comment
             * for the full citation trail. */
            st->cop0[13] = (st->cop0[13] & ~0x7Fu) | 0x28u; /* Cause.ExcCode = 10 (Reserved Instruction) */
            st->cop0[14] = this_pc; /* EPC */
            {
                uint32_t vector = (st->cop0[12] & 0x400000u) ? 0xBFC00180u : 0x80000080u; /* Status.BEV */
                st->pc = vector;
                st->next_pc = vector + 4;
            }
            st->cop0[12] = (st->cop0[12] & ~0x3Fu) | ((st->cop0[12] & 0x0Fu) << 2); /* Status stack push */
            st->exception_pending = 1; /* task #156 - see iop_core.h's field comment */
            break;
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
                /* Round 175 (task #340, 215th finding) - same real
                 * Reserved Instruction exception (ExcCode 0x0A) as
                 * the SPECIAL/REGIMM/primary-opcode default cases
                 * elsewhere in this function; see the SPECIAL default
                 * case's own comment for the full citation trail.
                 * Only RFE (funct=0x10) is real R3000A CO-format
                 * architecture (no TLB on the IOP, unlike the EE) -
                 * any other funct value here is genuinely undefined
                 * encoding space, the same class of gap as an
                 * unimplemented SPECIAL/REGIMM funct. */
                st->cop0[13] = (st->cop0[13] & ~0x7Fu) | 0x28u; /* Cause.ExcCode = 10 (Reserved Instruction) */
                st->cop0[14] = this_pc; /* EPC */
                {
                    uint32_t vector = (st->cop0[12] & 0x400000u) ? 0xBFC00180u : 0x80000080u; /* Status.BEV */
                    st->pc = vector;
                    st->next_pc = vector + 4;
                }
                st->cop0[12] = (st->cop0[12] & ~0x3Fu) | ((st->cop0[12] & 0x0Fu) << 2); /* Status stack push */
                st->exception_pending = 1; /* task #156 - see iop_core.h's field comment */
                break;
            }
            break;
        default:
            /* Round 175 (task #340, 215th finding) - same real
             * Reserved Instruction exception; see the SPECIAL default
             * case's own comment for the full citation trail. Real
             * COP0 only defines rs=0x00 (MFC0), 0x04 (MTC0), and
             * 0x10 (CO-format) on the R3000A - any other rs value is
             * genuinely undefined encoding space. */
            st->cop0[13] = (st->cop0[13] & ~0x7Fu) | 0x28u; /* Cause.ExcCode = 10 (Reserved Instruction) */
            st->cop0[14] = this_pc; /* EPC */
            {
                uint32_t vector = (st->cop0[12] & 0x400000u) ? 0xBFC00180u : 0x80000080u; /* Status.BEV */
                st->pc = vector;
                st->next_pc = vector + 4;
            }
            st->cop0[12] = (st->cop0[12] & ~0x3Fu) | ((st->cop0[12] & 0x0Fu) << 2); /* Status stack push */
            st->exception_pending = 1; /* task #156 - see iop_core.h's field comment */
            break;
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
        /* Round 175 (task #340, 215th finding) - same real Reserved
         * Instruction exception (ExcCode 0x0A) as the SPECIAL/REGIMM
         * default cases above; see the SPECIAL default case's own
         * comment for the full citation trail. */
        st->cop0[13] = (st->cop0[13] & ~0x7Fu) | 0x28u; /* Cause.ExcCode = 10 (Reserved Instruction) */
        st->cop0[14] = this_pc; /* EPC */
        {
            uint32_t vector = (st->cop0[12] & 0x400000u) ? 0xBFC00180u : 0x80000080u; /* Status.BEV */
            st->pc = vector;
            st->next_pc = vector + 4;
        }
        st->cop0[12] = (st->cop0[12] & ~0x3Fu) | ((st->cop0[12] & 0x0Fu) << 2); /* Status stack push */
        st->exception_pending = 1; /* task #156 - see iop_core.h's field comment */
        break;
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

    /* Round 389: real THREADMAN scheduler tick - wakes any thread
     * whose DelayThread() deadline has passed and, if a newly-woken
     * thread now outranks whatever is currently running, performs a
     * real pre-emptive context switch. Same unconditional-even-while-
     * idle rationale as iop_timers_tick() above (a real periodic
     * timer tick is exactly the mechanism real hardware's own
     * scheduler uses for this). No-op until the first real thread
     * primitive call has run (see iop_hle_thread.c's ensure_root_
     * thread()). */
    iop_hle_thread_tick(&g_iop);

    /* Round 153 (task #307): service the real async I/O queue once
     * per scheduler tick, same unconditional-even-while-idle
     * rationale as iop_timers_tick() above - a queued completion
     * (e.g. the CD-ROM boot-unblock request) must still fire even
     * while the CPU itself is parked in the real B0h/TestEvent poll
     * loop, exactly like a real timer-tick interrupt would. */
    iop_asyncio_service();

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
