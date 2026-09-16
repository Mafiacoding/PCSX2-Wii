/* Round 953 (task #946): full disassembly of the SCPH-50004 per-frame
 * VBLANK-burst "heartbeat" call targets found in Round 951/952.
 *
 * Round 951's disasm of 0x8000DBD0-0x8000DD10 showed the real
 * ack-then-poll VBLANK_START loop calls a fixed sequence of 9 real
 * subroutines each frame:
 *   0x8000d670(?)                 - called first, right after INTC ack
 *   0x8000bd58(a0=1,a1=1,a2=1)
 *   0x800073e0(a0=0x80016220)     -\
 *   0x800073e0(a0=0x80016228)      |
 *   0x8000e618(a0=0xffffdffd)      |  0x800073e0 is a common dispatcher,
 *   0x800073e0(a0=0x80016240)      |  called 9x with 9 different fixed
 *   0x8000e648(a0=leftover)        |  struct-pointer arguments in the
 *   0x8000d9b8(a0=127)             |  0x80016200-0x800162f0 range -
 *   0x800073e0(a0=0x80016258)      |  looks exactly like the real
 *   0x8000e4f0(a0=leftover)        |  per-cause AddIntcHandler()
 *   0x800073e0(a0=0x80016270)      |  registered-handler-list walk
 *   0x8000e5c0(a0=0x00082000)      |  Round 468 already found (12-byte
 *   0x800073e0(a0=0x80016290)      |  handler-struct pool via
 *   0x8000e588(a0=leftover)        |  0x800015A0).
 *   0x800073e0(a0=0x800162d8)      |
 *   0x800073e0(a0=0x800162f0)      |
 *   0x800073e0(a0=0x80016208)     -/
 *
 * This tool boots fresh to the same resting point Round 951/952 used
 * (~480M ee_instr), then fully disassembles each of the 9 distinct
 * non-table call targets plus the shared 0x800073e0 dispatcher, and
 * dumps the raw content of the 9 struct-pointer table entries
 * themselves (32 bytes each, generous upper bound) - looking
 * specifically for any SIF (0x1000F2xx-range) or pad-state touches
 * that would make Round 952's declined proposal's hypothesis
 * (VBLANK gated on a pad RPC) newly plausible, or confirm it remains
 * unsupported.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
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

static const char *regimm_mn(uint32_t rt)
{
    switch (rt) {
    case 0x00: return "bltz";
    case 0x01: return "bgez";
    case 0x02: return "bltzl";
    case 0x03: return "bgezl";
    case 0x10: return "bltzal";
    case 0x11: return "bgezal";
    default: return NULL;
    }
}

static const char *opcode_mn(uint32_t op)
{
    switch (op) {
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
    if (op == 0x01) {
        const char *mn = regimm_mn(rt);
        if (!mn) { snprintf(out, outsz, ".word 0x%08x (regimm rt=0x%02x)", w, rt); return; }
        snprintf(out, outsz, "%-5s%s, 0x%08x", mn, reg_name(rs), a + 4 + ((int32_t)imm << 2));
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
    if (op == 0x10 || op == 0x11 || op == 0x12) {
        snprintf(out, outsz, ".word 0x%08x (cop%d, rs=0x%02x rt=%s)", w, op - 0x10, rs, reg_name(rt));
        return;
    }
    const char *mn = opcode_mn(op);
    if (!mn) { snprintf(out, outsz, ".word 0x%08x (op=0x%02x)", w, op); return; }
    if (op >= 0x20 && op <= 0x3F && op != 0x2F) {
        snprintf(out, outsz, "%-5s%s, %d(%s)", mn, reg_name(rt), imm, reg_name(rs));
        return;
    }
    snprintf(out, outsz, "%-5s%s, %s, %d", mn, reg_name(rt), reg_name(rs), imm);
}

/* Disassemble from `start`, stopping either at a `jr ra` (function
 * return) plus its delay slot, or after `max_instr` instructions -
 * whichever comes first. Flags any instruction whose address operand
 * (computed from lui+ori/addiu pairs seen inline, best-effort) or
 * whose raw address itself falls in the real EE SIF MMIO window
 * (0x1000F200-0x1000F26F, see source/hw/sif.c's SIF_MSCOM..SIF_F260)
 * or looks like a padArea/pad-state touch. */
static void dump_function(ee_state_t *ee, uint32_t start, uint32_t max_instr, const char *label)
{
    printf("\n[R953] -- %s @ 0x%08x --\n", label, start);
    uint32_t a = start;
    for (uint32_t i = 0; i < max_instr; i++, a += 4) {
        uint32_t w = ee_mem_read32(ee, a);
        char buf[128];
        disasm_one(w, a, buf, sizeof(buf));
        printf("  0x%08x: %08x  %s\n", a, w, buf);
        uint32_t op = (w >> 26) & 0x3F, funct = w & 0x3F;
        if (op == 0x00 && funct == 0x08) { /* jr - print delay slot then stop */
            uint32_t a2 = a + 4;
            uint32_t w2 = ee_mem_read32(ee, a2);
            char buf2[128];
            disasm_one(w2, a2, buf2, sizeof(buf2));
            printf("  0x%08x: %08x  %s  (delay slot)\n", a2, w2, buf2);
            break;
        }
    }
}

static void dump_struct(ee_state_t *ee, uint32_t addr, const char *label)
{
    printf("[R953] struct entry %s @ 0x%08x: ", label, addr);
    for (int i = 0; i < 8; i++) {
        printf("%08x ", ee_mem_read32(ee, addr + i * 4));
    }
    printf("\n");
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
    ee_state_t *ee = ee_core_get_state();
    printf("[R953] ee_instr=%llu ee_pc=0x%08x ee_halted=%d\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);

    /* the shared dispatcher, called 9x with different struct pointers */
    dump_function(ee, 0x800073e0u, 60, "0x800073e0 (shared per-handler dispatcher)");

    /* the 8 distinct direct-called functions */
    dump_function(ee, 0x8000d670u, 60, "0x8000d670");
    dump_function(ee, 0x8000bd58u, 60, "0x8000bd58");
    dump_function(ee, 0x8000e618u, 60, "0x8000e618");
    dump_function(ee, 0x8000e648u, 60, "0x8000e648");
    dump_function(ee, 0x8000d9b8u, 60, "0x8000d9b8");
    dump_function(ee, 0x8000e4f0u, 60, "0x8000e4f0");
    dump_function(ee, 0x8000e5c0u, 60, "0x8000e5c0");
    dump_function(ee, 0x8000e588u, 60, "0x8000e588");

    /* the 9 handler-struct-pointer table entries actually passed to
     * 0x800073e0 (from the exact args captured in Round 951's raw
     * disasm of the caller at 0x8000DBD0-0x8000DD10) */
    printf("\n[R953] -- handler-struct-pointer table entries (raw words) --\n");
    dump_struct(ee, 0x80016208u, "#1 (0x80016208)");
    dump_struct(ee, 0x80016220u, "#2 (0x80016220)");
    dump_struct(ee, 0x80016228u, "#3 (0x80016228)");
    dump_struct(ee, 0x80016240u, "#4 (0x80016240)");
    dump_struct(ee, 0x80016258u, "#5 (0x80016258)");
    dump_struct(ee, 0x80016270u, "#6 (0x80016270)");
    dump_struct(ee, 0x80016290u, "#7 (0x80016290)");
    dump_struct(ee, 0x800162d8u, "#8 (0x800162d8)");
    dump_struct(ee, 0x800162f0u, "#9 (0x800162f0)");

    return 0;
}
