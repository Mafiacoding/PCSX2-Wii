/* Round 955 (task #937/#947 continuation, SCPH-50004): this round's
 * R933_RPCCALL_TRACE + new R955_LOADFILE_NAME_TRACE instrumentation
 * (ee_core.c, gated #ifdef, zero-cost when unset) already showed that
 * across a 60,000,000-slice SCPH-50004 diskless boot, EELOAD issues
 * the exact same LF_F_ELF_LOAD RPC_CALL request 13 times - always
 * devname="rom0" romname="OSDSYS", always SUCCEEDING
 * (r554_ok=1, epc=0x00100008, gp=0x00000000 - and gp=0 is real, cited
 * ps2sdk elf_load_all_section() behavior, not a bug, see the comment
 * at sif_loadfile_elf_load()).
 *
 * Since every one of the 13 attempts reports success and the reply
 * delivery path (ee_mem_write32 into recvbuf + ee_arm_rpc_call_pending)
 * is the same, already-battle-tested mechanism used by every other RPC
 * service in this project, the open question is purely empirical: does
 * the EE ever actually FETCH/EXECUTE any instruction inside the loaded
 * OSDSYS text range (0x00100000-0x00120000, the standard PS2 user-ELF
 * load address) after any of these 13 "successful" loads - or does the
 * calling thread never actually jump to the returned epc at all (which
 * would mean the retries are caused by a genuine un-consumed-completion
 * bug, not a real BIOS retry/relaunch behavior)?
 *
 * This tool samples ee->pc on every 5,000-slice tick (fine-grained,
 * since OSDSYS's own entry code executes for only a short window if it
 * runs at all) and records: (a) the first ee_instr at which pc ever
 * lands inside [0x00100000, 0x00120000), (b) a running visit count,
 * (c) the pc value + ee_instr at each of up to 32 first sightings, so
 * we can correlate visit timing against the 13 known LOADFILE-reply
 * timestamps directly from stderr's R933EVT/R955EVT lines (same run).
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
#include "core/hw/iop_cdvd.h"

#define OSDSYS_LO 0x00100000u
#define OSDSYS_HI 0x00120000u

static const char *scmd_name(uint8_t c)
{
    switch (c) {
    case SCMD_OPENCONFIG:  return "OPENCONFIG";
    case SCMD_READCONFIG:  return "READCONFIG";
    case SCMD_WRITECONFIG: return "WRITECONFIG";
    case SCMD_CLOSECONFIG: return "CLOSECONFIG";
    default: return "other";
    }
}

/* Round 955 follow-up: minimal disassembler (same table shape as
 * Rounds 953/954's own analyze.c tools) to decode the tight
 * 0x00100b60-0x00100ce0 loop the R955PC sampler caught OSDSYS's own
 * loaded code dwelling in for ~2,000,000 instructions before control
 * returns to BIOS kernel code and the next LOADFILE retry fires. */
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
    case 0x00: return "sll"; case 0x02: return "srl"; case 0x03: return "sra";
    case 0x04: return "sllv"; case 0x06: return "srlv"; case 0x07: return "srav";
    case 0x08: return "jr"; case 0x09: return "jalr"; case 0x0A: return "movz";
    case 0x0B: return "movn"; case 0x0C: return "syscall"; case 0x0D: return "break";
    case 0x0F: return "sync"; case 0x10: return "mfhi"; case 0x11: return "mthi";
    case 0x12: return "mflo"; case 0x13: return "mtlo"; case 0x14: return "dsllv";
    case 0x16: return "dsrlv"; case 0x17: return "dsrav"; case 0x18: return "mult";
    case 0x19: return "multu"; case 0x1A: return "div"; case 0x1B: return "divu";
    case 0x20: return "add"; case 0x21: return "addu"; case 0x22: return "sub";
    case 0x23: return "subu"; case 0x24: return "and"; case 0x25: return "or";
    case 0x26: return "xor"; case 0x27: return "nor"; case 0x2A: return "slt";
    case 0x2B: return "sltu"; case 0x2C: return "dadd"; case 0x2D: return "daddu";
    case 0x2E: return "dsub"; case 0x2F: return "dsubu"; case 0x38: return "dsll";
    case 0x3A: return "dsrl"; case 0x3B: return "dsra"; case 0x3C: return "dsll32";
    case 0x3E: return "dsrl32"; case 0x3F: return "dsra32";
    default: return NULL;
    }
}
static const char *regimm_mn(uint32_t rt)
{
    switch (rt) {
    case 0x00: return "bltz"; case 0x01: return "bgez";
    case 0x02: return "bltzl"; case 0x03: return "bgezl";
    case 0x10: return "bltzal"; case 0x11: return "bgezal";
    default: return NULL;
    }
}
static const char *opcode_mn(uint32_t op)
{
    switch (op) {
    case 0x02: return "j"; case 0x03: return "jal"; case 0x04: return "beq";
    case 0x05: return "bne"; case 0x06: return "blez"; case 0x07: return "bgtz";
    case 0x08: return "addi"; case 0x09: return "addiu"; case 0x0A: return "slti";
    case 0x0B: return "sltiu"; case 0x0C: return "andi"; case 0x0D: return "ori";
    case 0x0E: return "xori"; case 0x0F: return "lui"; case 0x10: return "cop0";
    case 0x11: return "cop1"; case 0x12: return "cop2"; case 0x14: return "beql";
    case 0x15: return "bnel"; case 0x16: return "blezl"; case 0x17: return "bgtzl";
    case 0x1A: return "ldl"; case 0x1B: return "ldr"; case 0x1E: return "lq";
    case 0x1F: return "sq"; case 0x20: return "lb"; case 0x21: return "lh";
    case 0x22: return "lwl"; case 0x23: return "lw"; case 0x24: return "lbu";
    case 0x25: return "lhu"; case 0x26: return "lwr"; case 0x27: return "lwu";
    case 0x28: return "sb"; case 0x29: return "sh"; case 0x2A: return "swl";
    case 0x2B: return "sw"; case 0x2E: return "swr"; case 0x2F: return "cache";
    case 0x31: return "lwc1"; case 0x36: return "lqc2"; case 0x37: return "ld";
    case 0x39: return "swc1"; case 0x3E: return "sqc2"; case 0x3F: return "sd";
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
            snprintf(out, outsz, "%-5s%s, %s, %u", mn, reg_name(rd), reg_name(rt), sh); return;
        }
        snprintf(out, outsz, "%-5s%s, %s, %s", mn, reg_name(rd), reg_name(rs), reg_name(rt)); return;
    }
    if (op == 0x01) {
        const char *mn = regimm_mn(rt);
        if (!mn) { snprintf(out, outsz, ".word 0x%08x (regimm rt=0x%02x)", w, rt); return; }
        snprintf(out, outsz, "%-5s%s, 0x%08x", mn, reg_name(rs), a + 4 + ((int32_t)imm << 2)); return;
    }
    if (op == 0x02 || op == 0x03) {
        snprintf(out, outsz, "%-5s0x%08x", opcode_mn(op), (a & 0xF0000000u) | target); return;
    }
    if (op == 0x04 || op == 0x05 || op == 0x14 || op == 0x15) {
        snprintf(out, outsz, "%-5s%s, %s, 0x%08x", opcode_mn(op), reg_name(rs), reg_name(rt),
                 a + 4 + ((int32_t)imm << 2)); return;
    }
    if (op == 0x06 || op == 0x07 || op == 0x16 || op == 0x17) {
        snprintf(out, outsz, "%-5s%s, 0x%08x", opcode_mn(op), reg_name(rs), a + 4 + ((int32_t)imm << 2)); return;
    }
    if (op == 0x0F) { snprintf(out, outsz, "lui  %s, 0x%04x", reg_name(rt), (uint16_t)imm); return; }
    if (op == 0x10 || op == 0x11 || op == 0x12) {
        snprintf(out, outsz, ".word 0x%08x (cop%d, rs=0x%02x rt=%s)", w, op - 0x10, rs, reg_name(rt)); return;
    }
    const char *mn = opcode_mn(op);
    if (!mn) { snprintf(out, outsz, ".word 0x%08x (op=0x%02x)", w, op); return; }
    if (op >= 0x20 && op <= 0x3F && op != 0x2F) {
        snprintf(out, outsz, "%-5s%s, %d(%s)", mn, reg_name(rt), imm, reg_name(rs)); return;
    }
    snprintf(out, outsz, "%-5s%s, %s, %d", mn, reg_name(rt), reg_name(rs), imm);
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
    uint64_t chunk = 5000ull, done = 0; /* fine-grained: 5,000-slice ticks */
    ee_state_t *ee = ee_core_get_state();

    uint64_t osdsys_visits = 0;
    uint64_t osdsys_first_instr = 0;
    int osdsys_first_seen = 0;
    int printed = 0;
    uint32_t last_pc_bucket_seen = 0xFFFFFFFFu; /* avoid spamming on a long dwell in-range */

    uint64_t last_scmd_count = 0;
    int scmd_printed = 0;
    int dumped_live = 0;

    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        uint32_t pc = ee->pc;
        if (pc >= OSDSYS_LO && pc < OSDSYS_HI) {
            osdsys_visits++;
            if (!osdsys_first_seen) {
                osdsys_first_seen = 1;
                osdsys_first_instr = ee->instructions_executed;
            }
            if (!dumped_live) {
                /* Round 955 follow-up: dump the code LIVE, right when first
                 * caught, before any later reload/BSS-clear can zero it out
                 * (a static end-of-run dump of this same range later showed
                 * all-zero - this live dump settles whether that's because
                 * the range is genuinely BSS/NOP-sled, or was overwritten
                 * afterward by a subsequent phase). */
                dumped_live = 1;
                printf("\n[R955LIVEDISASM] -- LIVE dump of 0x00100b00-0x00100d00 at first OSDSYS-range "
                       "sighting, ee_instr=%llu pc=0x%08x --\n",
                       (unsigned long long)ee->instructions_executed, pc);
                uint32_t a2 = 0x00100b00u;
                for (; a2 < 0x00100d00u; a2 += 4) {
                    uint32_t w2 = ee_mem_read32(ee, a2);
                    char buf2[128];
                    disasm_one(w2, a2, buf2, sizeof(buf2));
                    printf("  0x%08x: %08x  %-28s%s\n", a2, w2, buf2, (a2 == (pc & ~3u)) ? "   <-- PC" : "");
                }
            }
            if (printed < 32 && (pc < last_pc_bucket_seen || pc > last_pc_bucket_seen + 0x40u)) {
                printf("[R955PC] ee_instr=%llu pc=0x%08x (inside OSDSYS text range)\n",
                       (unsigned long long)ee->instructions_executed, pc);
                printed++;
                last_pc_bucket_seen = pc;
            }
        }
        uint64_t sc = iop_cdvd_get_scmd_call_count();
        if (sc != last_scmd_count) {
            if (scmd_printed < 64) {
                printf("[R955SCMD] ee_instr=%llu scmd_call_count %llu -> %llu last_scmd=0x%02x (%s)\n",
                       (unsigned long long)ee->instructions_executed,
                       (unsigned long long)last_scmd_count, (unsigned long long)sc,
                       iop_cdvd_get_last_scmd_issued(), scmd_name(iop_cdvd_get_last_scmd_issued()));
                scmd_printed++;
            }
            last_scmd_count = sc;
        }
    }

    printf("\n[R955PC] FINAL: ee_instr=%llu ee_pc=0x%08x ee_halted=%d\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);
    printf("[R955PC] FINAL: osdsys_text_range_visits(sampled every 5000 slices)=%llu "
           "first_seen_at_ee_instr=%llu first_seen=%d\n",
           (unsigned long long)osdsys_visits, (unsigned long long)osdsys_first_instr, osdsys_first_seen);
    printf("[R955PC] verdict: %s\n",
           osdsys_first_seen
               ? "PC WAS observed inside OSDSYS's real text range (0x00100000-0x00120000) at least "
                 "once - the LOADFILE-returned epc IS being reached/executed by real code at least "
                 "sometimes, so the 13x retry is NOT simply 'reply never consumed'."
               : "PC was NEVER observed inside OSDSYS's text range across the entire sampled budget, "
                 "despite 13 'successful' LF_F_ELF_LOAD replies reporting epc=0x00100008 - strong "
                 "evidence the calling thread never actually jumps to the returned epc, i.e. the RPC "
                 "completion this project delivers is not being consumed/acted upon by the real "
                 "caller code, which is why it keeps re-requesting the same load.");

    if (osdsys_first_seen) {
        printf("\n[R955DISASM] -- OSDSYS text-range dwell loop 0x00100b60-0x00100cf0 (RAM content still "
               "resident at end of run, address is fixed/reloaded to same spot each cycle) --\n");
        uint32_t a = 0x00100b60u;
        for (; a < 0x00100cf0u; a += 4) {
            uint32_t w = ee_mem_read32(ee, a);
            char buf[128];
            disasm_one(w, a, buf, sizeof(buf));
            printf("  0x%08x: %08x  %s\n", a, w, buf);
        }
    }

    return 0;
}
