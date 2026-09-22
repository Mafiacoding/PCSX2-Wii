/* Round 990 (task #970, follow-up to task #969/Round 989): characterizes
 * the new SCPH-50004 diskless-boot resting point pc=0x0026fe9c reached
 * once Round 989's SetupThread exception-vectoring fix broke the
 * "Restart Without Memory Clear" loop. STATUS.md's Round 989 writeup
 * already disassembly-verified this address is real, well-formed OSDSYS
 * code (a small array-index/accessor function), not a crash or wild
 * jump - this round asks the next real question: is control flow
 * actually PARKED there (single static address, dead loop / true halt-
 * equivalent), or OSCILLATING through a small set of nearby addresses
 * (a genuine polling loop, e.g. a VBLANK-wait), and whether GS PMODE/
 * DISPLAY2 show any sign of organic display setup beginning.
 *
 * disasm_one()/reg_name()/opcode_mn()/special_mn() below are the same
 * verbatim-reused block already used by tools/round655-ee-disasm/,
 * round944, round947, round950, round951's own analyze.c tools.
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

#define R990_TARGET_PC 0x0026fe9cu

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
    uint64_t chunk = 1000000ull, done = 0;
    ee_state_t *ee_probe = ee_core_get_state();
    int reached_target = 0;
    while (done < budget && !ee_probe->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        if (ee_probe->pc == R990_TARGET_PC) { reached_target = 1; break; }
    }

    ee_state_t  *ee  = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();
    gs_state_t  *gs  = gs_get_state();

    printf("[R990] reached_target_pc=%d after ee_instr=%llu (budget=%llu)\n",
           reached_target, (unsigned long long)ee->instructions_executed,
           (unsigned long long)budget);
    printf("[R990] ee_pc=0x%08x ee_halted=%d\n", ee->pc, ee->halted);
    printf("[R990] GPRs: v0=0x%08llx v1=0x%08llx a0=0x%08llx a1=0x%08llx a2=0x%08llx a3=0x%08llx ra=0x%08llx sp=0x%08llx gp=0x%08llx\n",
           (unsigned long long)ee->gpr[2].ud0, (unsigned long long)ee->gpr[3].ud0,
           (unsigned long long)ee->gpr[4].ud0, (unsigned long long)ee->gpr[5].ud0,
           (unsigned long long)ee->gpr[6].ud0, (unsigned long long)ee->gpr[7].ud0,
           (unsigned long long)ee->gpr[31].ud0, (unsigned long long)ee->gpr[29].ud0,
           (unsigned long long)ee->gpr[28].ud0);
    printf("[R990] iop_pc=0x%08x iop_halted=%d iop_idle=%u iop_sched_ticks=%llu\n",
           iop->pc, iop->halted, (unsigned)iop->idle, (unsigned long long)iop->sched_ticks);
    printf("[R990] gs pmode=0x%02llx smode2=0x%02llx dispfb2=0x%08llx display2=0x%016llx "
           "dispfb1=0x%08llx display1=0x%016llx\n",
           (unsigned long long)gs->pmode, (unsigned long long)gs->smode2,
           (unsigned long long)gs->dispfb2, (unsigned long long)gs->display2,
           (unsigned long long)gs->dispfb1, (unsigned long long)gs->display1);
    printf("[R990] R940/941-force fingerprint check: pmode==0x02 && dispfb2==0x1400 && "
           "display2==0x001bf27f0003227c -> %s\n",
           (gs->pmode == 0x02 && gs->dispfb2 == 0x1400 && gs->display2 == 0x001bf27f0003227cull)
               ? "MATCH (synthetic diagnostic force, not organic)"
               : "NO MATCH (differs from known force fingerprint)");

    printf("\n[R990] Disassembly window around target pc 0x%08x (-8/+8 words):\n", R990_TARGET_PC);
    dump_range(ee, R990_TARGET_PC - 32, R990_TARGET_PC + 32);

    /* Oscillation check: single-step a further small window, logging
     * every DISTINCT pc visited, to determine whether this is a true
     * static park (pc never changes across steps - e.g. a 1-instruction
     * self-branch) or a genuine multi-address polling loop. */
    printf("\n[R990] Oscillation trace: distinct pc values over next 4096 ee_core_step() calls:\n");
    {
        uint32_t seen[64]; int nseen = 0;
        for (int i = 0; i < 4096 && !ee->halted; i++) {
            uint32_t p = ee->pc;
            int found = 0;
            for (int j = 0; j < nseen; j++) if (seen[j] == p) { found = 1; break; }
            if (!found && nseen < 64) seen[nseen++] = p;
            ee_core_step();
        }
        printf("  %d distinct pc value(s) seen:", nseen);
        for (int j = 0; j < nseen; j++) printf(" 0x%08x", seen[j]);
        printf("\n");
    }

    printf("\n[R990] Post-oscillation-trace GS state: pmode=0x%02llx dispfb2=0x%08llx display2=0x%016llx\n",
           (unsigned long long)gs->pmode, (unsigned long long)gs->dispfb2,
           (unsigned long long)gs->display2);

    printf("\n[R990] Caller-context disassembly 0x0026f7c0-0x0026f840:\n");
    dump_range(ee, 0x0026f7c0u, 0x0026f840u);
    printf("\n[R990] Table dump at 0x0040dc70-0x0040dcd0 (word-per-line, hex value):\n");
    for (uint32_t a = 0x0040dc70u; a <= 0x0040dcd0u; a += 4) {
        printf("  0x%08x: %08x\n", a, ee_mem_read32(ee, a));
    }

    return 0;
}
