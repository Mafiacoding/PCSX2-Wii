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
 * IOP doesn't have one on real hardware either), no interrupt-driven
 * exceptions (nothing but SYSCALL currently raises one), no timer
 * ticking (iop_timers.c is a pure register stub). See docs/ROADMAP.md
 * for the full picture and docs/STATUS.md's "First real BIOS boot
 * attempt" section for what a real BIOS dump's execution against
 * this core actually looks like today.
 */

#include "core/iop/iop_core.h"
#include "core/hw/sif.h"
#include "core/hw/iop_intc.h"
#include "core/hw/iop_dma.h"
#include "core/hw/iop_timers.h"
#include "core/hw/iop_spu2.h" /* SPU2 register scaffold - task #95 */
#include "core/hw/iop_hle_bios.h"
#include "core/hw/iop_hle_modules.h"
#include "core/hw/iop_module_loader.h" /* real IOP module/IRX loader - task #92 */
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
    uint8_t *p = iop_mem_ptr(st, addr, 1);
    return p ? *p : 0;
}

uint16_t iop_mem_read16(iop_state_t *st, uint32_t addr)
{
    uint16_t spu2_val;
    if (iop_spu2_mmio_read16(addr, &spu2_val))
        return spu2_val;

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

    uint8_t *p = iop_mem_ptr(st, addr, 4);
    if (!p) return 0;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void iop_mem_write8(iop_state_t *st, uint32_t addr, uint8_t val)
{
    uint8_t *p = iop_mem_ptr(st, addr, 1);
    if (p) *p = val;
}

void iop_mem_write16(iop_state_t *st, uint32_t addr, uint16_t val)
{
    if (iop_spu2_mmio_write16(addr, val))
        return;

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
    iop_hle_bios_init(); /* IOP BIOS syscall trap (A0/B0/C0) - see core/hw/iop_hle_bios.h */
    iop_hle_modules_init(); /* IOP module registry scaffold - see core/hw/iop_hle_modules.h */
    iop_module_loader_reset(); /* real module/IRX boot sequencer - see core/hw/iop_module_loader.h */

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
    return 0;
}

static void halt(const char *reason)
{
    g_iop.halted = 1;
    strncpy(g_iop.halt_reason, reason, sizeof(g_iop.halt_reason) - 1);
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

    /* Real IOP module/IRX boot sequencer trampoline (task #92) -
     * see core/hw/iop_module_loader.h. Checked right after the A0/
     * B0/C0 BIOS trap, same "intercept before fetch" convention. */
    if (iop_module_loader_try_handle(st, pc)) {
        st->instructions_executed++;
        return st->halted ? 1 : 0;
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
        case 0x09: /* JALR */ { uint32_t tgt = GPR(rs); if (rd) LINK(rd); BRANCH_TO(tgt); } break;
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
            st->cop0[13] = (st->cop0[13] & ~0x7Fu) | 0x20u; /* Cause.ExcCode = 8 (Syscall) */
            st->cop0[14] = this_pc; /* EPC */
            uint32_t vector = (st->cop0[12] & 0x400000u) ? 0xBFC00180u : 0x80000080u; /* Status.BEV */
            st->pc = vector;
            st->next_pc = vector + 4;
            st->cop0[12] = (st->cop0[12] & ~0x3Fu) | ((st->cop0[12] & 0x0Fu) << 2); /* Status stack push */
        }
        break;
        case 0x0D: /* BREAK */ halt("BREAK"); return 1;
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
    case 0x03: /* JAL */  LINK(31); BRANCH_TO((this_pc & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2)); break;
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
