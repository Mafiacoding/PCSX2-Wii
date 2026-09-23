/* Round 1047 (task #447/#536/#1009 continuation): direct disassembly
 * of the real Sony IOP kernel code at the untracked execution context
 * Round 1046 found (current_thread_id=0, pc cycling 0x00019050-
 * 0x00019258) - the address range threads 5-8's own parked pc field
 * also sits in (0x00019258). Disassembler body reused verbatim from
 * tools/round655-ee-disasm/disasm.c via tools/round944-iop-halt-178a8/
 * analyze.c's established reuse chain (R3000A is a strict MIPS-I
 * subset of the R5900 table it covers - safe to reuse as-is, same
 * precedent as Round 944/947/953).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/iop/iop_core.h"

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
static const char *special_mn(uint32_t f){switch(f){
    case 0x00:return "sll";case 0x02:return "srl";case 0x03:return "sra";
    case 0x04:return "sllv";case 0x06:return "srlv";case 0x07:return "srav";
    case 0x08:return "jr";case 0x09:return "jalr";case 0x0A:return "movz";
    case 0x0B:return "movn";case 0x0C:return "syscall";case 0x0D:return "break";
    case 0x0F:return "sync";case 0x10:return "mfhi";case 0x11:return "mthi";
    case 0x12:return "mflo";case 0x13:return "mtlo";case 0x18:return "mult";
    case 0x19:return "multu";case 0x1A:return "div";case 0x1B:return "divu";
    case 0x20:return "add";case 0x21:return "addu";case 0x22:return "sub";
    case 0x23:return "subu";case 0x24:return "and";case 0x25:return "or";
    case 0x26:return "xor";case 0x27:return "nor";case 0x2A:return "slt";
    case 0x2B:return "sltu";default:return NULL;}}
static const char *regimm_mn(uint32_t rt){switch(rt){
    case 0x00:return "bltz";case 0x01:return "bgez";case 0x10:return "bltzal";
    case 0x11:return "bgezal";default:return NULL;}}

static void disasm_one(uint32_t w, uint32_t addr, char *out, size_t outsz)
{
    uint32_t op = w>>26, rs=(w>>21)&0x1F, rt=(w>>16)&0x1F, rd=(w>>11)&0x1F;
    uint32_t sh=(w>>6)&0x1F, funct=w&0x3F;
    int16_t imm = (int16_t)(w & 0xFFFF);
    uint32_t uimm = w & 0xFFFF;
    uint32_t jt = (addr & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
    if (w == 0) { snprintf(out, outsz, "nop"); return; }
    if (op == 0x00) {
        const char *mn = special_mn(funct);
        if (!mn) { snprintf(out, outsz, ".word 0x%08x (special funct=0x%02x)", w, funct); return; }
        if (funct==0x08) { snprintf(out, outsz, "jr      $%s", reg_name(rs)); return; }
        if (funct==0x09) { snprintf(out, outsz, "jalr    $%s, $%s", reg_name(rd), reg_name(rs)); return; }
        if (funct==0x00||funct==0x02||funct==0x03) { snprintf(out, outsz, "%-7s $%s, $%s, %u", mn, reg_name(rd), reg_name(rt), sh); return; }
        if (funct>=0x10 && funct<=0x13) { snprintf(out, outsz, "%-7s $%s", mn, reg_name(funct<=0x11?rd:rs)); return; }
        if (funct==0x18||funct==0x19||funct==0x1A||funct==0x1B) { snprintf(out, outsz, "%-7s $%s, $%s", mn, reg_name(rs), reg_name(rt)); return; }
        snprintf(out, outsz, "%-7s $%s, $%s, $%s", mn, reg_name(rd), reg_name(rs), reg_name(rt));
        return;
    }
    if (op == 0x01) {
        const char *mn = regimm_mn(rt);
        snprintf(out, outsz, "%-7s $%s, 0x%08x", mn?mn:"regimm?", reg_name(rs), addr+4+((int32_t)imm<<2));
        return;
    }
    switch (op) {
    case 0x02: snprintf(out, outsz, "j       0x%08x", jt); return;
    case 0x03: snprintf(out, outsz, "jal     0x%08x", jt); return;
    case 0x04: snprintf(out, outsz, "beq     $%s, $%s, 0x%08x", reg_name(rs), reg_name(rt), addr+4+((int32_t)imm<<2)); return;
    case 0x05: snprintf(out, outsz, "bne     $%s, $%s, 0x%08x", reg_name(rs), reg_name(rt), addr+4+((int32_t)imm<<2)); return;
    case 0x06: snprintf(out, outsz, "blez    $%s, 0x%08x", reg_name(rs), addr+4+((int32_t)imm<<2)); return;
    case 0x07: snprintf(out, outsz, "bgtz    $%s, 0x%08x", reg_name(rs), addr+4+((int32_t)imm<<2)); return;
    case 0x08: snprintf(out, outsz, "addi    $%s, $%s, %d", reg_name(rt), reg_name(rs), imm); return;
    case 0x09: snprintf(out, outsz, "addiu   $%s, $%s, %d", reg_name(rt), reg_name(rs), imm); return;
    case 0x0A: snprintf(out, outsz, "slti    $%s, $%s, %d", reg_name(rt), reg_name(rs), imm); return;
    case 0x0B: snprintf(out, outsz, "sltiu   $%s, $%s, %d", reg_name(rt), reg_name(rs), imm); return;
    case 0x0C: snprintf(out, outsz, "andi    $%s, $%s, 0x%04x", reg_name(rt), reg_name(rs), uimm); return;
    case 0x0D: snprintf(out, outsz, "ori     $%s, $%s, 0x%04x", reg_name(rt), reg_name(rs), uimm); return;
    case 0x0E: snprintf(out, outsz, "xori    $%s, $%s, 0x%04x", reg_name(rt), reg_name(rs), uimm); return;
    case 0x0F: snprintf(out, outsz, "lui     $%s, 0x%04x", reg_name(rt), uimm); return;
    case 0x10: snprintf(out, outsz, "cop0    rs=%u rt=%s rd=%u", rs, reg_name(rt), rd); return;
    case 0x20: snprintf(out, outsz, "lb      $%s, %d($%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x21: snprintf(out, outsz, "lh      $%s, %d($%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x23: snprintf(out, outsz, "lw      $%s, %d($%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x24: snprintf(out, outsz, "lbu     $%s, %d($%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x25: snprintf(out, outsz, "lhu     $%s, %d($%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x28: snprintf(out, outsz, "sb      $%s, %d($%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x29: snprintf(out, outsz, "sh      $%s, %d($%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x2B: snprintf(out, outsz, "sw      $%s, %d($%s)", reg_name(rt), imm, reg_name(rs)); return;
    default: snprintf(out, outsz, ".word 0x%08x (op=0x%02x)", w, op); return;
    }
}

static uint32_t iop_ram_word(iop_state_t *iop, uint32_t addr)
{
    addr &= 0x1FFFFF;
    if (addr + 4 > iop->ram_size) return 0xFFFFFFFFu;
    return (uint32_t)iop->ram[addr] | ((uint32_t)iop->ram[addr+1]<<8) |
           ((uint32_t)iop->ram[addr+2]<<16) | ((uint32_t)iop->ram[addr+3]<<24);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <bios> <ckpt>\n", argv[0]); return 1; }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr,"bios load fail\n"); return 1; }
    if (checkpoint_load(argv[2], &bios, &bios, NULL) != 0) { fprintf(stderr,"ckpt load fail\n"); return 1; }
    iop_state_t *iop = iop_core_get_state();
    printf("[R1047] iop pc=0x%08x idle=%d halted=%d\n", iop->pc, iop->idle, iop->halted);
    printf("\n[DISASM] 0x00018F80-0x00019380 (untracked exec region, current_thread_id=0):\n");
    for (uint32_t a = 0x00018F80u; a <= 0x00019380u; a += 4) {
        uint32_t w = iop_ram_word(iop, a);
        char line[192];
        disasm_one(w, a, line, sizeof(line));
        printf("%s0x%08X: %08X  %s\n", (a==iop->pc)?">>>":"   ", a, w, line);
    }
    return 0;
}
