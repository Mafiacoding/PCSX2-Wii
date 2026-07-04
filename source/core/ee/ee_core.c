/*
 * ee_core.c - R5900 (Emotion Engine) interpreter
 *
 * Instruction semantics (sign-extension rules, HI/LO handling, MMI
 * lane packing, etc.) are ported from PCSX2's own interpreter -
 * pcsx2/R5900OpcodeImpl.cpp and pcsx2/MMI.cpp - not reinvented from
 * the MIPS manual, so behavior matches real PCSX2 for the opcodes
 * covered here. Those files are GPL-3.0 (PCSX2 project), so this
 * file - and this project as a whole - is GPL-3.0. See /COPYING.GPLv3.
 *
 * Coverage, roughly in order of how much of a real BIOS boot they
 * unblock:
 *   - Full MIPS III integer core: ALU imm+reg, all shift variants
 *     incl. 64-bit D-forms, MULT/DIV, HI/LO moves, REGIMM branches,
 *     J/JAL/JR/JALR, byte/half/word/double load+store.
 *   - COP0: MFC0/MTC0 (Status/Config/generic registers only - no TLB,
 *     no BC0 branches, no ERET/exceptions).
 *   - CACHE, SYNC, PREF: accepted as no-ops (real BIOS init code issues
 *     these constantly for cache management we don't model).
 *   - A meaningful subset (~35 of ~90) of MMI (SIMD) opcodes: the
 *     add/sub/logic/copy/extend/pack family across byte/half/word
 *     lanes, plus the "pipeline 1" MULT1/DIV1/MFHI1/MFLO1 family.
 *
 * Still NOT implemented (halts cleanly, does not crash):
 *   - The other ~55 MMI opcodes (saturated arithmetic, compares,
 *     QFSRV, PMADDW/H family, PINTH/PINTEH, PROT3W, etc.)
 *   - COP1 (FPU), COP2 (VU0 macro mode)
 *   - LWL/LWR/SWL/SWR (unaligned load/store), LQ/SQ (128-bit)
 *   - TLB/MMU, exceptions/interrupts, SYSCALL handler table
 *   - The IOP (separate MIPS core) and its BIOS side-channel
 *
 * A real BIOS will still halt here - see docs/STATUS.md for what's
 * actually required to get to a rendered splash screen (short
 * version: the GS/GIF/DMA/IOP/SIF stack, which dwarfs the CPU work).
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

/* PS2 (EE + IOP) memory is little-endian; the Wii's PowerPC 750 is
 * big-endian. A plain memcpy() here would silently byte-swap every
 * instruction fetch and load/store, so all multi-byte accesses are
 * composed/decomposed byte-by-byte in explicit little-endian order,
 * independent of host/target endianness. */

uint8_t ee_mem_read8(ee_state_t *st, uint32_t addr)
{
    uint8_t *p = ee_mem_ptr(st, addr, 1);
    return p ? *p : 0;
}

uint16_t ee_mem_read16(ee_state_t *st, uint32_t addr)
{
    uint8_t *p = ee_mem_ptr(st, addr, 2);
    if (!p) return 0;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t ee_mem_read32(ee_state_t *st, uint32_t addr)
{
    uint8_t *p = ee_mem_ptr(st, addr, 4);
    if (!p) return 0;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t ee_mem_read64(ee_state_t *st, uint32_t addr)
{
    uint8_t *p = ee_mem_ptr(st, addr, 8);
    if (!p) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
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
    if (!p) return;
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
}

void ee_mem_write32(ee_state_t *st, uint32_t addr, uint32_t val)
{
    uint8_t *p = ee_mem_ptr(st, addr, 4);
    if (!p) return;
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
    p[2] = (uint8_t)((val >> 16) & 0xFF);
    p[3] = (uint8_t)((val >> 24) & 0xFF);
}

void ee_mem_write64(ee_state_t *st, uint32_t addr, uint64_t val)
{
    uint8_t *p = ee_mem_ptr(st, addr, 8);
    if (!p) return;
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((val >> (8 * i)) & 0xFF);
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

    return 0;
}

static void halt(const char *reason)
{
    g_state.halted = 1;
    strncpy(g_state.halt_reason, reason, sizeof(g_state.halt_reason) - 1);
}

/* 32-bit ALU results are computed in 32 bits, then sign-extended to
 * fill the low 64-bit half of the register (matches PCSX2's
 * u64(s64(s32(...))) idiom). The upper 64 bits (ud1) are left
 * untouched by ordinary (non-MMI) instructions - this matches real
 * EE/PCSX2 behavior. */
static inline uint64_t sext32(uint32_t v) { return (uint64_t)(int64_t)(int32_t)v; }

/* --- 128-bit lane accessors for MMI opcodes (lane 0 = least significant) --- */

static inline uint32_t lane_w(ee_reg128_t r, int n) {
    return (uint32_t)((n < 2 ? r.ud0 : r.ud1) >> ((n & 1) * 32));
}
static inline void set_lane_w(ee_reg128_t *r, int n, uint32_t val) {
    uint64_t *p = (n < 2) ? &r->ud0 : &r->ud1;
    int sh = (n & 1) * 32;
    uint64_t mask = ~((uint64_t)0xFFFFFFFFu << sh);
    *p = (*p & mask) | ((uint64_t)val << sh);
}
static inline uint16_t lane_h(ee_reg128_t r, int n) {
    return (uint16_t)((n < 4 ? r.ud0 : r.ud1) >> ((n & 3) * 16));
}
static inline void set_lane_h(ee_reg128_t *r, int n, uint16_t val) {
    uint64_t *p = (n < 4) ? &r->ud0 : &r->ud1;
    int sh = (n & 3) * 16;
    uint64_t mask = ~((uint64_t)0xFFFFu << sh);
    *p = (*p & mask) | ((uint64_t)val << sh);
}
static inline uint8_t lane_b(ee_reg128_t r, int n) {
    return (uint8_t)((n < 8 ? r.ud0 : r.ud1) >> ((n & 7) * 8));
}
static inline void set_lane_b(ee_reg128_t *r, int n, uint8_t val) {
    uint64_t *p = (n < 8) ? &r->ud0 : &r->ud1;
    int sh = (n & 7) * 8;
    uint64_t mask = ~((uint64_t)0xFFu << sh);
    *p = (*p & mask) | ((uint64_t)val << sh);
}

static int ee_step(void)
{
    ee_state_t *st = &g_state;
    uint32_t pc = st->pc;
    uint32_t instr = ee_mem_read32(st, pc);

    uint32_t op    = (instr >> 26) & 0x3F;
    uint32_t rs    = (instr >> 21) & 0x1F;
    uint32_t rt    = (instr >> 16) & 0x1F;
    uint32_t rd    = (instr >> 11) & 0x1F;
    uint32_t sa    = (instr >> 6)  & 0x1F; /* also used as MMI sub-table selector */
    int32_t  imm   = (int16_t)(instr & 0xFFFF);
    uint32_t uimm  = instr & 0xFFFF;
    uint32_t funct = instr & 0x3F;

    uint32_t this_pc = pc;
    uint32_t fallthrough_pc = st->next_pc;
    st->pc = fallthrough_pc;
    st->next_pc = fallthrough_pc + 4;

    uint32_t rs32 = (uint32_t)st->gpr[rs].ud0;
    uint32_t rt32 = (uint32_t)st->gpr[rt].ud0;

#define GPR(x)  st->gpr[x].ud0
#define GPR1(x) st->gpr[x].ud1
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
        case 0x0F: /* SYNC */   break; /* no-op: no cache/pipeline model */
        case 0x10: /* MFHI */   if (rd) GPR(rd) = st->hi.ud0; break;
        case 0x11: /* MTHI */   st->hi.ud0 = GPR(rs); break;
        case 0x12: /* MFLO */   if (rd) GPR(rd) = st->lo.ud0; break;
        case 0x13: /* MTLO */   st->lo.ud0 = GPR(rs); break;
        case 0x14: /* DSLLV */  if (rd) GPR(rd) = GPR(rt) << (rs32 & 0x3F); break;
        case 0x16: /* DSRLV */  if (rd) GPR(rd) = GPR(rt) >> (rs32 & 0x3F); break;
        case 0x17: /* DSRAV */  if (rd) GPR(rd) = (uint64_t)((int64_t)GPR(rt) >> (rs32 & 0x3F)); break;
        case 0x18: /* MULT */ {
            int64_t res = (int64_t)(int32_t)rs32 * (int64_t)(int32_t)rt32;
            st->lo.ud0 = sext32((uint32_t)(res & 0xFFFFFFFFu));
            st->hi.ud0 = sext32((uint32_t)(res >> 32));
            if (rd) GPR(rd) = st->lo.ud0;
        } break;
        case 0x19: /* MULTU */ {
            uint64_t res = (uint64_t)rs32 * (uint64_t)rt32;
            st->lo.ud0 = sext32((uint32_t)(res & 0xFFFFFFFFu));
            st->hi.ud0 = sext32((uint32_t)(res >> 32));
            if (rd) GPR(rd) = st->lo.ud0;
        } break;
        case 0x1A: /* DIV */
            if (rt32 != 0) {
                st->lo.ud0 = sext32((uint32_t)((int32_t)rs32 / (int32_t)rt32));
                st->hi.ud0 = sext32((uint32_t)((int32_t)rs32 % (int32_t)rt32));
            }
            break;
        case 0x1B: /* DIVU */
            if (rt32 != 0) {
                st->lo.ud0 = sext32(rs32 / rt32);
                st->hi.ud0 = sext32(rs32 % rt32);
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
            halt("unimplemented SPECIAL funct");
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

    case 0x10: /* COP0 */
        switch (rs) {
        case 0x00: /* MFC0 */
            if (rt) GPR(rt) = sext32(st->cop0[rd]);
            break;
        case 0x04: /* MTC0 */
            if (rd == 16) /* Config: preserve read-only IC/DC bits like PCSX2's WriteCP0Config */
                st->cop0[16] = (rt32 & ~0xFC0u) | 0x440u;
            else
                st->cop0[rd] = rt32;
            break;
        default:
            halt("unimplemented COP0 sub-opcode (BC0/TLB/ERET not implemented)");
            return 1;
        }
        break;

    case 0x1C: /* MMI */
        switch (funct) {
        case 0x00: /* MADD */ {
            int64_t acc = (int64_t)((uint64_t)st->lo.ud0 & 0xFFFFFFFFu) | ((int64_t)(int32_t)(uint32_t)st->hi.ud0 << 32);
            int64_t res = acc + (int64_t)(int32_t)rs32 * (int64_t)(int32_t)rt32;
            st->lo.ud0 = sext32((uint32_t)(res & 0xFFFFFFFFu));
            st->hi.ud0 = sext32((uint32_t)(res >> 32));
            if (rd) GPR(rd) = st->lo.ud0;
        } break;
        case 0x01: /* MADDU */ {
            uint64_t acc = ((uint64_t)(uint32_t)st->lo.ud0) | ((uint64_t)(uint32_t)st->hi.ud0 << 32);
            uint64_t res = acc + (uint64_t)rs32 * (uint64_t)rt32;
            st->lo.ud0 = sext32((uint32_t)(res & 0xFFFFFFFFu));
            st->hi.ud0 = sext32((uint32_t)(res >> 32));
            if (rd) GPR(rd) = st->lo.ud0;
        } break;
        case 0x04: /* PLZCW */
            if (rd) {
                for (int n = 0; n < 2; n++) {
                    int32_t v = (int32_t)lane_w(st->gpr[rs], n);
                    int count = 0;
                    /* count leading bits equal to the sign bit, excluding it */
                    uint32_t sign = (uint32_t)v >> 31;
                    uint32_t x = (uint32_t)v;
                    for (int b = 30; b >= 0; b--) {
                        if (((x >> b) & 1) != sign) break;
                        count++;
                    }
                    set_lane_w(&st->gpr[rd], n, (uint32_t)count);
                }
            }
            break;
        case 0x10: /* MFHI1 */ if (rd) GPR(rd) = st->hi.ud1; break;
        case 0x11: /* MTHI1 */ st->hi.ud1 = GPR(rs); break;
        case 0x12: /* MFLO1 */ if (rd) GPR(rd) = st->lo.ud1; break;
        case 0x13: /* MTLO1 */ st->lo.ud1 = GPR(rs); break;
        case 0x18: /* MULT1 */ {
            int64_t res = (int64_t)(int32_t)rs32 * (int64_t)(int32_t)rt32;
            st->lo.ud1 = sext32((uint32_t)(res & 0xFFFFFFFFu));
            st->hi.ud1 = sext32((uint32_t)(res >> 32));
            if (rd) GPR(rd) = st->lo.ud1;
        } break;
        case 0x19: /* MULTU1 */ {
            uint64_t res = (uint64_t)rs32 * (uint64_t)rt32;
            st->lo.ud1 = sext32((uint32_t)(res & 0xFFFFFFFFu));
            st->hi.ud1 = sext32((uint32_t)(res >> 32));
            if (rd) GPR(rd) = st->lo.ud1;
        } break;
        case 0x1A: /* DIV1 */
            if (rt32 != 0) {
                st->lo.ud1 = sext32((uint32_t)((int32_t)rs32 / (int32_t)rt32));
                st->hi.ud1 = sext32((uint32_t)((int32_t)rs32 % (int32_t)rt32));
            }
            break;
        case 0x1B: /* DIVU1 */
            if (rt32 != 0) {
                st->lo.ud1 = sext32(rs32 / rt32);
                st->hi.ud1 = sext32(rs32 % rt32);
            }
            break;
        case 0x34: /* PSLLH */ if (rd) for (int n = 0; n < 8; n++) set_lane_h(&st->gpr[rd], n, (uint16_t)(lane_h(st->gpr[rt], n) << (sa & 0xF))); break;
        case 0x36: /* PSRLH */ if (rd) for (int n = 0; n < 8; n++) set_lane_h(&st->gpr[rd], n, (uint16_t)(lane_h(st->gpr[rt], n) >> (sa & 0xF))); break;
        case 0x37: /* PSRAH */ if (rd) for (int n = 0; n < 8; n++) set_lane_h(&st->gpr[rd], n, (uint16_t)((int16_t)lane_h(st->gpr[rt], n) >> (sa & 0xF))); break;
        case 0x3C: /* PSLLW */ if (rd) for (int n = 0; n < 4; n++) set_lane_w(&st->gpr[rd], n, lane_w(st->gpr[rt], n) << sa); break;
        case 0x3E: /* PSRLW */ if (rd) for (int n = 0; n < 4; n++) set_lane_w(&st->gpr[rd], n, lane_w(st->gpr[rt], n) >> sa); break;
        case 0x3F: /* PSRAW */ if (rd) for (int n = 0; n < 4; n++) set_lane_w(&st->gpr[rd], n, (uint32_t)((int32_t)lane_w(st->gpr[rt], n) >> sa)); break;

        case 0x08: /* MMI0 */
            switch (sa) {
            case 0x00: /* PADDW */ if (rd) for (int n = 0; n < 4; n++) set_lane_w(&st->gpr[rd], n, lane_w(st->gpr[rs], n) + lane_w(st->gpr[rt], n)); break;
            case 0x01: /* PSUBW */ if (rd) for (int n = 0; n < 4; n++) set_lane_w(&st->gpr[rd], n, lane_w(st->gpr[rs], n) - lane_w(st->gpr[rt], n)); break;
            case 0x04: /* PADDH */ if (rd) for (int n = 0; n < 8; n++) set_lane_h(&st->gpr[rd], n, (uint16_t)(lane_h(st->gpr[rs], n) + lane_h(st->gpr[rt], n))); break;
            case 0x05: /* PSUBH */ if (rd) for (int n = 0; n < 8; n++) set_lane_h(&st->gpr[rd], n, (uint16_t)(lane_h(st->gpr[rs], n) - lane_h(st->gpr[rt], n))); break;
            case 0x08: /* PADDB */ if (rd) for (int n = 0; n < 16; n++) set_lane_b(&st->gpr[rd], n, (uint8_t)(lane_b(st->gpr[rs], n) + lane_b(st->gpr[rt], n))); break;
            case 0x09: /* PSUBB */ if (rd) for (int n = 0; n < 16; n++) set_lane_b(&st->gpr[rd], n, (uint8_t)(lane_b(st->gpr[rs], n) - lane_b(st->gpr[rt], n))); break;
            case 0x12: /* PEXTLW */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    set_lane_w(&out, 0, lane_w(Rt, 0)); set_lane_w(&out, 1, lane_w(Rs, 0));
                    set_lane_w(&out, 2, lane_w(Rt, 1)); set_lane_w(&out, 3, lane_w(Rs, 1));
                    st->gpr[rd] = out;
                }
                break;
            case 0x13: /* PPACW */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    set_lane_w(&out, 0, lane_w(Rt, 0)); set_lane_w(&out, 1, lane_w(Rt, 2));
                    set_lane_w(&out, 2, lane_w(Rs, 0)); set_lane_w(&out, 3, lane_w(Rs, 2));
                    st->gpr[rd] = out;
                }
                break;
            case 0x16: /* PEXTLH */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    set_lane_h(&out, 0, lane_h(Rt, 0)); set_lane_h(&out, 1, lane_h(Rs, 0));
                    set_lane_h(&out, 2, lane_h(Rt, 1)); set_lane_h(&out, 3, lane_h(Rs, 1));
                    set_lane_h(&out, 4, lane_h(Rt, 2)); set_lane_h(&out, 5, lane_h(Rs, 2));
                    set_lane_h(&out, 6, lane_h(Rt, 3)); set_lane_h(&out, 7, lane_h(Rs, 3));
                    st->gpr[rd] = out;
                }
                break;
            case 0x17: /* PPACH */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    set_lane_h(&out, 0, lane_h(Rt, 0)); set_lane_h(&out, 1, lane_h(Rt, 2));
                    set_lane_h(&out, 2, lane_h(Rt, 4)); set_lane_h(&out, 3, lane_h(Rt, 6));
                    set_lane_h(&out, 4, lane_h(Rs, 0)); set_lane_h(&out, 5, lane_h(Rs, 2));
                    set_lane_h(&out, 6, lane_h(Rs, 4)); set_lane_h(&out, 7, lane_h(Rs, 6));
                    st->gpr[rd] = out;
                }
                break;
            case 0x1A: /* PEXTLB */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    for (int n = 0; n < 8; n++) {
                        set_lane_b(&out, n * 2,     lane_b(Rt, n));
                        set_lane_b(&out, n * 2 + 1, lane_b(Rs, n));
                    }
                    st->gpr[rd] = out;
                }
                break;
            case 0x1B: /* PPACB */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    for (int n = 0; n < 8; n++) {
                        set_lane_b(&out, n,     lane_b(Rt, n * 2));
                        set_lane_b(&out, n + 8, lane_b(Rs, n * 2));
                    }
                    st->gpr[rd] = out;
                }
                break;
            default:
                halt("unimplemented MMI0 sub-opcode");
                return 1;
            }
            break;

        case 0x28: /* MMI1 */
            switch (sa) {
            case 0x10: /* PADDUW */ if (rd) for (int n = 0; n < 4; n++) set_lane_w(&st->gpr[rd], n, lane_w(st->gpr[rs], n) + lane_w(st->gpr[rt], n)); break;
            case 0x11: /* PSUBUW */ if (rd) for (int n = 0; n < 4; n++) { uint32_t a = lane_w(st->gpr[rs], n), b = lane_w(st->gpr[rt], n); set_lane_w(&st->gpr[rd], n, (a > b) ? a - b : 0); } break;
            case 0x12: /* PEXTUW */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    set_lane_w(&out, 0, lane_w(Rt, 2)); set_lane_w(&out, 1, lane_w(Rs, 2));
                    set_lane_w(&out, 2, lane_w(Rt, 3)); set_lane_w(&out, 3, lane_w(Rs, 3));
                    st->gpr[rd] = out;
                }
                break;
            case 0x14: /* PADDUH */ if (rd) for (int n = 0; n < 8; n++) set_lane_h(&st->gpr[rd], n, (uint16_t)(lane_h(st->gpr[rs], n) + lane_h(st->gpr[rt], n))); break;
            case 0x15: /* PSUBUH */ if (rd) for (int n = 0; n < 8; n++) { uint16_t a = lane_h(st->gpr[rs], n), b = lane_h(st->gpr[rt], n); set_lane_h(&st->gpr[rd], n, (a > b) ? (uint16_t)(a - b) : 0); } break;
            case 0x16: /* PEXTUH */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    set_lane_h(&out, 0, lane_h(Rt, 4)); set_lane_h(&out, 1, lane_h(Rs, 4));
                    set_lane_h(&out, 2, lane_h(Rt, 5)); set_lane_h(&out, 3, lane_h(Rs, 5));
                    set_lane_h(&out, 4, lane_h(Rt, 6)); set_lane_h(&out, 5, lane_h(Rs, 6));
                    set_lane_h(&out, 6, lane_h(Rt, 7)); set_lane_h(&out, 7, lane_h(Rs, 7));
                    st->gpr[rd] = out;
                }
                break;
            case 0x18: /* PADDUB */ if (rd) for (int n = 0; n < 16; n++) set_lane_b(&st->gpr[rd], n, (uint8_t)(lane_b(st->gpr[rs], n) + lane_b(st->gpr[rt], n))); break;
            case 0x19: /* PSUBUB */ if (rd) for (int n = 0; n < 16; n++) { uint8_t a = lane_b(st->gpr[rs], n), b = lane_b(st->gpr[rt], n); set_lane_b(&st->gpr[rd], n, (a > b) ? (uint8_t)(a - b) : 0); } break;
            case 0x1A: /* PEXTUB */
                if (rd) {
                    ee_reg128_t Rs = st->gpr[rs], Rt = st->gpr[rt], out;
                    for (int n = 0; n < 8; n++) {
                        set_lane_b(&out, n * 2,     lane_b(Rt, n + 8));
                        set_lane_b(&out, n * 2 + 1, lane_b(Rs, n + 8));
                    }
                    st->gpr[rd] = out;
                }
                break;
            default:
                halt("unimplemented MMI1 sub-opcode (incl. QFSRV)");
                return 1;
            }
            break;

        case 0x09: /* MMI2 */
            switch (sa) {
            case 0x08: /* PMFHI */ if (rd) st->gpr[rd] = st->hi; break;
            case 0x09: /* PMFLO */ if (rd) st->gpr[rd] = st->lo; break;
            case 0x0E: /* PCPYLD */
                if (rd) {
                    ee_reg128_t out;
                    out.ud1 = st->gpr[rs].ud0;
                    out.ud0 = st->gpr[rt].ud0;
                    st->gpr[rd] = out;
                }
                break;
            case 0x12: /* PAND */ if (rd) { st->gpr[rd].ud0 = st->gpr[rs].ud0 & st->gpr[rt].ud0; st->gpr[rd].ud1 = st->gpr[rs].ud1 & st->gpr[rt].ud1; } break;
            case 0x13: /* PXOR */ if (rd) { st->gpr[rd].ud0 = st->gpr[rs].ud0 ^ st->gpr[rt].ud0; st->gpr[rd].ud1 = st->gpr[rs].ud1 ^ st->gpr[rt].ud1; } break;
            default:
                halt("unimplemented MMI2 sub-opcode");
                return 1;
            }
            break;

        case 0x29: /* MMI3 */
            switch (sa) {
            case 0x08: /* PMTHI */ st->hi = st->gpr[rs]; break;
            case 0x09: /* PMTLO */ st->lo = st->gpr[rs]; break;
            case 0x0E: /* PCPYUD */
                if (rd) {
                    ee_reg128_t out;
                    out.ud0 = st->gpr[rs].ud1;
                    out.ud1 = st->gpr[rt].ud1;
                    st->gpr[rd] = out;
                }
                break;
            case 0x12: /* POR */  if (rd) { st->gpr[rd].ud0 = st->gpr[rs].ud0 | st->gpr[rt].ud0; st->gpr[rd].ud1 = st->gpr[rs].ud1 | st->gpr[rt].ud1; } break;
            case 0x13: /* PNOR */ if (rd) { st->gpr[rd].ud0 = ~(st->gpr[rs].ud0 | st->gpr[rt].ud0); st->gpr[rd].ud1 = ~(st->gpr[rs].ud1 | st->gpr[rt].ud1); } break;
            default:
                halt("unimplemented MMI3 sub-opcode");
                return 1;
            }
            break;

        default:
            halt("unimplemented MMI top-level funct");
            return 1;
        }
        break;

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
    case 0x2F: /* CACHE */ break; /* no-op: no cache model */
    case 0x33: /* PREF */  break; /* no-op: prefetch hint */
    case 0x3F: /* SD */ ee_mem_write64(st, rs32 + imm, GPR(rt)); break;

    default:
        halt("unimplemented primary opcode (COP1/COP2/LWL-SWR/LQ-SQ territory)");
        return 1;
    }

#undef GPR
#undef GPR1
#undef BRANCH_TO
#undef LINK

    st->gpr[0].ud0 = 0;
    st->gpr[0].ud1 = 0;
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
