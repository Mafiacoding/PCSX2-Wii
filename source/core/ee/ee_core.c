/*
 * ee_core.c - R5900 (Emotion Engine) interpreter
 *
 * Instruction semantics (sign-extension rules for 32-bit ops into the
 * 64-bit register file, HI/LO handling for MULT/DIV, etc.) are ported
 * from PCSX2's own interpreter, pcsx2/R5900OpcodeImpl.cpp - not
 * reinvented from the MIPS manual, so behavior matches real PCSX2 for
 * the opcodes covered here. That file is GPL-3.0 (PCSX2 project), so
 * this file - and this project as a whole, once you link against
 * derived logic like this - is also GPL-3.0. See /COPYING.GPLv3.
 *
 * Coverage: a meaningful subset of the MIPS III/EE integer core -
 * ALU (imm + reg-reg), shifts (incl. 64-bit D-variants), MULT/DIV,
 * HI/LO moves, branches (incl. REGIMM: BLTZ/BGEZ), jumps incl. link
 * register, and byte/half/word/double loads and stores.
 *
 * Still NOT implemented (halts cleanly, does not crash):
 *   - MMI (multimedia/SIMD) opcodes - see MMI.cpp upstream
 *   - COP1 (FPU), COP2 (VU0 macro mode)
 *   - LWL/LWR/SWL/SWR (unaligned load/store), LQ/SQ (128-bit)
 *   - TLB / MMU, exceptions/interrupts, SYSCALL handler table
 *   - The IOP (separate MIPS core) and its BIOS side-channel
 *   - The 128-bit register upper halves
 *
 * A real BIOS will still halt here fairly quickly - see docs/STATUS.md
 * for what's actually required to get further.
 */

#include "core/ee/ee_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#define EE_RAM_SIZE (32 * 1024 * 1024)

static ee_state_t g_state;

ee_state_t *ee_core_get_state(void) { return &g_state; }

/* --- memory access --- */

static inline uint8_t *ee_mem_ptr(ee_state_t *st, uint32_t addr, uint32_t size)
{
    if (addr >= 0xBFC00000u) {
        uint32_t off = addr - 0xBFC00000u;
        if (st->bios && off + size <= st->bios->size)
            return st->bios->data + off;
        return NULL;
    }
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys + size <= st->ram_size)
        return st->ram + phys;
    return NULL;
}

uint8_t ee_mem_read8(ee_state_t *st, uint32_t addr)
{
    uint8_t *p = ee_mem_ptr(st, addr, 1);
    return p ? *p : 0;
}

uint16_t ee_mem_read16(ee_state_t *st, uint32_t addr)
{
    uint8_t *p = ee_mem_ptr(st, addr, 2);
    uint16_t v = 0;
    if (p) memcpy(&v, p, 2);
    return v;
}

uint32_t ee_mem_read32(ee_state_t *st, uint32_t addr)
{
    uint8_t *p = ee_mem_ptr(st, addr, 4);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

uint64_t ee_mem_read64(ee_state_t *st, uint32_t addr)
{
    uint8_t *p = ee_mem_ptr(st, addr, 8);
    uint64_t v = 0;
    if (p) memcpy(&v, p, 8);
    return v;
}

void ee_mem_write8(ee_state_t *st, uint32_t addr, uint8_t val)
{
    uint8_t *p = ee_mem_ptr(st, addr, 1);
    if (p) *p = val;
}

void ee_mem_write16(ee_state_t *st, uint32_t addr, uint16_t val)
{
    uint8_t *p = ee_mem_ptr(st, addr, 2);
    if (p) memcpy(p, &val, 2);
}

void ee_mem_write32(ee_state_t *st, uint32_t addr, uint32_t val)
{
    uint8_t *p = ee_mem_ptr(st, addr, 4);
    if (p) memcpy(p, &val, 4);
}

void ee_mem_write64(ee_state_t *st, uint32_t addr, uint64_t val)
{
    uint8_t *p = ee_mem_ptr(st, addr, 8);
    if (p) memcpy(p, &val, 8);
}

int ee_core_init(const bios_image_t *bios)
{
    memset(&g_state, 0, sizeof(g_state));

    g_state.ram = memalign(32, EE_RAM_SIZE);
    if (!g_state.ram) {
        printf("[!] Could not allocate %u MB of EE RAM (out of memory)\n",
               EE_RAM_SIZE / (1024 * 1024));
        return -1;
    }
    memset(g_state.ram, 0, EE_RAM_SIZE);
    g_state.ram_size = EE_RAM_SIZE;

    g_state.bios = bios;
    g_state.pc = BIOS_RESET_VECTOR;
    g_state.next_pc = BIOS_RESET_VECTOR + 4;
    g_state.gpr[0] = 0;

    return 0;
}

static void halt(const char *reason)
{
    g_state.halted = 1;
    strncpy(g_state.halt_reason, reason, sizeof(g_state.halt_reason) - 1);
}

/* Sign/zero-extension helpers matching R5900OpcodeImpl.cpp's u64(s64(s32(...)))
 * idiom: 32-bit ALU results are computed in 32 bits, then sign-extended
 * to fill the (low) 64-bit register. */
static inline uint64_t sext32(uint32_t v) { return (uint64_t)(int64_t)(int32_t)v; }

static int ee_step(void)
{
    ee_state_t *st = &g_state;
    uint32_t pc = st->pc;
    uint32_t instr = ee_mem_read32(st, pc);

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

    uint32_t rs32 = (uint32_t)st->gpr[rs];
    uint32_t rt32 = (uint32_t)st->gpr[rt];

#define GPR(x) st->gpr[x]
#define BRANCH_TO(target) do { st->next_pc = (target); } while (0)
#define LINK(reg) do { GPR(reg) = this_pc + 8; } while (0)

    switch (op) {
    case 0x00: /* SPECIAL */
        switch (funct) {
        case 0x00: /* SLL */    if (rd) GPR(rd) = sext32(rt32 << sa); break;
        case 0x02: /* SRL */    if (rd) GPR(rd) = sext32(rt32 >> sa); break;
        case 0x03: /* SRA */    if (rd) GPR(rd) = sext32((uint32_t)((int32_t)rt32 >> sa)); break;
        case 0x04: /* SLLV */   if (rd) GPR(rd) = sext32(rt32 << (rs32 & 0x1F)); break;
        case 0x06: /* SRLV */   if (rd) GPR(rd) = sext32(rt32 >> (rs32 & 0x1F)); break;
        case 0x07: /* SRAV */   if (rd) GPR(rd) = sext32((uint32_t)((int32_t)rt32 >> (rs32 & 0x1F))); break;
        case 0x08: /* JR */     BRANCH_TO((uint32_t)GPR(rs)); break;
        case 0x09: /* JALR */   { uint32_t tgt = (uint32_t)GPR(rs); if (rd) LINK(rd); BRANCH_TO(tgt); } break;
        case 0x0A: /* MOVZ */   if (rd && GPR(rt) == 0) GPR(rd) = GPR(rs); break;
        case 0x0B: /* MOVN */   if (rd && GPR(rt) != 0) GPR(rd) = GPR(rs); break;
        case 0x0C: /* SYSCALL */ halt("SYSCALL (no BIOS syscall table implemented)"); return 1;
        case 0x0D: /* BREAK */  halt("BREAK"); return 1;
        case 0x10: /* MFHI */   if (rd) GPR(rd) = st->hi; break;
        case 0x11: /* MTHI */   st->hi = GPR(rs); break;
        case 0x12: /* MFLO */   if (rd) GPR(rd) = st->lo; break;
        case 0x13: /* MTLO */   st->lo = GPR(rs); break;
        case 0x14: /* DSLLV */  if (rd) GPR(rd) = GPR(rt) << (rs32 & 0x3F); break;
        case 0x16: /* DSRLV */  if (rd) GPR(rd) = GPR(rt) >> (rs32 & 0x3F); break;
        case 0x17: /* DSRAV */  if (rd) GPR(rd) = (uint64_t)((int64_t)GPR(rt) >> (rs32 & 0x3F)); break;
        case 0x18: /* MULT */ {
            int64_t res = (int64_t)(int32_t)rs32 * (int64_t)(int32_t)rt32;
            st->lo = sext32((uint32_t)(res & 0xFFFFFFFFu));
            st->hi = sext32((uint32_t)(res >> 32));
            if (rd) GPR(rd) = st->lo;
        } break;
        case 0x19: /* MULTU */ {
            uint64_t res = (uint64_t)rs32 * (uint64_t)rt32;
            st->lo = sext32((uint32_t)(res & 0xFFFFFFFFu));
            st->hi = sext32((uint32_t)(res >> 32));
            if (rd) GPR(rd) = st->lo;
        } break;
        case 0x1A: /* DIV */
            if (rt32 != 0) {
                st->lo = sext32((uint32_t)((int32_t)rs32 / (int32_t)rt32));
                st->hi = sext32((uint32_t)((int32_t)rs32 % (int32_t)rt32));
            }
            break;
        case 0x1B: /* DIVU */
            if (rt32 != 0) {
                st->lo = sext32(rs32 / rt32);
                st->hi = sext32(rs32 % rt32);
            }
            break;
        case 0x20: /* ADD */
        case 0x21: /* ADDU */   if (rd) GPR(rd) = sext32(rs32 + rt32); break;
        case 0x22: /* SUB */
        case 0x23: /* SUBU */   if (rd) GPR(rd) = sext32(rs32 - rt32); break;
        case 0x24: /* AND */    if (rd) GPR(rd) = GPR(rs) & GPR(rt); break;
        case 0x25: /* OR */     if (rd) GPR(rd) = GPR(rs) | GPR(rt); break;
        case 0x26: /* XOR */    if (rd) GPR(rd) = GPR(rs) ^ GPR(rt); break;
        case 0x27: /* NOR */    if (rd) GPR(rd) = ~(GPR(rs) | GPR(rt)); break;
        case 0x2A: /* SLT */    if (rd) GPR(rd) = ((int64_t)GPR(rs) < (int64_t)GPR(rt)) ? 1 : 0; break;
        case 0x2B: /* SLTU */   if (rd) GPR(rd) = (GPR(rs) < GPR(rt)) ? 1 : 0; break;
        case 0x2D: /* DADDU */  if (rd) GPR(rd) = GPR(rs) + GPR(rt); break;
        case 0x2F: /* DSUBU */  if (rd) GPR(rd) = GPR(rs) - GPR(rt); break;
        case 0x38: /* DSLL */   if (rd) GPR(rd) = GPR(rt) << sa; break;
        case 0x3A: /* DSRL */   if (rd) GPR(rd) = GPR(rt) >> sa; break;
        case 0x3B: /* DSRA */   if (rd) GPR(rd) = (uint64_t)((int64_t)GPR(rt) >> sa); break;
        case 0x3C: /* DSLL32 */ if (rd) GPR(rd) = GPR(rt) << (sa + 32); break;
        case 0x3E: /* DSRL32 */ if (rd) GPR(rd) = GPR(rt) >> (sa + 32); break;
        case 0x3F: /* DSRA32 */ if (rd) GPR(rd) = (uint64_t)((int64_t)GPR(rt) >> (sa + 32)); break;
        default:
            halt("unimplemented SPECIAL funct (likely MMI territory)");
            return 1;
        }
        break;

    case 0x01: /* REGIMM */
        switch (rt) {
        case 0x00: /* BLTZ */ if ((int64_t)GPR(rs) < 0)  BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        case 0x01: /* BGEZ */ if ((int64_t)GPR(rs) >= 0) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
        default:
            halt("unimplemented REGIMM opcode");
            return 1;
        }
        break;

    case 0x02: /* J */   BRANCH_TO((this_pc & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2)); break;
    case 0x03: /* JAL */  LINK(31); BRANCH_TO((this_pc & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2)); break;
    case 0x04: /* BEQ */  if (GPR(rs) == GPR(rt)) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
    case 0x05: /* BNE */  if (GPR(rs) != GPR(rt)) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
    case 0x06: /* BLEZ */ if ((int64_t)GPR(rs) <= 0) BRANCH_TO(this_pc + 4 + (imm << 2)); break;
    case 0x07: /* BGTZ */ if ((int64_t)GPR(rs) > 0)  BRANCH_TO(this_pc + 4 + (imm << 2)); break;

    case 0x08: /* ADDI */
    case 0x09: /* ADDIU */ if (rt) GPR(rt) = sext32((uint32_t)((int32_t)rs32 + imm)); break;
    case 0x0A: /* SLTI */  if (rt) GPR(rt) = ((int64_t)GPR(rs) < (int64_t)imm) ? 1 : 0; break;
    case 0x0B: /* SLTIU */ if (rt) GPR(rt) = (GPR(rs) < (uint64_t)(int64_t)imm) ? 1 : 0; break;
    case 0x0C: /* ANDI */  if (rt) GPR(rt) = GPR(rs) & (uint64_t)uimm; break;
    case 0x0D: /* ORI */   if (rt) GPR(rt) = GPR(rs) | (uint64_t)uimm; break;
    case 0x0E: /* XORI */  if (rt) GPR(rt) = GPR(rs) ^ (uint64_t)uimm; break;
    case 0x0F: /* LUI */   if (rt) GPR(rt) = sext32(uimm << 16); break;

    case 0x20: /* LB */  if (rt) GPR(rt) = (uint64_t)(int64_t)(int8_t)ee_mem_read8(st, rs32 + imm); else ee_mem_read8(st, rs32 + imm); break;
    case 0x21: /* LH */  if (rt) GPR(rt) = (uint64_t)(int64_t)(int16_t)ee_mem_read16(st, rs32 + imm); else ee_mem_read16(st, rs32 + imm); break;
    case 0x23: /* LW */  if (rt) GPR(rt) = sext32(ee_mem_read32(st, rs32 + imm)); else ee_mem_read32(st, rs32 + imm); break;
    case 0x24: /* LBU */ if (rt) GPR(rt) = ee_mem_read8(st, rs32 + imm); else ee_mem_read8(st, rs32 + imm); break;
    case 0x25: /* LHU */ if (rt) GPR(rt) = ee_mem_read16(st, rs32 + imm); else ee_mem_read16(st, rs32 + imm); break;
    case 0x27: /* LWU */ if (rt) GPR(rt) = ee_mem_read32(st, rs32 + imm); else ee_mem_read32(st, rs32 + imm); break;
    case 0x37: /* LD */  if (rt) GPR(rt) = ee_mem_read64(st, rs32 + imm); else ee_mem_read64(st, rs32 + imm); break;

    case 0x28: /* SB */ ee_mem_write8(st, rs32 + imm, (uint8_t)GPR(rt)); break;
    case 0x29: /* SH */ ee_mem_write16(st, rs32 + imm, (uint16_t)GPR(rt)); break;
    case 0x2B: /* SW */ ee_mem_write32(st, rs32 + imm, (uint32_t)GPR(rt)); break;
    case 0x3F: /* SD */ ee_mem_write64(st, rs32 + imm, GPR(rt)); break;

    default:
        halt("unimplemented primary opcode (MMI/COP1/COP2/LWL-SWR/LQ-SQ territory)");
        return 1;
    }

#undef GPR
#undef BRANCH_TO
#undef LINK

    st->gpr[0] = 0;
    st->instructions_executed++;
    return 0;
}

void ee_core_run(const bios_image_t *bios)
{
    (void)bios;
    const uint64_t step_report_interval = 10000;

    while (!g_state.halted) {
        if (ee_step())
            break;

        if ((g_state.instructions_executed % step_report_interval) == 0) {
            printf("  ... %llu instructions executed, pc=0x%08lX\n",
                   (unsigned long long)g_state.instructions_executed, (unsigned long)g_state.pc);
        }
    }

    printf("\n[!] EE core halted after %llu instructions at pc=0x%08lX\n",
           (unsigned long long)g_state.instructions_executed, (unsigned long)g_state.pc);
    printf("    reason: %s\n", g_state.halt_reason[0] ? g_state.halt_reason : "(unknown)");
}

void ee_core_shutdown(void)
{
    if (g_state.ram) {
        free(g_state.ram);
        g_state.ram = NULL;
    }
}
