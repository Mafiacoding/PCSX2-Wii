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
 *     J/JAL/JR/JALR, byte/half/word/double load+store, and unaligned
 *     LWL/LWR/SWL/SWR.
 *   - COP0: MFC0/MTC0 (Status/Config/generic registers only), plus
 *     the "CO"-format instructions ERET, EI, DI (ported from PCSX2's
 *     COP0.cpp), and RFE (the older MIPS I exception-return
 *     instruction - not in PCSX2's own EE opcode table since real
 *     EE/R5900 doesn't need it, but real BIOS PS1-backward-
 *     compatibility-mode boot code does execute it, so it's
 *     implemented here with RFE's standard MIPS I semantics). Still
 *     no TLB (TLBR/TLBWI/TLBWR/TLBP), no BC0 branches, no actual
 *     exception RAISING (ERET/RFE only handle the return side; there
 *     is no MMU/interrupt logic that would ever trigger EPC/Cause to
 *     be set other than explicit MTC0 writes).
 *   - CACHE, SYNC, PREF: accepted as no-ops (real BIOS init code issues
 *     these constantly for cache management we don't model).
 *   - A meaningful subset (~35 of ~90) of MMI (SIMD) opcodes: the
 *     add/sub/logic/copy/extend/pack family across byte/half/word
 *     lanes, plus the "pipeline 1" MULT1/DIV1/MFHI1/MFLO1 family.
 *   - LQ/SQ (128-bit load/store, primary opcodes 0x1E/0x1F) - ported
 *     from PCSX2's R5900OpcodeImpl.cpp. Address masked to 16-byte
 *     alignment (real hardware silently ignores the low 4 bits rather
 *     than faulting); LQ skips the read entirely when rt==$0 (matches
 *     PCSX2's interpreter exactly - unlike LW/LH/etc elsewhere in
 *     this file, which still perform a discarded read for its memory
 *     side effects). Added after real-BIOS testing (see
 *     docs/STATUS.md) showed the EE now running 53M+ real instructions
 *     before halting on exactly this gap - the first real evidence
 *     that boot code needs 128-bit memory access, presumably for
 *     VU0 data staging.
 *
 * Still NOT implemented (halts cleanly, does not crash):
 *   - The other ~55 MMI opcodes (saturated arithmetic, compares,
 *     QFSRV, PMADDW/H family, PINTH/PINTEH, PROT3W, etc.)
 *   - COP2 (VU0 macro mode)
 *   - COP1 (FPU): core arithmetic (ADD/SUB/MUL/DIV/ABS/MOV/NEG.S,
 *     SQRT.S/RSQRT.S, MAX.S/MIN.S, CVT.W.S/CVT.S.W, C.EQ/LT/LE.S,
 *     MFC1/MTC1/CFC1/CTC1), BC1F/BC1T (branch on FP condition flag),
 *     and the full ACC (accumulator) family - ADDA.S/SUBA.S/MULA.S/
 *     MADD.S/MSUB.S/MADDA.S/MSUBA.S - are implemented, ported from
 *     pcsx2/FPU.cpp including the PS2's non-IEEE denormal/infinity
 *     handling (fpuDouble/checkOverflow/checkUnderflow), the
 *     MAX.S/MIN.S bit-level signed-int comparison quirk (fp_max/
 *     fp_min), and a real hardware/PCSX2 asymmetry in the ACC family
 *     worth remembering: MADD.S/MSUB.S run their intermediate
 *     fs*ft product through fpuDouble() a SECOND time before adding/
 *     subtracting it from ACC, but MADDA.S/MSUBA.S do NOT - ported
 *     exactly as PCSX2 has it, not "cleaned up" to be consistent.
 *     Still NOT implemented: BC1FL/BC1TL ("likely" branches - this
 *     project has no likely-branch infrastructure for ANY branch yet,
 *     integer or FP), and the FPU exception-cause control-register
 *     flags (O/U/I/D/SO/SU/SI/SD) - only the condition flag (C,
 *     needed for BC1) is modeled; nothing in this project raises FPU
 *     exceptions from the others yet, a documented, consistent
 *     simplification across every FPU op in this file.
 *   - TLB/MMU (no TLB entries modeled at all), interrupt/exception
 *     RAISING (nothing ever triggers an exception - only the
 *     exception-RETURN instructions ERET/RFE above are implemented),
 *     SYSCALL handler table
 *   - The IOP has its own core (iop_core.c) and BIOS syscall trap
 *     (core/hw/iop_hle_bios.c) now - see docs/ROADMAP.md for scope
 *
 * A real BIOS will still halt here - see docs/STATUS.md for what's
 * actually required to get to a rendered splash screen (short
 * version: the GS/GIF/DMA/IOP/SIF stack, which dwarfs the CPU work).
 */

#include "core/ee/ee_core.h"
#include "core/hw/dma.h"
#include "core/hw/gs.h"
#include "core/hw/gif.h"
#include "core/hw/sif.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>

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
    /* Hardware register window (0x10000000-0x1000FFFF): DMA controller,
     * SIF mailbox registers, and friends. Other addresses in this
     * window (timers, INTC, SIO, GIF/VIF/IPU control regs) still fall
     * through to the silent-no-op RAM/BIOS path below, which returns
     * 0. See docs/ROADMAP.md. */
    uint32_t hw_val;
    if (dma_mmio_read32(addr, &hw_val))
        return hw_val;
    if (sif_mmio_read32(addr, &hw_val))
        return hw_val;

    uint8_t *p = ee_mem_ptr(st, addr, 4);
    if (!p) return 0;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t ee_mem_read64(ee_state_t *st, uint32_t addr)
{
    uint64_t gs_val;
    if (gs_mmio_read64(addr, &gs_val))
        return gs_val;

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
    if (dma_mmio_write32(addr, val))
        return;
    if (sif_mmio_write32(addr, val))
        return;

    uint8_t *p = ee_mem_ptr(st, addr, 4);
    if (!p) return;
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
    p[2] = (uint8_t)((val >> 16) & 0xFF);
    p[3] = (uint8_t)((val >> 24) & 0xFF);
}

void ee_mem_write64(ee_state_t *st, uint32_t addr, uint64_t val)
{
    if (gs_mmio_write64(addr, val))
        return;

    uint8_t *p = ee_mem_ptr(st, addr, 8);
    if (!p) return;
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((val >> (8 * i)) & 0xFF);
}

int ee_core_init(const bios_image_t *bios)
{
    memset(&g_state, 0, sizeof(g_state));

    dma_init(); /* EE DMA controller register block - see core/hw/dma.h */
    gs_init();  /* GS privileged register block - see core/hw/gs.h */
    sif_init(); /* EE-side SIF/SBUS mailbox registers - see core/hw/sif.h */

    g_state.ram = memalign(32, EE_RAM_SIZE);
    if (!g_state.ram) {
        printf("[!] Could not allocate %u MB of EE RAM (out of memory)\n",
               EE_RAM_SIZE / (1024 * 1024));
        return -1;
    }
    memset(g_state.ram, 0, EE_RAM_SIZE);
    g_state.ram_size = EE_RAM_SIZE;

    dma_bind_ee_ram(g_state.ram, g_state.ram_size); /* chain-mode DMA reads tags/data from here */
    gif_init();
    dma_set_sink(DMA_CHANNEL_GIF, gif_process_quadwords); /* GIF DMA transfers now actually get parsed and drawn */

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

/* --- COP1 (FPU) helpers, ported from PCSX2's pcsx2/FPU.cpp ---
 * PS2's FPU isn't strict IEEE-754: it treats denormal inputs as
 * signed zero and clamps infinite inputs to +/-Fmax *before* doing
 * arithmetic (fpuDouble), then separately clamps infinite/denormal
 * *results* after the operation (checkOverflow/checkUnderflow). Both
 * steps matter for matching real hardware/PCSX2 behavior. */
#define FPU_POS_INFINITY 0x7f800000u
#define FPU_POS_FMAX      0x7F7FFFFFu
#define FPU_NEG_FMAX      0xFF7FFFFFu

static inline float bits_to_float(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
static inline uint32_t float_to_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

static float fpu_double(uint32_t f)
{
    uint32_t exp = f & 0x7f800000u;
    if (exp == 0)               return bits_to_float(f & 0x80000000u);              /* denormal -> signed zero */
    if (exp == 0x7f800000u)     return bits_to_float((f & 0x80000000u) | FPU_POS_FMAX); /* infinity -> +/-Fmax */
    return bits_to_float(f);
}

static int fpu_check_overflow(uint32_t *reg)
{
    if ((*reg & ~0x80000000u) == FPU_POS_INFINITY) {
        *reg = (*reg & 0x80000000u) | FPU_POS_FMAX;
        return 1;
    }
    return 0;
}

static void fpu_check_underflow(uint32_t *reg)
{
    if ((*reg & 0x7F800000u) == 0 && (*reg & 0x007FFFFFu) != 0)
        *reg &= 0x80000000u; /* denormal result -> signed zero */
}

/* fp_max/fp_min - ported from PCSX2's FPU.cpp exactly (bit-level
 * comparison, not a float compare): when both operands are negative,
 * a plain signed-32-bit min/max on the raw bit patterns gives the
 * IEEE-754 max/min respectively (negative floats sort in REVERSED
 * order as signed integers), otherwise a plain signed-32-bit max/min
 * on the bit patterns already agrees with the float comparison. This
 * is why PCSX2 implements it this way rather than a naive
 * bits_to_float() compare - it's not a shortcut, it's the actual
 * hardware-matching algorithm. */
static uint32_t fp_max(uint32_t a, uint32_t b)
{
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (sa < 0 && sb < 0) return (uint32_t)((sa < sb) ? sa : sb);
    return (uint32_t)((sa > sb) ? sa : sb);
}
static uint32_t fp_min(uint32_t a, uint32_t b)
{
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (sa < 0 && sb < 0) return (uint32_t)((sa > sb) ? sa : sb);
    return (uint32_t)((sa < sb) ? sa : sb);
}


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
            if (rs & 0x10) {
                /* "CO" format: rs's top bit set means the real
                 * operation is selected by the 6-bit funct field
                 * (bits 0-5), not by rs itself - matches PCSX2's own
                 * tbl_COP0_C0[64] dispatch table in
                 * R5900OpcodeTables.cpp. */
                uint32_t co_funct = instr & 0x3F;
                switch (co_funct) {
                case 0x10: /* RFE - "Return From Exception".
                     * NOTE: real PCSX2's EE (R5900) opcode table does
                     * NOT implement this (tbl_COP0_C0[0x10] is
                     * COP0_Unknown there) - RFE is the ORIGINAL MIPS I
                     * exception-return instruction, superseded by
                     * ERET (funct 0x18, below) on MIPS III+. It shows
                     * up here because this real BIOS dump's early
                     * boot code includes PS1-backward-compatibility-
                     * mode boot code (this file's own header bytes
                     * contain the ASCII string "PS compatible mode"),
                     * which runs in a MIPS-I-compatible execution
                     * context where RFE is meaningful. Implemented
                     * with RFE's standard, unambiguous MIPS I
                     * semantics (restore the 3-level interrupt-
                     * enable/kernel-mode bit stack by shifting it
                     * right by 2) rather than skipped/faked, since
                     * that's well-defined regardless of the exact
                     * execution mode question above. RFE does NOT
                     * itself change PC (unlike ERET) - the caller
                     * separately jumps to EPC, typically via JR. */
                    st->cop0[12] = (st->cop0[12] & ~0x0Fu) | ((st->cop0[12] >> 2) & 0x0Fu);
                    break;
                case 0x18: /* ERET - ported from PCSX2's COP0::ERET().
                     * No branch delay slot (unlike ordinary jumps/
                     * branches) - takes effect immediately, so this
                     * sets st->pc/next_pc directly rather than going
                     * through the BRANCH_TO() delay-slot convention. */
                {
                    uint32_t target;
                    if (st->cop0[12] & 0x4u) { /* Status.ERL */
                        target = st->cop0[30]; /* ErrorEPC */
                        st->cop0[12] &= ~0x4u;
                    } else {
                        target = st->cop0[14]; /* EPC */
                        st->cop0[12] &= ~0x2u; /* Status.EXL */
                    }
                    st->pc = target;
                    st->next_pc = target + 4;
                    break;
                }
                case 0x38: /* EI - ported from PCSX2's COP0::EI(). Gated
                     * the same way real hardware/PCSX2 gates it:
                     * only takes effect if _EDI, EXL, ERL are set, or
                     * KSU==0 (kernel mode) - i.e. code running in
                     * user mode without the right permission bit
                     * can't enable interrupts via this instruction. */
                    if ((st->cop0[12] & 0x20000u) ||      /* _EDI  (bit 17) */
                        (st->cop0[12] & 0x2u) ||           /* EXL   (bit 1)  */
                        (st->cop0[12] & 0x4u) ||           /* ERL   (bit 2)  */
                        ((st->cop0[12] & 0x18u) == 0))      /* KSU==0 (bits 3-4) */
                    {
                        st->cop0[12] |= 0x10000u; /* Status.EIE = 1 (bit 16) */
                    }
                    break;
                case 0x39: /* DI - ported from PCSX2's COP0::DI(), same gating as EI. */
                    if ((st->cop0[12] & 0x20000u) ||
                        (st->cop0[12] & 0x2u) ||
                        (st->cop0[12] & 0x4u) ||
                        ((st->cop0[12] & 0x18u) == 0))
                    {
                        st->cop0[12] &= ~0x10000u; /* Status.EIE = 0 */
                    }
                    break;
                default:
                {
                    char buf[96];
                    snprintf(buf, sizeof(buf),
                             "unimplemented COP0 CO-format funct 0x%02X (TLBR/TLBWI/TLBWR/TLBP not implemented, pc=0x%08X)",
                             (unsigned int)co_funct, (unsigned int)this_pc);
                    halt(buf);
                    return 1;
                }
                }
            } else {
                char buf[96];
                snprintf(buf, sizeof(buf),
                         "unimplemented COP0 sub-opcode (rs=0x%02X, pc=0x%08X, BC0 not implemented)",
                         (unsigned int)rs, (unsigned int)this_pc);
                halt(buf);
                return 1;
            }
            break;
        }
        break;

    case 0x11: /* COP1 (FPU, single-precision only) */
        switch (rs) {
        case 0x00: /* MFC1 */ if (rt) GPR(rt) = sext32(st->fpr[rd]); break;
        case 0x02: /* CFC1 */
            if (rt) {
                if (rd == 31) GPR(rt) = sext32(st->fcr31);
                else if (rd == 0) GPR(rt) = sext32(0x2E00u);
                else GPR(rt) = 0;
            }
            break;
        case 0x04: /* MTC1 */ st->fpr[rd] = rt32; break;
        case 0x06: /* CTC1 */ if (rd == 31) st->fcr31 = rt32; break;
        case 0x10: { /* COP1.S - single-precision arithmetic, fd=sa fs=rd ft=rt */
            uint32_t fd = sa, fs = rd, ft = rt;
            switch (funct) {
            case 0x00: /* ADD.S */ {
                float r = fpu_double(st->fpr[fs]) + fpu_double(st->fpr[ft]);
                st->fpr[fd] = float_to_bits(r);
                if (!fpu_check_overflow(&st->fpr[fd])) fpu_check_underflow(&st->fpr[fd]);
            } break;
            case 0x01: /* SUB.S */ {
                float r = fpu_double(st->fpr[fs]) - fpu_double(st->fpr[ft]);
                st->fpr[fd] = float_to_bits(r);
                if (!fpu_check_overflow(&st->fpr[fd])) fpu_check_underflow(&st->fpr[fd]);
            } break;
            case 0x02: /* MUL.S */ {
                float r = fpu_double(st->fpr[fs]) * fpu_double(st->fpr[ft]);
                st->fpr[fd] = float_to_bits(r);
                if (!fpu_check_overflow(&st->fpr[fd])) fpu_check_underflow(&st->fpr[fd]);
            } break;
            case 0x03: /* DIV.S */ {
                uint32_t divisor = st->fpr[ft], dividend = st->fpr[fs];
                if ((divisor & 0x7F800000u) == 0) {
                    /* divide-by-zero (denormal counts as zero too): result is
                     * signed +/-Fmax, sign = XOR of operand signs. */
                    st->fpr[fd] = ((divisor ^ dividend) & 0x80000000u) | FPU_POS_FMAX;
                } else {
                    float r = fpu_double(dividend) / fpu_double(divisor);
                    st->fpr[fd] = float_to_bits(r);
                    fpu_check_overflow(&st->fpr[fd]);
                    fpu_check_underflow(&st->fpr[fd]);
                }
            } break;
            case 0x05: /* ABS.S */ st->fpr[fd] = st->fpr[fs] & 0x7fffffffu; break;
            case 0x06: /* MOV.S */ st->fpr[fd] = st->fpr[fs]; break;
            case 0x07: /* NEG.S */ st->fpr[fd] = st->fpr[fs] ^ 0x80000000u; break;
            case 0x24: /* CVT.W.S (float -> int32) */
                if ((st->fpr[fs] & 0x7F800000u) <= 0x4E800000u) {
                    st->fpr[fd] = (uint32_t)(int32_t)bits_to_float(st->fpr[fs]);
                } else {
                    st->fpr[fd] = (st->fpr[fs] & 0x80000000u) ? 0x80000000u : 0x7fffffffu;
                }
                break;
            case 0x04: /* SQRT.S - real hardware/PCSX2 quirk: the source
                        * operand is Ft (rt field), NOT Fs, and Fs
                        * (rd field) is unused - ported exactly from
                        * PCSX2's SQRT_S(), not a typo. Documented
                        * simplification, matching this file's
                        * existing FPU scope: the D/I exception-cause
                        * control-register flags PCSX2 also sets here
                        * are NOT modeled (nothing in this project
                        * raises FPU exceptions from them yet - same
                        * simplification already in place for every
                        * other FPU op above, which also skip the
                        * O/U/SO/SU flags). */
                if ((st->fpr[ft] & 0x7F800000u) == 0) {
                    st->fpr[fd] = st->fpr[ft] & 0x80000000u; /* +/-0 */
                } else if (st->fpr[ft] & 0x80000000u) {
                    float r = sqrtf(fabsf(fpu_double(st->fpr[ft])));
                    st->fpr[fd] = float_to_bits(r);
                } else {
                    float r = sqrtf(fpu_double(st->fpr[ft]));
                    st->fpr[fd] = float_to_bits(r);
                }
                break;
            case 0x16: /* RSQRT.S: fd = fs / sqrt(ft) - ported from
                        * PCSX2's RSQRT_S(). Same documented flag
                        * simplification as SQRT.S above. */
                if ((st->fpr[ft] & 0x7F800000u) == 0) {
                    /* ft is +/-0 (denormals-are-zero): result is
                     * +/-Fmax, sign taken from ft. */
                    st->fpr[fd] = (st->fpr[ft] & 0x80000000u) | FPU_POS_FMAX;
                } else if (st->fpr[ft] & 0x80000000u) {
                    float denom = sqrtf(fabsf(fpu_double(st->fpr[ft])));
                    float r = fpu_double(st->fpr[fs]) / denom;
                    st->fpr[fd] = float_to_bits(r);
                    fpu_check_overflow(&st->fpr[fd]);
                    fpu_check_underflow(&st->fpr[fd]);
                } else {
                    float r = fpu_double(st->fpr[fs]) / sqrtf(fpu_double(st->fpr[ft]));
                    st->fpr[fd] = float_to_bits(r);
                    fpu_check_overflow(&st->fpr[fd]);
                    fpu_check_underflow(&st->fpr[fd]);
                }
                break;
            case 0x28: /* MAX.S - ported from PCSX2's MAX_S()/fp_max():
                        * a bit-level signed-int max/min, not a float
                        * compare - see fp_max()'s comment. No
                        * overflow/underflow clamping needed (result
                        * is always one of the two original values). */
                st->fpr[fd] = fp_max(st->fpr[fs], st->fpr[ft]);
                break;
            case 0x29: /* MIN.S - ported from PCSX2's MIN_S()/fp_min(). */
                st->fpr[fd] = fp_min(st->fpr[fs], st->fpr[ft]);
                break;
            case 0x18: /* ADDA.S - ACC = fs + ft, ported from PCSX2's
                        * ADDA_S(). */
            {
                float r = fpu_double(st->fpr[fs]) + fpu_double(st->fpr[ft]);
                st->acc = float_to_bits(r);
                if (!fpu_check_overflow(&st->acc)) fpu_check_underflow(&st->acc);
            } break;
            case 0x19: /* SUBA.S - ACC = fs - ft, ported from SUBA_S(). */
            {
                float r = fpu_double(st->fpr[fs]) - fpu_double(st->fpr[ft]);
                st->acc = float_to_bits(r);
                if (!fpu_check_overflow(&st->acc)) fpu_check_underflow(&st->acc);
            } break;
            case 0x1A: /* MULA.S - ACC = fs * ft, ported from MULA_S(). */
            {
                float r = fpu_double(st->fpr[fs]) * fpu_double(st->fpr[ft]);
                st->acc = float_to_bits(r);
                if (!fpu_check_overflow(&st->acc)) fpu_check_underflow(&st->acc);
            } break;
            case 0x1C: /* MADD.S - fd = ACC + (fs * ft), ported from
                        * PCSX2's MADD_S(). Real hardware/PCSX2 quirk
                        * worth preserving exactly: the intermediate
                        * product is run through fpuDouble() a SECOND
                        * time when it's read back for the addition
                        * (PCSX2's own FPRreg temp; temp.f = fpuDouble
                        * (fs)*fpuDouble(ft); then fpuDouble(temp.UL)
                        * again) - unlike MADDA.S below, which doesn't
                        * do this second pass. Not an oversight to
                        * "simplify away" - ported as-is. */
            {
                float prod = fpu_double(st->fpr[fs]) * fpu_double(st->fpr[ft]);
                uint32_t prod_bits = float_to_bits(prod);
                float r = fpu_double(st->acc) + fpu_double(prod_bits);
                st->fpr[fd] = float_to_bits(r);
                if (!fpu_check_overflow(&st->fpr[fd])) fpu_check_underflow(&st->fpr[fd]);
            } break;
            case 0x1D: /* MSUB.S - fd = ACC - (fs * ft), same
                        * double-fpuDouble()-pass quirk as MADD.S
                        * above, ported from PCSX2's MSUB_S(). */
            {
                float prod = fpu_double(st->fpr[fs]) * fpu_double(st->fpr[ft]);
                uint32_t prod_bits = float_to_bits(prod);
                float r = fpu_double(st->acc) - fpu_double(prod_bits);
                st->fpr[fd] = float_to_bits(r);
                if (!fpu_check_overflow(&st->fpr[fd])) fpu_check_underflow(&st->fpr[fd]);
            } break;
            case 0x1E: /* MADDA.S - ACC += fs * ft, ported from PCSX2's
                        * MADDA_S(). NOTE: no intermediate
                        * fpuDouble()-of-the-product pass here, unlike
                        * MADD.S above - that asymmetry is real,
                        * ported exactly as PCSX2 has it. */
            {
                float r = fpu_double(st->acc) + (fpu_double(st->fpr[fs]) * fpu_double(st->fpr[ft]));
                st->acc = float_to_bits(r);
                if (!fpu_check_overflow(&st->acc)) fpu_check_underflow(&st->acc);
            } break;
            case 0x1F: /* MSUBA.S - ACC -= fs * ft, ported from MSUBA_S(). */
            {
                float r = fpu_double(st->acc) - (fpu_double(st->fpr[fs]) * fpu_double(st->fpr[ft]));
                st->acc = float_to_bits(r);
                if (!fpu_check_overflow(&st->acc)) fpu_check_underflow(&st->acc);
            } break;
            case 0x32: /* C.EQ.S */
                if (fpu_double(st->fpr[fs]) == fpu_double(st->fpr[ft])) st->fcr31 |= 0x00800000u;
                else st->fcr31 &= ~0x00800000u;
                break;
            case 0x34: /* C.LT.S */
                if (fpu_double(st->fpr[fs]) < fpu_double(st->fpr[ft])) st->fcr31 |= 0x00800000u;
                else st->fcr31 &= ~0x00800000u;
                break;
            case 0x36: /* C.LE.S */
                if (fpu_double(st->fpr[fs]) <= fpu_double(st->fpr[ft])) st->fcr31 |= 0x00800000u;
                else st->fcr31 &= ~0x00800000u;
                break;
            default:
                halt("unimplemented COP1.S funct (SQRT/RSQRT/MADD/MAX/MIN family not implemented)");
                return 1;
            }
        } break;
        case 0x14: /* COP1.W - only CVT.S.W (int32 -> float) */
            if (funct == 0x20) {
                uint32_t fd = sa, fs = rd;
                st->fpr[fd] = float_to_bits((float)(int32_t)st->fpr[fs]);
            } else {
                halt("unimplemented COP1.W funct");
                return 1;
            }
            break;
        case 0x08: /* COP1 BC (branch on FP condition flag) - sub-selected
                    * by the rt field (matches PCSX2's tbl_COP1_BC1[32],
                    * indexed 0=BC1F, 1=BC1T, 2=BC1FL, 3=BC1TL). Only
                    * the two non-"likely" variants are implemented -
                    * see the case default below for why BC1FL/BC1TL
                    * are still open. */
            switch (rt) {
            case 0x00: /* BC1F - branch if the FP condition flag (fcr31
                        * bit 0x00800000, set by C.EQ/LT/LE.S) is
                        * CLEAR. Ported from PCSX2's BC1()/BC1F(). */
                if (!(st->fcr31 & 0x00800000u)) BRANCH_TO(this_pc + 4 + (imm << 2));
                break;
            case 0x01: /* BC1T - branch if the flag is SET. */
                if ((st->fcr31 & 0x00800000u)) BRANCH_TO(this_pc + 4 + (imm << 2));
                break;
            default:
                /* BC1FL/BC1TL ("branch likely" variants): still not
                 * implemented. Real semantics additionally nullify
                 * (skip) the branch delay-slot instruction when the
                 * branch is NOT taken - this project has no "likely
                 * branch" infrastructure yet for ANY branch (integer
                 * BEQL/BNEL/etc. aren't implemented either), so adding
                 * just the FP half would be inconsistent. Left as a
                 * clearly scoped follow-up rather than a half-fix. */
                halt("unimplemented BC1 variant (BC1FL/BC1TL - likely branches not implemented)");
                return 1;
            }
            break;
        default:
            halt("unimplemented COP1 sub-opcode");
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

    case 0x22: /* LWL */ {
        static const uint32_t LWL_MASK[4]  = { 0xffffffu, 0x0000ffffu, 0x000000ffu, 0x00000000u };
        static const uint8_t  LWL_SHIFT[4] = { 24, 16, 8, 0 };
        uint32_t addr = rs32 + imm;
        uint32_t shift = addr & 3;
        uint32_t mem = ee_mem_read32(st, addr & ~3u);
        if (rt) GPR(rt) = sext32((rt32 & LWL_MASK[shift]) | (mem << LWL_SHIFT[shift]));
    } break;
    case 0x26: /* LWR */ {
        static const uint32_t LWR_MASK[4]  = { 0x00000000u, 0xff000000u, 0xffff0000u, 0xffffff00u };
        static const uint8_t  LWR_SHIFT[4] = { 0, 8, 16, 24 };
        uint32_t addr = rs32 + imm;
        uint32_t shift = addr & 3;
        uint32_t mem = ee_mem_read32(st, addr & ~3u);
        uint32_t result = (rt32 & LWR_MASK[shift]) | (mem >> LWR_SHIFT[shift]);
        if (rt) {
            if (shift == 0)
                GPR(rt) = sext32(result); /* full 64-bit sign extend, matches PCSX2 */
            else
                GPR(rt) = (GPR(rt) & 0xFFFFFFFF00000000ULL) | result; /* upper 32 bits preserved */
        }
    } break;

    case 0x20: /* LB */  if (rt) GPR(rt) = (uint64_t)(int64_t)(int8_t)ee_mem_read8(st, rs32 + imm); else ee_mem_read8(st, rs32 + imm); break;
    case 0x21: /* LH */  if (rt) GPR(rt) = (uint64_t)(int64_t)(int16_t)ee_mem_read16(st, rs32 + imm); else ee_mem_read16(st, rs32 + imm); break;
    case 0x23: /* LW */  if (rt) GPR(rt) = sext32(ee_mem_read32(st, rs32 + imm)); else ee_mem_read32(st, rs32 + imm); break;
    case 0x24: /* LBU */ if (rt) GPR(rt) = ee_mem_read8(st, rs32 + imm); else ee_mem_read8(st, rs32 + imm); break;
    case 0x25: /* LHU */ if (rt) GPR(rt) = ee_mem_read16(st, rs32 + imm); else ee_mem_read16(st, rs32 + imm); break;
    case 0x27: /* LWU */ if (rt) GPR(rt) = ee_mem_read32(st, rs32 + imm); else ee_mem_read32(st, rs32 + imm); break;
    case 0x37: /* LD */  if (rt) GPR(rt) = ee_mem_read64(st, rs32 + imm); else ee_mem_read64(st, rs32 + imm); break;

    case 0x2A: /* SWL */ {
        static const uint32_t SWL_MASK[4]  = { 0xffffff00u, 0xffff0000u, 0xff000000u, 0x00000000u };
        static const uint8_t  SWL_SHIFT[4] = { 24, 16, 8, 0 };
        uint32_t addr = rs32 + imm;
        uint32_t shift = addr & 3;
        uint32_t mem = ee_mem_read32(st, addr & ~3u);
        ee_mem_write32(st, addr & ~3u, (rt32 >> SWL_SHIFT[shift]) | (mem & SWL_MASK[shift]));
    } break;
    case 0x2E: /* SWR */ {
        static const uint32_t SWR_MASK[4]  = { 0x00000000u, 0x000000ffu, 0x0000ffffu, 0x00ffffffu };
        static const uint8_t  SWR_SHIFT[4] = { 0, 8, 16, 24 };
        uint32_t addr = rs32 + imm;
        uint32_t shift = addr & 3;
        uint32_t mem = ee_mem_read32(st, addr & ~3u);
        ee_mem_write32(st, addr & ~3u, (rt32 << SWR_SHIFT[shift]) | (mem & SWR_MASK[shift]));
    } break;

    case 0x28: /* SB */ ee_mem_write8(st, rs32 + imm, (uint8_t)GPR(rt)); break;
    case 0x29: /* SH */ ee_mem_write16(st, rs32 + imm, (uint16_t)GPR(rt)); break;
    case 0x2B: /* SW */ ee_mem_write32(st, rs32 + imm, (uint32_t)GPR(rt)); break;
    case 0x2F: /* CACHE */ break; /* no-op: no cache model */
    case 0x33: /* PREF */  break; /* no-op: prefetch hint */
    case 0x3F: /* SD */ ee_mem_write64(st, rs32 + imm, GPR(rt)); break;

    case 0x1E: /* LQ - 128-bit load, ported from PCSX2's R5900OpcodeImpl.cpp.
                * Address is masked to 16-byte alignment (real hardware
                * ignores the low 4 bits rather than faulting on
                * unaligned access, unlike LW/LD). Matches PCSX2's own
                * interpreter exactly in skipping the read entirely
                * when rt==$0 (unlike LW/LH/etc elsewhere in this
                * file, which still perform the read for its memory
                * side effects even when the destination is
                * discarded) - LQ has no such side-effect-only path in
                * real PCSX2, so neither does this. */
        if (rt) {
            uint32_t addr = (rs32 + imm) & ~0xFu;
            GPR(rt)  = ee_mem_read64(st, addr);
            GPR1(rt) = ee_mem_read64(st, addr + 8);
        }
        break;
    case 0x1F: /* SQ - 128-bit store, ported from PCSX2's R5900OpcodeImpl.cpp.
                * Same 16-byte alignment masking as LQ. Always writes
                * both halves, including when rt==$0 (whose value is
                * always zero) - matches real hardware/PCSX2, no
                * special-case needed. */
    {
        uint32_t addr = (rs32 + imm) & ~0xFu;
        ee_mem_write64(st, addr,     GPR(rt));
        ee_mem_write64(st, addr + 8, GPR1(rt));
    } break;

    default:
    {
        /* NOTE: this halt point can be reached in a way that has
         * nothing to do with the actual opcode value - see the
         * real-BIOS-testing note in docs/STATUS.md ("EE program
         * counter escapes into the hardware register window" /
         * out-of-range JALR target investigation). If `op` here is
         * 0 (SPECIAL) with funct also reading as 0, that's the
         * signature of this exact scenario: PC drifted through a
         * long stretch of unpopulated ("reads as zero") memory,
         * decoding every word as a harmless SLL/NOP, until it
         * happened to reach a live hardware register address whose
         * CURRENT value decoded as something genuinely invalid. */
        char buf[96];
        snprintf(buf, sizeof(buf), "unimplemented primary opcode 0x%02X (pc=0x%08X)",
                 (unsigned int)op, (unsigned int)this_pc);
        halt(buf);
        return 1;
    }
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

/* Public single-instruction step, for callers (currently
 * source/core/system.c's interleaved EE/IOP scheduler) that need to
 * interleave execution with another core rather than run this core
 * to completion in isolation. Returns the same value as the internal
 * ee_step(): 0 to keep going, 1 if this step halted the core. Safe to
 * keep calling after a halt (ee_step() re-checks st->halted itself
 * via the same path ee_core_run()'s loop uses). */
int ee_core_step(void)
{
    if (g_state.halted)
        return 1;
    return ee_step();
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
