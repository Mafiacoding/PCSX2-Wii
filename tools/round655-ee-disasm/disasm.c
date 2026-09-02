/* Round 655 (task #536/#447 continuation): standalone host-native EE
 * (R5900 MIPS-based CPU) disassembler.
 *
 * Purpose: the user asked to disassemble the real JP BIOS (scph10000)
 * from the very first instruction at the reset vector all the way to
 * the OSDSYS memory-card/CD-selection menu, to walk through what the
 * real BIOS boot sequence actually does. No EE disassembler tool
 * currently exists in this project (Round 350's was lost in an earlier
 * sandbox reset - see docs/STATUS.md), so this rebuilds one, the same
 * way Round 653 built a VU1 micro-program disassembler.
 *
 * Mnemonic/field source of truth: this project's OWN real, heavily-
 * researched EE interpreter (source/core/ee/ee_core.c's big
 * switch(op){switch(funct){...}} dispatcher, ~line 2915 onward). Every
 * opcode/funct/sub-table value and mnemonic below was extracted
 * DIRECTLY from that already-verified, already-cited real R5900 opcode
 * table (itself cross-referenced against PCSX2's R5900OpcodeImpl.cpp
 * and ps2sdk/ps2tek across many prior rounds) - not re-derived from
 * scratch and not guessed. This guarantees the disassembler's mnemonics
 * exactly match what this project's own emulator would actually DO with
 * the same encoded instruction, which is the most useful ground truth
 * for tracing this project's own boot behavior.
 *
 * Standard MIPS-I/R5900 instruction formats:
 *   R-type: opcode(6) rs(5) rt(5) rd(5) sa(5) funct(6)
 *   I-type: opcode(6) rs(5) rt(5) immediate(16)
 *   J-type: opcode(6) target(26)
 *
 * Coverage: full SPECIAL/REGIMM/COP0/COP1/MMI(+MMI0-3) tables as
 * implemented in ee_core.c, plus all documented I-type/load-store
 * opcodes including the R5900-specific LQ/SQ/LQC2/SQC2. COP2 (VU0
 * macro-mode arithmetic) is only decoded down to its top-level
 * transfer instructions (MFC2/QMFC2/CFC2/MTC2/QMTC2/CTC2/BC2) - the
 * deeper macro-mode FMAC arithmetic space is large and not yet needed
 * for the BIOS boot-flow walkthrough this tool exists to support; any
 * COP2 arithmetic opcode encountered is printed as "cop2.op rs=0x%02X"
 * rather than guessed at, honestly flagging the gap instead of
 * fabricating a mnemonic.
 *
 * Usage: disasm <raw_dump_file> <base_addr_hex> <start_addr_hex> <count>
 *   raw_dump_file: a flat binary dump (BIOS ROM, RAM snapshot, etc.)
 *   base_addr_hex: the MIPS virtual address that file offset 0 maps to
 *                   (e.g. 0xBFC00000 for a raw scph10000.bin BIOS dump)
 *   start_addr_hex: address to start disassembling from
 *   count: number of instructions to print
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static const char *reg_name(uint32_t r)
{
    static const char *names[32] = {
        "zero","at","v0","v1","a0","a1","a2","a3",
        "t0","t1","t2","t3","t4","t5","t6","t7",
        "s0","s1","s2","s3","s4","s5","s6","s7",
        "t8","t9","k0","k1","gp","sp","fp","ra"
    };
    return names[r & 0x1F];
}

static uint32_t rd_word(const uint8_t *buf, size_t bufsz, uint32_t file_off, int *ok)
{
    if ((size_t)file_off + 4 > bufsz) { *ok = 0; return 0; }
    *ok = 1;
    return (uint32_t)buf[file_off] | ((uint32_t)buf[file_off+1] << 8) |
           ((uint32_t)buf[file_off+2] << 16) | ((uint32_t)buf[file_off+3] << 24);
}

/* SPECIAL (op=0x00) funct table - matches ee_core.c exactly. */
static const char *special_mn(uint32_t funct)
{
    switch (funct) {
    case 0x00: return "sll";
    case 0x02: return "srl";
    case 0x03: return "sra";
    case 0x04: return "sllv";
    case 0x06: return "srlv";
    case 0x07: return "srav";
    case 0x08: return "jr";
    case 0x09: return "jalr";
    case 0x0A: return "movz";
    case 0x0B: return "movn";
    case 0x0C: return "syscall";
    case 0x0D: return "break";
    case 0x0F: return "sync";
    case 0x10: return "mfhi";
    case 0x11: return "mthi";
    case 0x12: return "mflo";
    case 0x13: return "mtlo";
    case 0x14: return "dsllv";
    case 0x16: return "dsrlv";
    case 0x17: return "dsrav";
    case 0x18: return "mult";
    case 0x19: return "multu";
    case 0x1A: return "div";
    case 0x1B: return "divu";
    case 0x20: return "add";
    case 0x21: return "addu";
    case 0x22: return "sub";
    case 0x23: return "subu";
    case 0x24: return "and";
    case 0x25: return "or";
    case 0x26: return "xor";
    case 0x27: return "nor";
    case 0x28: return "mfsa";
    case 0x29: return "mtsa";
    case 0x2A: return "slt";
    case 0x2B: return "sltu";
    case 0x2D: return "daddu";
    case 0x2F: return "dsubu";
    case 0x30: return "tge";
    case 0x31: return "tgeu";
    case 0x32: return "tlt";
    case 0x33: return "tltu";
    case 0x34: return "teq";
    case 0x36: return "tne";
    case 0x38: return "dsll";
    case 0x3A: return "dsrl";
    case 0x3B: return "dsra";
    case 0x3C: return "dsll32";
    case 0x3E: return "dsrl32";
    case 0x3F: return "dsra32";
    default: return NULL;
    }
}

static const char *regimm_mn(uint32_t rt)
{
    switch (rt) {
    case 0x00: return "bltz";
    case 0x01: return "bgez";
    case 0x02: return "bltzl";
    case 0x03: return "bgezl";
    case 0x10: return "bltzal";
    case 0x11: return "bgezal";
    case 0x12: return "bltzall";
    case 0x13: return "bgezall";
    default: return NULL;
    }
}

static const char *mmi0_mn(uint32_t sa)
{
    switch (sa) {
    case 0x00: return "paddw"; case 0x01: return "psubw";
    case 0x02: return "pcgtw"; case 0x03: return "pmaxw";
    case 0x04: return "paddh"; case 0x05: return "psubh";
    case 0x06: return "pcgth"; case 0x07: return "pmaxh";
    case 0x08: return "paddb"; case 0x09: return "psubb";
    case 0x0A: return "pcgtb";
    case 0x10: return "paddsw"; case 0x11: return "psubsw";
    case 0x12: return "pextlw"; case 0x13: return "ppacw";
    case 0x14: return "paddsh"; case 0x15: return "psubsh";
    case 0x16: return "pextlh"; case 0x17: return "ppach";
    case 0x18: return "paddsb"; case 0x19: return "psubsb";
    case 0x1A: return "pextlb"; case 0x1B: return "ppacb";
    case 0x1E: return "pext5"; case 0x1F: return "ppac5";
    default: return NULL;
    }
}
static const char *mmi1_mn(uint32_t sa)
{
    switch (sa) {
    case 0x01: return "pabsw"; case 0x02: return "pceqw";
    case 0x03: return "pminw"; case 0x04: return "padsbh";
    case 0x05: return "pabsh"; case 0x06: return "pceqh";
    case 0x07: return "pminh"; case 0x0A: return "pceqb";
    case 0x10: return "padduw"; case 0x11: return "psubuw";
    case 0x12: return "pextuw"; case 0x14: return "padduh";
    case 0x15: return "psubuh"; case 0x16: return "pextuh";
    case 0x18: return "paddub"; case 0x19: return "psubub";
    case 0x1A: return "pextub"; case 0x1B: return "qfsrv";
    default: return NULL;
    }
}
static const char *mmi2_mn(uint32_t sa)
{
    switch (sa) {
    case 0x00: return "pmaddw"; case 0x02: return "psllvw";
    case 0x03: return "psrlvw"; case 0x04: return "pmsubw";
    case 0x08: return "pmfhi";  case 0x09: return "pmflo";
    case 0x0A: return "pinth";  case 0x0C: return "pmultw";
    case 0x0D: return "pdivw";  case 0x0E: return "pcpyld";
    case 0x12: return "pand";   case 0x13: return "pxor";
    case 0x1A: return "pexeh";  case 0x1B: return "prevh";
    case 0x1E: return "pexew";  case 0x1F: return "prot3w";
    default: return NULL;
    }
}
static const char *mmi3_mn(uint32_t sa)
{
    switch (sa) {
    case 0x00: return "pmadduw"; case 0x03: return "psravw";
    case 0x08: return "pmthi";   case 0x09: return "pmtlo";
    case 0x0A: return "pinteh";  case 0x0C: return "pmultuw";
    case 0x0D: return "pdivuw";  case 0x0E: return "pcpyud";
    case 0x12: return "por";     case 0x13: return "pnor";
    case 0x1A: return "pexch";   case 0x1B: return "pcpyh";
    case 0x1E: return "pexcw";
    default: return NULL;
    }
}

static const char *mmi_mn(uint32_t funct)
{
    switch (funct) {
    case 0x00: return "madd";
    case 0x01: return "maddu";
    case 0x04: return "plzcw";
    case 0x10: return "mfhi1";
    case 0x11: return "mthi1";
    case 0x12: return "mflo1";
    case 0x13: return "mtlo1";
    case 0x18: return "mult1";
    case 0x19: return "multu1";
    case 0x1A: return "div1";
    case 0x1B: return "divu1";
    case 0x20: return "madd1";
    case 0x21: return "maddu1";
    case 0x30: return "pmfhl";
    case 0x31: return "pmthl";
    case 0x34: return "psllh";
    case 0x36: return "psrlh";
    case 0x37: return "psrah";
    case 0x3C: return "psllw";
    case 0x3E: return "psrlw";
    case 0x3F: return "psraw";
    default: return NULL;
    }
}

/* Returns 1 and fills out[] with a full disassembled line (no trailing
 * newline), or 0 if the word could not be read (out of buffer). */
static void disasm_one(uint32_t instr, uint32_t pc, char *out, size_t outsz)
{
    uint32_t op    = (instr >> 26) & 0x3F;
    uint32_t rs    = (instr >> 21) & 0x1F;
    uint32_t rt    = (instr >> 16) & 0x1F;
    uint32_t rd    = (instr >> 11) & 0x1F;
    uint32_t sa    = (instr >> 6)  & 0x1F;
    uint32_t funct = instr & 0x3F;
    int32_t  imm   = (int16_t)(instr & 0xFFFF);
    uint32_t uimm  = instr & 0xFFFF;
    char buf[160];

    if (instr == 0x00000000) { snprintf(out, outsz, "nop"); return; }

    switch (op) {
    case 0x00: { /* SPECIAL */
        const char *mn = special_mn(funct);
        if (!mn) { snprintf(out, outsz, "special.0x%02X rs=%s rt=%s rd=%s sa=%u", funct, reg_name(rs), reg_name(rt), reg_name(rd), sa); return; }
        if (funct == 0x00 && instr == 0) { snprintf(out, outsz, "nop"); return; }
        if (funct == 0x00 || funct == 0x02 || funct == 0x03) /* SLL/SRL/SRA */
            snprintf(out, outsz, "%s %s, %s, %u", mn, reg_name(rd), reg_name(rt), sa);
        else if (funct == 0x38 || funct == 0x3A || funct == 0x3B) /* DSLL/DSRL/DSRA */
            snprintf(out, outsz, "%s %s, %s, %u", mn, reg_name(rd), reg_name(rt), sa);
        else if (funct == 0x3C || funct == 0x3E || funct == 0x3F) /* DSLL32/DSRL32/DSRA32 */
            snprintf(out, outsz, "%s %s, %s, %u", mn, reg_name(rd), reg_name(rt), sa);
        else if (funct == 0x08) /* JR */
            snprintf(out, outsz, "jr %s", reg_name(rs));
        else if (funct == 0x09) /* JALR */
            snprintf(out, outsz, "jalr %s, %s", reg_name(rd), reg_name(rs));
        else if (funct == 0x0C) /* SYSCALL */
            snprintf(out, outsz, "syscall");
        else if (funct == 0x0D) /* BREAK */
            snprintf(out, outsz, "break");
        else if (funct == 0x0F) /* SYNC */
            snprintf(out, outsz, "sync");
        else if (funct == 0x10 || funct == 0x12) /* MFHI/MFLO */
            snprintf(out, outsz, "%s %s", mn, reg_name(rd));
        else if (funct == 0x11 || funct == 0x13) /* MTHI/MTLO */
            snprintf(out, outsz, "%s %s", mn, reg_name(rs));
        else if (funct == 0x18 || funct == 0x19 || funct == 0x1A || funct == 0x1B) /* MULT/MULTU/DIV/DIVU */
            snprintf(out, outsz, "%s %s, %s", mn, reg_name(rs), reg_name(rt));
        else if (funct == 0x28 || funct == 0x29) /* MFSA/MTSA */
            snprintf(out, outsz, "%s %s", mn, reg_name(funct == 0x28 ? rd : rs));
        else if (funct >= 0x30 && funct <= 0x36) /* trap-on-condition */
            snprintf(out, outsz, "%s %s, %s", mn, reg_name(rs), reg_name(rt));
        else /* generic 3-reg ALU: ADD/ADDU/SUB/SUBU/AND/OR/XOR/NOR/SLT/SLTU/DADDU/DSUBU/xxV shifts/MOVZ/MOVN */
            snprintf(out, outsz, "%s %s, %s, %s", mn, reg_name(rd), reg_name(rs), reg_name(rt));
        return;
    }
    case 0x01: { /* REGIMM */
        const char *mn = regimm_mn(rt);
        uint32_t target = pc + 4 + ((uint32_t)(int32_t)imm << 2);
        if (!mn) { snprintf(out, outsz, "regimm.0x%02X rs=%s imm=0x%04X", rt, reg_name(rs), uimm); return; }
        snprintf(out, outsz, "%s %s, 0x%08X", mn, reg_name(rs), target);
        return;
    }
    case 0x02: case 0x03: { /* J / JAL */
        uint32_t target = (pc & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2);
        snprintf(out, outsz, "%s 0x%08X", op == 0x02 ? "j" : "jal", target);
        return;
    }
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x14: case 0x15: case 0x16: case 0x17: { /* BEQ/BNE/BLEZ/BGTZ + likely */
        static const char *names[] = { "beq","bne","blez","bgtz" };
        static const char *namesl[] = { "beql","bnel","blezl","bgtzl" };
        uint32_t target = pc + 4 + ((uint32_t)(int32_t)imm << 2);
        int idx = (op <= 0x07) ? (int)(op - 0x04) : (int)(op - 0x14);
        const char *mn = (op <= 0x07) ? names[idx] : namesl[idx];
        if (op == 0x04 || op == 0x05 || op == 0x14 || op == 0x15) /* two-reg compare */
            snprintf(out, outsz, "%s %s, %s, 0x%08X", mn, reg_name(rs), reg_name(rt), target);
        else /* BLEZ/BGTZ vs zero */
            snprintf(out, outsz, "%s %s, 0x%08X", mn, reg_name(rs), target);
        return;
    }
    case 0x08: snprintf(out, outsz, "addi %s, %s, %d", reg_name(rt), reg_name(rs), imm); return;
    case 0x09: snprintf(out, outsz, "addiu %s, %s, %d", reg_name(rt), reg_name(rs), imm); return;
    case 0x0A: snprintf(out, outsz, "slti %s, %s, %d", reg_name(rt), reg_name(rs), imm); return;
    case 0x0B: snprintf(out, outsz, "sltiu %s, %s, %d", reg_name(rt), reg_name(rs), imm); return;
    case 0x0C: snprintf(out, outsz, "andi %s, %s, 0x%04X", reg_name(rt), reg_name(rs), uimm); return;
    case 0x0D: snprintf(out, outsz, "ori %s, %s, 0x%04X", reg_name(rt), reg_name(rs), uimm); return;
    case 0x0E: snprintf(out, outsz, "xori %s, %s, 0x%04X", reg_name(rt), reg_name(rs), uimm); return;
    case 0x0F: snprintf(out, outsz, "lui %s, 0x%04X", reg_name(rt), uimm); return;
    case 0x18: snprintf(out, outsz, "daddi %s, %s, %d", reg_name(rt), reg_name(rs), imm); return;
    case 0x19: snprintf(out, outsz, "daddiu %s, %s, %d", reg_name(rt), reg_name(rs), imm); return;
    case 0x10: { /* COP0 */
        if (rs == 0x00) { snprintf(out, outsz, "mfc0 %s, $%u", reg_name(rt), rd); return; }
        if (rs == 0x04) { snprintf(out, outsz, "mtc0 %s, $%u", reg_name(rt), rd); return; }
        if (rs == 0x10) { /* CO=1, funct-selected */
            switch (funct) {
            case 0x01: snprintf(out, outsz, "tlbr"); return;
            case 0x02: snprintf(out, outsz, "tlbwi"); return;
            case 0x06: snprintf(out, outsz, "tlbwr"); return;
            case 0x08: snprintf(out, outsz, "tlbp"); return;
            case 0x10: snprintf(out, outsz, "rfe"); return;
            case 0x18: snprintf(out, outsz, "eret"); return;
            case 0x38: snprintf(out, outsz, "ei"); return;
            case 0x39: snprintf(out, outsz, "di"); return;
            default: snprintf(out, outsz, "cop0.co.0x%02X", funct); return;
            }
        }
        snprintf(out, outsz, "cop0.0x%02X rt=%s rd=%u", rs, reg_name(rt), rd);
        return;
    }
    case 0x11: { /* COP1 (FPU) */
        if (rs == 0x00) { snprintf(out, outsz, "mfc1 %s, $f%u", reg_name(rt), rd); return; }
        if (rs == 0x02) { snprintf(out, outsz, "cfc1 %s, $%u", reg_name(rt), rd); return; }
        if (rs == 0x04) { snprintf(out, outsz, "mtc1 %s, $f%u", reg_name(rt), rd); return; }
        if (rs == 0x06) { snprintf(out, outsz, "ctc1 %s, $%u", reg_name(rt), rd); return; }
        if (rs == 0x08) { /* BC1 */
            uint32_t target = pc + 4 + ((uint32_t)(int32_t)imm << 2);
            static const char *bc[] = { "bc1f","bc1t","bc1fl","bc1tl" };
            if (rt <= 3) { snprintf(out, outsz, "%s 0x%08X", bc[rt], target); return; }
            snprintf(out, outsz, "cop1.bc.0x%02X 0x%08X", rt, target); return;
        }
        if (rs == 0x10) { /* COP1.S, fd=sa fs=rd ft=rt */
            switch (funct) {
            case 0x00: snprintf(out, outsz, "add.s $f%u, $f%u, $f%u", sa, rd, rt); return;
            case 0x01: snprintf(out, outsz, "sub.s $f%u, $f%u, $f%u", sa, rd, rt); return;
            case 0x02: snprintf(out, outsz, "mul.s $f%u, $f%u, $f%u", sa, rd, rt); return;
            case 0x03: snprintf(out, outsz, "div.s $f%u, $f%u, $f%u", sa, rd, rt); return;
            case 0x04: snprintf(out, outsz, "sqrt.s $f%u, $f%u", sa, rt); return;
            case 0x05: snprintf(out, outsz, "abs.s $f%u, $f%u", sa, rd); return;
            case 0x06: snprintf(out, outsz, "mov.s $f%u, $f%u", sa, rd); return;
            case 0x07: snprintf(out, outsz, "neg.s $f%u, $f%u", sa, rd); return;
            case 0x16: snprintf(out, outsz, "rsqrt.s $f%u, $f%u, $f%u", sa, rd, rt); return;
            case 0x18: snprintf(out, outsz, "adda.s $f%u, $f%u", rd, rt); return;
            case 0x19: snprintf(out, outsz, "suba.s $f%u, $f%u", rd, rt); return;
            case 0x1A: snprintf(out, outsz, "mula.s $f%u, $f%u", rd, rt); return;
            case 0x1C: snprintf(out, outsz, "madd.s $f%u, $f%u, $f%u", sa, rd, rt); return;
            case 0x1D: snprintf(out, outsz, "msub.s $f%u, $f%u, $f%u", sa, rd, rt); return;
            case 0x1E: snprintf(out, outsz, "madda.s $f%u, $f%u", rd, rt); return;
            case 0x1F: snprintf(out, outsz, "msuba.s $f%u, $f%u", rd, rt); return;
            case 0x24: snprintf(out, outsz, "cvt.w.s $f%u, $f%u", sa, rd); return;
            case 0x28: snprintf(out, outsz, "max.s $f%u, $f%u, $f%u", sa, rd, rt); return;
            case 0x29: snprintf(out, outsz, "min.s $f%u, $f%u, $f%u", sa, rd, rt); return;
            case 0x32: snprintf(out, outsz, "c.eq.s $f%u, $f%u", rd, rt); return;
            case 0x34: snprintf(out, outsz, "c.lt.s $f%u, $f%u", rd, rt); return;
            case 0x36: snprintf(out, outsz, "c.le.s $f%u, $f%u", rd, rt); return;
            default: snprintf(out, outsz, "cop1.s.0x%02X", funct); return;
            }
        }
        if (rs == 0x14) { /* COP1.W */
            if (funct == 0x20) { snprintf(out, outsz, "cvt.s.w $f%u, $f%u", sa, rd); return; }
            snprintf(out, outsz, "cop1.w.0x%02X", funct); return;
        }
        snprintf(out, outsz, "cop1.0x%02X", rs);
        return;
    }
    case 0x12: { /* COP2 (VU0 macro mode) - top-level transfers only, see file header */
        if (rs == 0x00) { snprintf(out, outsz, "mfc2 %s, $vi%u", reg_name(rt), rd); return; }
        if (rs == 0x01) { snprintf(out, outsz, "qmfc2 %s, $vf%u", reg_name(rt), rd); return; }
        if (rs == 0x02) { snprintf(out, outsz, "cfc2 %s, $vi%u", reg_name(rt), rd); return; }
        if (rs == 0x04) { snprintf(out, outsz, "mtc2 %s, $vi%u", reg_name(rt), rd); return; }
        if (rs == 0x05) { snprintf(out, outsz, "qmtc2 %s, $vf%u", reg_name(rt), rd); return; }
        if (rs == 0x06) { snprintf(out, outsz, "ctc2 %s, $vi%u", reg_name(rt), rd); return; }
        if (rs == 0x08) {
            uint32_t target = pc + 4 + ((uint32_t)(int32_t)imm << 2);
            snprintf(out, outsz, "cop2.bc.0x%02X 0x%08X", rt, target); return;
        }
        snprintf(out, outsz, "cop2.macro rs=0x%02X (VU0 arithmetic, not decoded - see file header)", rs);
        return;
    }
    case 0x1C: { /* MMI */
        if (funct == 0x08) { const char *m = mmi0_mn(sa); if (m) snprintf(out, outsz, "%s %s, %s, %s", m, reg_name(rd), reg_name(rs), reg_name(rt)); else snprintf(out, outsz, "mmi0.0x%02X rd=%s rs=%s rt=%s", sa, reg_name(rd), reg_name(rs), reg_name(rt)); return; }
        if (funct == 0x28) { const char *m = mmi1_mn(sa); if (m) snprintf(out, outsz, "%s %s, %s, %s", m, reg_name(rd), reg_name(rs), reg_name(rt)); else snprintf(out, outsz, "mmi1.0x%02X rd=%s rs=%s rt=%s", sa, reg_name(rd), reg_name(rs), reg_name(rt)); return; }
        if (funct == 0x09) { const char *m = mmi2_mn(sa); if (m) snprintf(out, outsz, "%s %s, %s, %s", m, reg_name(rd), reg_name(rs), reg_name(rt)); else snprintf(out, outsz, "mmi2.0x%02X rd=%s rs=%s rt=%s", sa, reg_name(rd), reg_name(rs), reg_name(rt)); return; }
        if (funct == 0x29) { const char *m = mmi3_mn(sa); if (m) snprintf(out, outsz, "%s %s, %s, %s", m, reg_name(rd), reg_name(rs), reg_name(rt)); else snprintf(out, outsz, "mmi3.0x%02X rd=%s rs=%s rt=%s", sa, reg_name(rd), reg_name(rs), reg_name(rt)); return; }
        {
            const char *mn = mmi_mn(funct);
            if (!mn) { snprintf(out, outsz, "mmi.0x%02X rd=%s rs=%s rt=%s sa=%u", funct, reg_name(rd), reg_name(rs), reg_name(rt), sa); return; }
            snprintf(out, outsz, "%s %s, %s, %s", mn, reg_name(rd), reg_name(rs), reg_name(rt));
        }
        return;
    }
    case 0x20: snprintf(out, outsz, "lb %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x21: snprintf(out, outsz, "lh %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x22: snprintf(out, outsz, "lwl %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x23: snprintf(out, outsz, "lw %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x24: snprintf(out, outsz, "lbu %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x25: snprintf(out, outsz, "lhu %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x26: snprintf(out, outsz, "lwr %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x27: snprintf(out, outsz, "lwu %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x28: snprintf(out, outsz, "sb %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x29: snprintf(out, outsz, "sh %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x2A: snprintf(out, outsz, "swl %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x2B: snprintf(out, outsz, "sw %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x2C: snprintf(out, outsz, "sdl %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x2D: snprintf(out, outsz, "sdr %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x2E: snprintf(out, outsz, "swr %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x2F: snprintf(out, outsz, "cache 0x%02X, %d(%s)", rt, imm, reg_name(rs)); return;
    case 0x31: snprintf(out, outsz, "lwc1 $f%u, %d(%s)", rt, imm, reg_name(rs)); return;
    case 0x33: snprintf(out, outsz, "pref %d(%s)", imm, reg_name(rs)); return;
    case 0x1A: snprintf(out, outsz, "ldl %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x1B: snprintf(out, outsz, "ldr %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x1E: snprintf(out, outsz, "lq %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x1F: snprintf(out, outsz, "sq %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x36: snprintf(out, outsz, "lqc2 $vf%u, %d(%s)", rt, imm, reg_name(rs)); return;
    case 0x3E: snprintf(out, outsz, "sqc2 $vf%u, %d(%s)", rt, imm, reg_name(rs)); return;
    case 0x37: snprintf(out, outsz, "ld %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x39: snprintf(out, outsz, "swc1 $f%u, %d(%s)", rt, imm, reg_name(rs)); return;
    case 0x3F: snprintf(out, outsz, "sd %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    default:
        snprintf(out, outsz, "op.0x%02X rs=%s rt=%s imm=0x%04X (unrecognized)", op, reg_name(rs), reg_name(rt), uimm);
        return;
    }
    (void)buf;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <raw_dump_file> <base_addr_hex> <start_addr_hex> <count>\n", argv[0]);
        fprintf(stderr, "  example (BIOS reset vector): %s scph10000.bin 0xBFC00000 0xBFC00000 64\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];
    uint32_t base = (uint32_t)strtoul(argv[2], NULL, 16);
    uint32_t start = (uint32_t)strtoul(argv[3], NULL, 16);
    long count = strtol(argv[4], NULL, 10);

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);

    for (long i = 0; i < count; i++) {
        uint32_t addr = start + (uint32_t)(i * 4);
        uint32_t file_off = addr - base;
        int ok = 0;
        uint32_t word = rd_word(buf, (size_t)sz, file_off, &ok);
        if (!ok) { printf("0x%08X: <out of range>\n", addr); continue; }
        char line[192];
        disasm_one(word, addr, line, sizeof(line));
        printf("0x%08X: %08X  %s\n", addr, word, line);
    }
    free(buf);
    return 0;
}
