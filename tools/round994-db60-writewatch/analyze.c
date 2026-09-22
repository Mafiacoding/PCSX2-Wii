/* Round 994 (task #974, follow-up to task #973/Round 993): find WHEN and
 * (via narrowing) WHAT writes the anomalous value 0x000194d0 into
 * MEM[0x0040DB60] (companion-struct offset +8, relative to base 0x0040DB58
 * that Round 991/993 found).
 *
 * Round 993 established that at the Round 990 resting pc (0x0026fe9c),
 * this field ALREADY holds 0x000194d0, even though the registration
 * function's own init code (per Round 991's disassembly of 0x0026fc18)
 * unconditionally zeroes it (`sw zero, 8(v0)`), and the guard flag
 * confirms that init genuinely ran. So the write must happen SOMEWHERE
 * BETWEEN init running and the resting pc being reached - i.e. forward-
 * watching from the resting pc (Round 992's technique) cannot catch it;
 * we have to watch DURING the earlier boot phase instead.
 *
 * Method: run from cold boot in small chunks, sampling
 * MEM[0x0040DB60] after every chunk, and report the first chunk boundary
 * where it changes from 0 to non-zero. Once bracketed, re-run from cold
 * boot again with single-instruction stepping across just that bracket
 * to pinpoint the exact instruction and disassemble it + its caller
 * context.
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

#define WATCH_ADDR 0x0040db60u
#define GUARD_ADDR 0x002ab284u

/* ---- minimal disassembler (same shape as Round 990-993 tools) ---- */
static const char *reg_name(int r) {
    static const char *names[32] = {
        "zero","at","v0","v1","a0","a1","a2","a3",
        "t0","t1","t2","t3","t4","t5","t6","t7",
        "s0","s1","s2","s3","s4","s5","s6","s7",
        "t8","t9","k0","k1","gp","sp","fp","ra"
    };
    return names[r & 31];
}

static void disasm_one(uint32_t pc, uint32_t w, char *out, size_t outsz) {
    uint32_t op = (w >> 26) & 0x3f;
    uint32_t rs = (w >> 21) & 0x1f;
    uint32_t rt = (w >> 16) & 0x1f;
    uint32_t rd = (w >> 11) & 0x1f;
    uint32_t sh = (w >> 6) & 0x1f;
    uint32_t funct = w & 0x3f;
    int16_t imm = (int16_t)(w & 0xffff);
    uint32_t target = (w & 0x03ffffff) << 2;

    if (w == 0) { snprintf(out, outsz, "nop"); return; }
    switch (op) {
    case 0x00: /* SPECIAL */
        switch (funct) {
        case 0x00: snprintf(out, outsz, "sll  %s, %s, %u", reg_name(rd), reg_name(rt), sh); return;
        case 0x02: snprintf(out, outsz, "srl  %s, %s, %u", reg_name(rd), reg_name(rt), sh); return;
        case 0x08: snprintf(out, outsz, "jr   %s", reg_name(rs)); return;
        case 0x09: snprintf(out, outsz, "jalr %s, %s", reg_name(rd), reg_name(rs)); return;
        case 0x20: snprintf(out, outsz, "add  %s, %s, %s", reg_name(rd), reg_name(rs), reg_name(rt)); return;
        case 0x21: snprintf(out, outsz, "addu %s, %s, %s", reg_name(rd), reg_name(rs), reg_name(rt)); return;
        case 0x23: snprintf(out, outsz, "subu %s, %s, %s", reg_name(rd), reg_name(rs), reg_name(rt)); return;
        case 0x24: snprintf(out, outsz, "and  %s, %s, %s", reg_name(rd), reg_name(rs), reg_name(rt)); return;
        case 0x25: snprintf(out, outsz, "or   %s, %s, %s", reg_name(rd), reg_name(rs), reg_name(rt)); return;
        case 0x2d: snprintf(out, outsz, "daddu %s, %s, %s", reg_name(rd), reg_name(rs), reg_name(rt)); return;
        default: snprintf(out, outsz, "special funct=0x%02x", funct); return;
        }
    case 0x02: snprintf(out, outsz, "j    0x%08x", (pc & 0xf0000000u) | target); return;
    case 0x03: snprintf(out, outsz, "jal  0x%08x", (pc & 0xf0000000u) | target); return;
    case 0x04: snprintf(out, outsz, "beq  %s, %s, 0x%08x", reg_name(rs), reg_name(rt), pc + 4 + (imm << 2)); return;
    case 0x05: snprintf(out, outsz, "bne  %s, %s, 0x%08x", reg_name(rs), reg_name(rt), pc + 4 + (imm << 2)); return;
    case 0x06: snprintf(out, outsz, "blez %s, 0x%08x", reg_name(rs), pc + 4 + (imm << 2)); return;
    case 0x07: snprintf(out, outsz, "bgtz %s, 0x%08x", reg_name(rs), pc + 4 + (imm << 2)); return;
    case 0x08: snprintf(out, outsz, "addi %s, %s, %d", reg_name(rt), reg_name(rs), imm); return;
    case 0x09: snprintf(out, outsz, "addiu %s, %s, %d", reg_name(rt), reg_name(rs), imm); return;
    case 0x0a: snprintf(out, outsz, "slti %s, %s, %d", reg_name(rt), reg_name(rs), imm); return;
    case 0x0c: snprintf(out, outsz, "andi %s, %s, 0x%x", reg_name(rt), reg_name(rs), (unsigned)(uint16_t)imm); return;
    case 0x0d: snprintf(out, outsz, "ori  %s, %s, 0x%x", reg_name(rt), reg_name(rs), (unsigned)(uint16_t)imm); return;
    case 0x0f: snprintf(out, outsz, "lui  %s, 0x%04x", reg_name(rt), (unsigned)(uint16_t)imm); return;
    case 0x19: snprintf(out, outsz, "daddiu %s, %s, %d", reg_name(rt), reg_name(rs), imm); return;
    case 0x20: snprintf(out, outsz, "lb   %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x21: snprintf(out, outsz, "lh   %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x23: snprintf(out, outsz, "lw   %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x24: snprintf(out, outsz, "lbu  %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x25: snprintf(out, outsz, "lhu  %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x28: snprintf(out, outsz, "sb   %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x29: snprintf(out, outsz, "sh   %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x2b: snprintf(out, outsz, "sw   %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x37: snprintf(out, outsz, "ld   %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    case 0x3f: snprintf(out, outsz, "sd   %s, %d(%s)", reg_name(rt), imm, reg_name(rs)); return;
    default: snprintf(out, outsz, "op=0x%02x rs=%s rt=%s imm=%d", op, reg_name(rs), reg_name(rt), imm); return;
    }
}

static void dump_range(ee_state_t *ee, uint32_t lo, uint32_t hi) {
    for (uint32_t a = lo; a <= hi; a += 4) {
        uint32_t w = ee_mem_read32(ee, a);
        char buf[128];
        disasm_one(a, w, buf, sizeof(buf));
        printf("  0x%08x: %08x  %s\n", a, w, buf);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <total_budget>\n", argv[0]);
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0); /* match system_safe_printf's raw write(1,...)
                                        * semantics so this tool's printf() output
                                        * interleaves in true chronological order
                                        * with system.c's internal diagnostics -
                                        * mixed buffered/unbuffered writes to the
                                        * same fd were producing a misleadingly
                                        * reordered-looking log. */
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    uint64_t total_budget = strtoull(argv[2], NULL, 10);

    ee_state_t *ee = ee_core_get_state();

    /* IMPORTANT (methodology fix found this round): do NOT call
     * ee_mem_read32() on a KUSEG address immediately after system_init(),
     * before the EE has executed any instructions. ee_mem_read32()'s
     * ee_mem_ptr()/ee_mem_check_tlb_fault() path is a REAL architectural
     * access with genuine TLB-miss-exception side effects (this project
     * models the R5900 MMU for real) - reading an address the BIOS
     * hasn't mapped yet raises a real TLB-refill exception on the EE
     * and permanently redirects it into the exception vector before
     * boot even starts, corrupting the entire rest of the trace. (First
     * attempt this round hit exactly this: ee->pc got stuck at
     * 0xBFC00680/0x690/0x380 - the exception vector - for the whole
     * run, discovered by checking ee_mem_read32()'s source at
     * ee_core.c:1830-1838.) Fix: run a small warm-up slice first so the
     * BIOS's own real early boot code (which does set up TLB entries -
     * see the "TLB spad=0 kernel=1:12 default=13:30 extended=31:38"
     * message it prints for real, confirmed in Round 993's rerun) gets
     * a chance to map RAM before this tool peeks at it. */
    system_run_interleaved(200000ull);

    /* Phase 1: coarse scan from cold boot, sampling every 1M
     * instructions, watching both the guard flag and struct+8. */
    uint64_t chunk = 1000000ull, done = 200000ull;
    uint32_t last_guard = ee_mem_read32(ee, GUARD_ADDR);
    uint32_t last_val = ee_mem_read32(ee, WATCH_ADDR);
    uint64_t guard_set_at = 0, val_changed_at = 0;
    printf("[R994] after warm-up slice: ee_instr=%llu pc=0x%08x guard=0x%08x val=0x%08x\n",
           (unsigned long long)ee->instructions_executed, ee->pc, last_guard, last_val);
    while (done < total_budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        uint32_t g = ee_mem_read32(ee, GUARD_ADDR);
        uint32_t v = ee_mem_read32(ee, WATCH_ADDR);
        if (g != last_guard && guard_set_at == 0) {
            guard_set_at = ee->instructions_executed;
            printf("[R994] guard flag changed 0x%08x -> 0x%08x at ee_instr=%llu (bracket: [%llu, %llu])\n",
                   last_guard, g, (unsigned long long)guard_set_at,
                   (unsigned long long)(guard_set_at - chunk), (unsigned long long)guard_set_at);
        }
        if (v != last_val && val_changed_at == 0) {
            val_changed_at = ee->instructions_executed;
            printf("[R994] MEM[0x%08x] changed 0x%08x -> 0x%08x at ee_instr=%llu (bracket: [%llu, %llu])\n",
                   WATCH_ADDR, last_val, v, (unsigned long long)val_changed_at,
                   (unsigned long long)(val_changed_at - chunk), (unsigned long long)val_changed_at);
        }
        last_guard = g;
        last_val = v;
        fflush(stdout);
        if (guard_set_at && val_changed_at) break;
        if (ee->pc == 0x0026fe9cu) {
            printf("[R994] reached Round 990 resting pc at ee_instr=%llu, guard=0x%08x val=0x%08x - stopping coarse scan\n",
                   (unsigned long long)ee->instructions_executed, g, v);
            break;
        }
    }
    printf("\n[R994] Coarse result: guard_set_at=%llu val_changed_at=%llu final ee_instr=%llu pc=0x%08x\n",
           (unsigned long long)guard_set_at, (unsigned long long)val_changed_at,
           (unsigned long long)ee->instructions_executed, ee->pc);

    if (val_changed_at == 0) {
        printf("[R994] value never changed within budget - re-check total_budget or WATCH_ADDR.\n");
        return 0;
    }

    /* Phase 2: re-run from cold boot, single-stepping across the
     * bracket [val_changed_at - chunk, val_changed_at] to pinpoint the
     * exact instruction. */
    bios_image_t bios2;
    if (bios_load(argv[1], &bios2) != 0) { fprintf(stderr, "bios reload fail\n"); return 1; }
    if (system_init(&bios2, &bios2) != 0) { fprintf(stderr, "system_init reinit fail\n"); return 1; }
    ee_state_t *ee2 = ee_core_get_state();
    uint64_t bracket_lo = (val_changed_at > chunk) ? (val_changed_at - chunk) : 0;
    if (bracket_lo > 0) system_run_interleaved(bracket_lo);
    printf("[R994] Phase 2: fast-forwarded to ee_instr=%llu, now single-stepping to find exact write...\n",
           (unsigned long long)ee2->instructions_executed);
    uint32_t prev_val = ee_mem_read32(ee2, WATCH_ADDR);
    uint64_t step_budget = (val_changed_at - bracket_lo) + 1000;
    int found = 0;
    for (uint64_t i = 0; i < step_budget && !ee2->halted; i++) {
        uint32_t prev_pc = ee2->pc;
        system_run_interleaved(1);
        uint32_t v = ee_mem_read32(ee2, WATCH_ADDR);
        if (v != prev_val) {
            printf("\n[R994] EXACT WRITE FOUND at ee_instr=%llu, instruction pc=0x%08x: 0x%08x -> 0x%08x\n",
                   (unsigned long long)ee2->instructions_executed, prev_pc, prev_val, v);
            printf("[R994] Register state at write: pc=0x%08x ra=0x%08llx sp=0x%08llx gp=0x%08llx\n",
                   prev_pc, (unsigned long long)ee2->gpr[31].ud0, (unsigned long long)ee2->gpr[29].ud0,
                   (unsigned long long)ee2->gpr[28].ud0);
            printf("[R994] Disassembly context around write site:\n");
            dump_range(ee2, (prev_pc >= 32) ? prev_pc - 32 : 0, prev_pc + 32);
            found = 1;
            break;
        }
        prev_val = v;
    }
    if (!found) {
        printf("[R994] single-step pass did not catch the write within the bracket - "
               "bracket or chunk-boundary sampling may be inexact (e.g. IOP-side or DMA write "
               "not advancing ee->instructions_executed the same way). Needs a different bracket "
               "or method next round.\n");
    }
    return 0;
}
