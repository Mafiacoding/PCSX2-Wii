/*
 * iop_core.c - R3000A (IOP) interpreter
 *
 * Semantics ported from PCSX2's pcsx2/R3000AOpcodeTables.cpp
 * (GPL-3.0), same approach as ee_core.c: not reinvented from the
 * MIPS manual, so behavior matches real PCSX2 for opcodes covered.
 *
 * Coverage: the MIPS I integer core - ALU imm+reg, shifts, MULT/DIV,
 * HI/LO moves, branches (incl. REGIMM w/ link variants), jumps incl.
 * link register, byte/half/word load+store, and unaligned
 * LWL/LWR/SWL/SWR (which the EE core doesn't have yet - MIPS I and
 * MIPS III share this part of the ISA, so it was cheap to include
 * here first). Basic COP0 (MFC0/MTC0).
 *
 * NOT implemented: no SIF, no IOP hardware registers (interrupt
 * controller, DMA, timers, controller/memcard modules), no BIOS
 * module loading. This core currently just executes whatever's at
 * the reset vector in isolation - it isn't wired into main.c and
 * doesn't talk to the EE core at all yet. See docs/ROADMAP.md.
 */

#include "core/iop/iop_core.h"
#include "core/hw/sif.h"
#include "core/hw/iop_intc.h"
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

    g_iop.ram = memalign(32, IOP_RAM_SIZE);
    if (!g_iop.ram)
        return -1;
    memset(g_iop.ram, 0, IOP_RAM_SIZE);
    g_iop.ram_size = IOP_RAM_SIZE;

    g_iop.bios = bios;
    g_iop.pc = IOP_RESET_VECTOR;
    g_iop.next_pc = IOP_RESET_VECTOR + 4;
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
        case 0x0C: /* SYSCALL */ halt("SYSCALL (no IOP BIOS syscall table implemented)"); return 1;
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
            halt("unimplemented SPECIAL funct");
            return 1;
        }
        break;

    case 0x01: /* REGIMM */
        switch (rt) {
        case 0x00: /* BLTZ */   if ((int32_t)GPR(rs) < 0)  BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        case 0x01: /* BGEZ */   if ((int32_t)GPR(rs) >= 0) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        case 0x10: /* BLTZAL */ LINK(31); if ((int32_t)GPR(rs) < 0)  BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        case 0x11: /* BGEZAL */ LINK(31); if ((int32_t)GPR(rs) >= 0) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        default:
            halt("unimplemented REGIMM opcode");
            return 1;
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
            halt("unimplemented COP0 sub-opcode");
            return 1;
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
        halt("unimplemented primary opcode");
        return 1;
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
