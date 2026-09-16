/* Round 951 (task #447/#536/#887/#944): characterizes the new EE/IOP
 * resting point reached after Round 950's DVE fix on the SCPH-50004
 * boot survey (EE oscillating around 0x8000E5F0-0x8000E600 /
 * 0x8000DBE0-0x8000DD04 / 0x00100B68-0x00100C7C; IOP parked at
 * pc=0x00155C00, iop_moved=0 across a 55,000,000-instruction sampled
 * window in the Round 951 survey run).
 *
 * Answers three direct questions:
 *  (1) is the IOP genuinely idle (deliberate park, per Round 943's
 *      documented idle mechanism) or truly stuck executing/looping?
 *  (2) is the current GS pmode=0x02/dispfb2=0x1400/display2=
 *      0x001bf27f0003227c state organic (BIOS wrote it) or the
 *      pre-existing Round 940/941 diagnostic force (still firing
 *      unconditionally at instr==25,000,000 regardless of BIOS)?
 *  (3) what is the EE's resting-loop code actually doing at
 *      0x8000E5F0-0x8000E600 and 0x00100B68-0x00100C90?
 *
 * disasm_one() below is the same verbatim-reused block originally from
 * tools/round655-ee-disasm/disasm.c (already re-used by Rounds 944/
 * 947/950's own analyze.c tools).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/gs.h"

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
    case 0x2A: return "slt";
    case 0x2B: return "sltu";
    case 0x2C: return "dadd";
    case 0x2D: return "daddu";
    case 0x2E: return "dsub";
    case 0x2F: return "dsubu";
    case 0x38: return "dsll";
    case 0x3A: return "dsrl";
    case 0x3B: return "dsra";
    case 0x3C: return "dsll32";
    case 0x3E: return "dsrl32";
    case 0x3F: return "dsra32";
    default: return NULL;
    }
}

static const char *opcode_mn(uint32_t op)
{
    switch (op) {
    case 0x01: return "regimm";
    case 0x02: return "j";
    case 0x03: return "jal";
    case 0x04: return "beq";
    case 0x05: return "bne";
    case 0x06: return "blez";
    case 0x07: return "bgtz";
    case 0x08: return "addi";
    case 0x09: return "addiu";
    case 0x0A: return "slti";
    case 0x0B: return "sltiu";
    case 0x0C: return "andi";
    case 0x0D: return "ori";
    case 0x0E: return "xori";
    case 0x0F: return "lui";
    case 0x10: return "cop0";
    case 0x11: return "cop1";
    case 0x12: return "cop2";
    case 0x14: return "beql";
    case 0x15: return "bnel";
    case 0x16: return "blezl";
    case 0x17: return "bgtzl";
    case 0x1A: return "ldl";
    case 0x1B: return "ldr";
    case 0x1E: return "lq";
    case 0x1F: return "sq";
    case 0x20: return "lb";
    case 0x21: return "lh";
    case 0x22: return "lwl";
    case 0x23: return "lw";
    case 0x24: return "lbu";
    case 0x25: return "lhu";
    case 0x26: return "lwr";
    case 0x27: return "lwu";
    case 0x28: return "sb";
    case 0x29: return "sh";
    case 0x2A: return "swl";
    case 0x2B: return "sw";
    case 0x2E: return "swr";
    case 0x2F: return "cache";
    case 0x31: return "lwc1";
    case 0x36: return "lqc2";
    case 0x37: return "ld";
    case 0x39: return "swc1";
    case 0x3E: return "sqc2";
    case 0x3F: return "sd";
    default: return NULL;
    }
}

static void disasm_one(uint32_t w, uint32_t a, char *out, size_t outsz)
{
    uint32_t op = (w >> 26) & 0x3F;
    uint32_t rs = (w >> 21) & 0x1F;
    uint32_t rt = (w >> 16) & 0x1F;
    uint32_t rd = (w >> 11) & 0x1F;
    uint32_t sh = (w >> 6) & 0x1F;
    uint32_t funct = w & 0x3F;
    int16_t imm = (int16_t)(w & 0xFFFF);
    uint32_t target = (w & 0x03FFFFFF) << 2;

    if (w == 0) { snprintf(out, outsz, "nop"); return; }

    if (op == 0x00) {
        const char *mn = special_mn(funct);
        if (!mn) { snprintf(out, outsz, ".word 0x%08x (special funct=0x%02x)", w, funct); return; }
        if (funct == 0x08) { snprintf(out, outsz, "jr   %s", reg_name(rs)); return; }
        if (funct == 0x09) { snprintf(out, outsz, "jalr %s, %s", reg_name(rd), reg_name(rs)); return; }
        if (funct >= 0x00 && funct <= 0x07 && funct != 0x01 && funct != 0x05) {
            snprintf(out, outsz, "%-5s%s, %s, %u", mn, reg_name(rd), reg_name(rt), sh);
            return;
        }
        snprintf(out, outsz, "%-5s%s, %s, %s", mn, reg_name(rd), reg_name(rs), reg_name(rt));
        return;
    }

    if (op == 0x02 || op == 0x03) {
        snprintf(out, outsz, "%-5s0x%08x", opcode_mn(op), (a & 0xF0000000u) | target);
        return;
    }
    if (op == 0x04 || op == 0x05 || op == 0x14 || op == 0x15) {
        snprintf(out, outsz, "%-5s%s, %s, 0x%08x", opcode_mn(op), reg_name(rs), reg_name(rt),
                 a + 4 + ((int32_t)imm << 2));
        return;
    }
    if (op == 0x06 || op == 0x07 || op == 0x16 || op == 0x17) {
        snprintf(out, outsz, "%-5s%s, 0x%08x", opcode_mn(op), reg_name(rs),
                 a + 4 + ((int32_t)imm << 2));
        return;
    }
    if (op == 0x0F) { snprintf(out, outsz, "lui  %s, 0x%04x", reg_name(rt), (uint16_t)imm); return; }
    const char *mn = opcode_mn(op);
    if (!mn) { snprintf(out, outsz, ".word 0x%08x (op=0x%02x)", w, op); return; }
    if (op >= 0x20 && op <= 0x3F && op != 0x2F) {
        snprintf(out, outsz, "%-5s%s, %d(%s)", mn, reg_name(rt), imm, reg_name(rs));
        return;
    }
    snprintf(out, outsz, "%-5s%s, %s, %d", mn, reg_name(rt), reg_name(rs), imm);
}

static void dump_range(ee_state_t *ee, uint32_t lo, uint32_t hi)
{
    for (uint32_t a = lo; a <= hi; a += 4) {
        uint32_t w = ee_mem_read32(ee, a);
        char buf[128];
        disasm_one(w, a, buf, sizeof(buf));
        printf("  0x%08x: %08x  %s\n", a, w, buf);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <budget>\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    uint64_t budget = strtoull(argv[2], NULL, 10);
    uint64_t chunk = 5000000ull, done = 0;
    ee_state_t *ee_probe = ee_core_get_state();
    while (done < budget && !ee_probe->halted) {
        system_run_interleaved(chunk);
        done += chunk;
    }

    ee_state_t  *ee  = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();
    gs_state_t  *gs  = gs_get_state();

    printf("[R951] ee_instr=%llu ee_pc=0x%08x ee_halted=%d\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);
    printf("[R951] iop_pc=0x%08x iop_halted=%d iop_idle=%u iop_sched_ticks=%llu\n",
           iop->pc, iop->halted, (unsigned)iop->idle, (unsigned long long)iop->sched_ticks);
    printf("[R951] gs pmode=0x%02x smode2=0x%02x dispfb2=0x%08x display2=0x%016llx "
           "dispfb1=0x%08x display1=0x%016llx\n",
           gs->pmode, gs->smode2, gs->dispfb2, (unsigned long long)gs->display2,
           gs->dispfb1, (unsigned long long)gs->display1);
    printf("[R951] R940/941-force fingerprint check: pmode==0x02 && dispfb2==0x1400 && "
           "display2==0x001bf27f0003227cull -> %s\n",
           (gs->pmode == 0x02 && gs->dispfb2 == 0x1400 && gs->display2 == 0x001bf27f0003227cull)
               ? "MATCH (this is the Round 940/941 synthetic diagnostic force, NOT organic BIOS output)"
               : "NO MATCH (state differs from the known force fingerprint - could be organic)");

    printf("\n[R951] EE resting-oscillation disassembly 0x8000DBD0-0x8000DD10:\n");
    dump_range(ee, 0x8000DBD0u, 0x8000DD10u);
    printf("\n[R951] EE resting-oscillation disassembly 0x8000E5E0-0x8000E610:\n");
    dump_range(ee, 0x8000E5E0u, 0x8000E610u);
    printf("\n[R951] EE resting-oscillation disassembly 0x00100B60-0x00100C90:\n");
    dump_range(ee, 0x00100B60u, 0x00100C90u);
    printf("\n[R951] IOP park-point context, IOP RAM words pc-40..pc+16 (0x%08x):\n", iop->pc);
    for (uint32_t off = (uint32_t)-40; off <= 16; off += 4) {
        uint32_t a = iop->pc + off;
        uint32_t w = iop_mem_read32(iop, a);
        printf("  0x%08x: %08x\n", a, w);
    }

    return 0;
}
