/*
 * ee_core.c - minimal R5900 (Emotion Engine) interpreter
 *
 * Implements just enough of the MIPS III base ISA (a handful of ALU,
 * branch and load/store opcodes) to decode and step through the very
 * first instructions the PS2 BIOS reset vector executes. It does NOT
 * implement:
 *   - MMI (multimedia) instructions - used pervasively by real BIOS code
 *   - COP1 (FPU), COP2 (VU0 macro mode)
 *   - TLB / MMU, exceptions, interrupts
 *   - The IOP (I/O Processor, separate MIPS core) or its BIOS side-channel
 *   - The 128-bit register upper halves
 *
 * In practice this means the interpreter will decode a few dozen to a
 * few hundred instructions from a real BIOS image before hitting an
 * unimplemented opcode and halting cleanly (rather than crashing). That
 * halt is a diagnostic, not a bug - see docs/STATUS.md for what a real
 * implementation would require.
 */

#include "core/ee/ee_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#define EE_RAM_SIZE (32 * 1024 * 1024)

static ee_state_t g_state;

ee_state_t *ee_core_get_state(void) { return &g_state; }

uint32_t ee_mem_read32(ee_state_t *st, uint32_t addr)
{
    /* KSEG1 (0xBFC00000+) maps directly to BIOS ROM, uncached */
    if (addr >= 0xBFC00000u) {
        uint32_t off = addr - 0xBFC00000u;
        if (st->bios && off + 4 <= st->bios->size) {
            uint32_t v;
            memcpy(&v, st->bios->data + off, 4);
            return v;
        }
        return 0;
    }
    uint32_t phys = addr & 0x1FFFFFFFu; /* strip KSEG mapping bits */
    if (phys + 4 <= st->ram_size) {
        uint32_t v;
        memcpy(&v, st->ram + phys, 4);
        return v;
    }
    return 0;
}

void ee_mem_write32(ee_state_t *st, uint32_t addr, uint32_t val)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys + 4 <= st->ram_size) {
        memcpy(st->ram + phys, &val, 4);
    }
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
    g_state.gpr[0] = 0; /* $zero is hardwired */

    return 0;
}

static void halt(const char *reason)
{
    g_state.halted = 1;
    strncpy(g_state.halt_reason, reason, sizeof(g_state.halt_reason) - 1);
}

/* Decodes and executes exactly one instruction. Returns 0 to continue,
 * non-zero if the core halted. */
static int ee_step(void)
{
    uint32_t pc = g_state.pc;
    uint32_t instr = ee_mem_read32(&g_state, pc);

    uint32_t op   = (instr >> 26) & 0x3F;
    uint32_t rs   = (instr >> 21) & 0x1F;
    uint32_t rt   = (instr >> 16) & 0x1F;
    uint32_t rd   = (instr >> 11) & 0x1F;
    int32_t  imm  = (int16_t)(instr & 0xFFFF);
    uint32_t uimm = instr & 0xFFFF;
    uint32_t funct = instr & 0x3F;

    uint32_t this_pc = pc;
    uint32_t fallthrough_pc = g_state.next_pc;
    g_state.pc = fallthrough_pc;
    g_state.next_pc = fallthrough_pc + 4;

    switch (op) {
    case 0x00: /* SPECIAL */
        switch (funct) {
        case 0x00: /* SLL (incl. true NOP = SLL $0,$0,0) */
            g_state.gpr[rd] = (uint32_t)((uint32_t)g_state.gpr[rt] << ((instr >> 6) & 0x1F));
            break;
        case 0x25: /* OR */
            g_state.gpr[rd] = g_state.gpr[rs] | g_state.gpr[rt];
            break;
        case 0x24: /* AND */
            g_state.gpr[rd] = g_state.gpr[rs] & g_state.gpr[rt];
            break;
        case 0x21: /* ADDU */
            g_state.gpr[rd] = (uint32_t)(g_state.gpr[rs] + g_state.gpr[rt]);
            break;
        case 0x08: /* JR */
            g_state.next_pc = (uint32_t)g_state.gpr[rs];
            break;
        default:
            halt("unimplemented SPECIAL funct (likely MMI/COP2 territory)");
            return 1;
        }
        break;

    case 0x0F: /* LUI */
        g_state.gpr[rt] = ((uint32_t)uimm) << 16;
        break;

    case 0x0D: /* ORI */
        g_state.gpr[rt] = g_state.gpr[rs] | uimm;
        break;

    case 0x09: /* ADDIU */
        g_state.gpr[rt] = (uint32_t)((int32_t)g_state.gpr[rs] + imm);
        break;

    case 0x23: /* LW */
        g_state.gpr[rt] = (int32_t)ee_mem_read32(&g_state, (uint32_t)(g_state.gpr[rs] + imm));
        break;

    case 0x2B: /* SW */
        ee_mem_write32(&g_state, (uint32_t)(g_state.gpr[rs] + imm), (uint32_t)g_state.gpr[rt]);
        break;

    case 0x04: /* BEQ */
        if (g_state.gpr[rs] == g_state.gpr[rt])
            g_state.next_pc = this_pc + 4 + (imm << 2);
        break;

    case 0x05: /* BNE */
        if (g_state.gpr[rs] != g_state.gpr[rt])
            g_state.next_pc = this_pc + 4 + (imm << 2);
        break;

    case 0x02: /* J */
        g_state.next_pc = (this_pc & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2);
        break;

    default:
        halt("unimplemented primary opcode");
        return 1;
    }

    g_state.gpr[0] = 0; /* $zero always reads as 0 */
    g_state.instructions_executed++;
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
