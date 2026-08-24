/*
 * Round 653 (task #640): standalone VU1 micro-program disassembler.
 *
 * Reads a raw 16KB dump of VU1's micro[] instruction memory (as
 * produced by r654_dump_vu1_micro()-style instrumentation, or any
 * other 16KB little-endian dump of core/hw/vu.h's VU1_MICRO_SIZE
 * region) and prints readable mnemonics for both the lower (integer
 * ALU/branch/load-store/misc) and upper (FMAC arithmetic) halves of
 * each 64-bit instruction pair, using this project's own
 * manual-sourced field/opcode tables in source/hw/vu_opcodes.h -
 * this tool does not invent any new opcode semantics, it only prints
 * what vu.c's own decode logic (vu_exec_lower/vu_exec_upper) would
 * do, in a form a human can read across a wide address range at
 * once (vu.c's own interpreter never printed anything - this was
 * needed to see the *structure* of a micro-program, not just single
 * instruction traces).
 *
 * This is an additive, decoupled tool (like tools/round588's forced-
 * render driver) - it is excluded from SOURCES in the top-level
 * Makefile and does not touch anything under source/ or include/.
 *
 * Usage: round653_vu1_disasm <dump.bin> [start_offset_hex] [count]
 *   start_offset_hex defaults to 0, count (instruction PAIRS) defaults
 *   to 64 (512 bytes).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define VU_U_DEST(w)   (((w) >> 21) & 0xFu)
#define VU_U_FT(w)     (((w) >> 16) & 0x1Fu)
#define VU_U_FS(w)     (((w) >> 11) & 0x1Fu)
#define VU_U_FD(w)     (((w) >> 6)  & 0x1Fu)
#define VU_U_FUNCT6(w) ((w) & 0x3Fu)
#define VU_U_SUB4(w)   (((w) >> 2)  & 0xFu)
#define VU_U_BC(w)     ((w) & 0x3u)
#define VU_U_I(w)      (((w) >> 31) & 0x1u)
#define VU_U_E(w)      (((w) >> 30) & 0x1u)

#define VU_L_OPCODE(w) (((w) >> 25) & 0x7Fu)
#define VU_L_DEST(w)   (((w) >> 21) & 0xFu)
#define VU_L_RT(w)     (((w) >> 16) & 0x1Fu)
#define VU_L_RS(w)     (((w) >> 11) & 0x1Fu)
#define VU_L_RD(w)     (((w) >> 6)  & 0x1Fu)
#define VU_L_FUNCT6(w) ((w) & 0x3Fu)
#define VU_L_IMM11(w)  ((w) & 0x7FFu)
#define VU_L_SPECIAL_OPCODE 0x40u

enum {
    VUL_LQ = 0x00, VUL_SQ = 0x01, VUL_ILW = 0x04, VUL_ISW = 0x05,
    VUL_IADDIU = 0x08, VUL_ISUBIU = 0x09,
    VUL_FCEQ = 0x10, VUL_FCSET = 0x11, VUL_FCAND = 0x12, VUL_FCOR = 0x13,
    VUL_FSEQ = 0x14, VUL_FSSET = 0x15, VUL_FSAND = 0x16, VUL_FSOR = 0x17,
    VUL_FMEQ = 0x18, VUL_FMAND = 0x1A, VUL_FMOR = 0x1B, VUL_FCGET = 0x1C,
    VUL_B = 0x20, VUL_BAL = 0x21, VUL_JR = 0x24, VUL_JALR = 0x25,
    VUL_IBEQ = 0x28, VUL_IBNE = 0x29, VUL_IBLTZ = 0x2C,
    VUL_IBGTZ = 0x2D, VUL_IBLEZ = 0x2E, VUL_IBGEZ = 0x2F
};

enum { VULS_IADD = 0x30, VULS_ISUB = 0x31, VULS_IADDI = 0x32, VULS_IOR = 0x33, VULS_IAND = 0x34 };

enum {
    VULS_FD_MOVE_GROUP = 0x0C, VULS_FD_LSQ_GROUP = 0x0D,
    VULS_FD_DIVQ_GROUP = 0x0E, VULS_FD_MTIR_GROUP = 0x0F,
    VULS_FD_R_GROUP = 0x10, VULS_FD_XTOP_GROUP = 0x1A,
    VULS_FD_XGKICK_GROUP = 0x1B, VULS_FD_WAITP = 0x1E
};

static const char *bc_name(uint32_t bc) {
    switch (bc) { case 0: return "x"; case 1: return "y"; case 2: return "z"; default: return "w"; }
}
static void dest_str(char *out, uint32_t dest) {
    out[0] = (dest & 8) ? 'x' : '-';
    out[1] = (dest & 4) ? 'y' : '-';
    out[2] = (dest & 2) ? 'z' : '-';
    out[3] = (dest & 1) ? 'w' : '-';
    out[4] = 0;
}

static void disasm_lower(uint32_t w, uint32_t pc, char *out, size_t outsz) {
    uint32_t opcode = VU_L_OPCODE(w);
    uint32_t rt = VU_L_RT(w), rs = VU_L_RS(w) & 0xFu, rd = VU_L_RD(w);
    uint32_t dest = VU_L_DEST(w);
    int32_t imm11 = (int32_t)(VU_L_IMM11(w) << 21) >> 21;
    char dstr[5];
    dest_str(dstr, dest);

    if (opcode != VU_L_SPECIAL_OPCODE) {
        switch (opcode) {
            case VUL_LQ:  snprintf(out, outsz, "LQ.%s   vf%02u, %d(vi%02u)", dstr, rt, imm11, rs); return;
            case VUL_SQ:  snprintf(out, outsz, "SQ.%s   vf%02u, %d(vi%02u)", dstr, rs, imm11, rt); return;
            case VUL_ILW: snprintf(out, outsz, "ILW.%s  vi%02u, %d(vi%02u)", dstr, rt, imm11, rs); return;
            case VUL_ISW: snprintf(out, outsz, "ISW.%s  vi%02u, %d(vi%02u)", dstr, rt, imm11, rs); return;
            case VUL_IADDIU: snprintf(out, outsz, "IADDIU vi%02u, vi%02u, 0x%x", rt, rs, VU_L_IMM11(w) | ((rd & 0x3u) << 11)); return;
            case VUL_ISUBIU: snprintf(out, outsz, "ISUBIU vi%02u, vi%02u, 0x%x", rt, rs, VU_L_IMM11(w) | ((rd & 0x3u) << 11)); return;
            case VUL_FCEQ:  snprintf(out, outsz, "FCEQ   vi%02u, 0x%x", rt, VU_L_IMM11(w) & 0xFFFu); return;
            case VUL_FCSET: snprintf(out, outsz, "FCSET  0x%x", VU_L_IMM11(w) & 0xFFFu); return;
            case VUL_FCAND: snprintf(out, outsz, "FCAND  vi%02u, 0x%x", rt, VU_L_IMM11(w) & 0xFFFu); return;
            case VUL_FCOR:  snprintf(out, outsz, "FCOR   vi%02u, 0x%x", rt, VU_L_IMM11(w) & 0xFFFu); return;
            case VUL_FSEQ: case VUL_FSSET: case VUL_FSAND: case VUL_FSOR:
                snprintf(out, outsz, "FS*    vi%02u", rt); return;
            case VUL_B:   snprintf(out, outsz, "B      0x%04x", (uint32_t)((int32_t)pc + 8 + imm11 * 8)); return;
            case VUL_BAL: snprintf(out, outsz, "BAL    vi%02u, 0x%04x", rt, (uint32_t)((int32_t)pc + 8 + imm11 * 8)); return;
            case VUL_JR:   snprintf(out, outsz, "JR     vi%02u", rs); return;
            case VUL_JALR: snprintf(out, outsz, "JALR   vi%02u, vi%02u", rt, rs); return;
            case VUL_IBEQ:  snprintf(out, outsz, "IBEQ   vi%02u, vi%02u, 0x%04x", rs, rt, (uint32_t)((int32_t)pc + 8 + imm11 * 8)); return;
            case VUL_IBNE:  snprintf(out, outsz, "IBNE   vi%02u, vi%02u, 0x%04x", rs, rt, (uint32_t)((int32_t)pc + 8 + imm11 * 8)); return;
            case VUL_IBLTZ: snprintf(out, outsz, "IBLTZ  vi%02u, 0x%04x", rs, (uint32_t)((int32_t)pc + 8 + imm11 * 8)); return;
            case VUL_IBGTZ: snprintf(out, outsz, "IBGTZ  vi%02u, 0x%04x", rs, (uint32_t)((int32_t)pc + 8 + imm11 * 8)); return;
            case VUL_IBLEZ: snprintf(out, outsz, "IBLEZ  vi%02u, 0x%04x", rs, (uint32_t)((int32_t)pc + 8 + imm11 * 8)); return;
            case VUL_IBGEZ: snprintf(out, outsz, "IBGEZ  vi%02u, 0x%04x", rs, (uint32_t)((int32_t)pc + 8 + imm11 * 8)); return;
            default: snprintf(out, outsz, "L.unk  opcode=0x%02x", opcode); return;
        }
    }

    uint32_t funct6 = VU_L_FUNCT6(w);
    switch (funct6) {
        case VULS_IADD:  snprintf(out, outsz, "IADD   vi%02u, vi%02u, vi%02u", rd & 0xFu, rs, rt & 0xFu); return;
        case VULS_ISUB:  snprintf(out, outsz, "ISUB   vi%02u, vi%02u, vi%02u", rd & 0xFu, rs, rt & 0xFu); return;
        case VULS_IADDI: snprintf(out, outsz, "IADDI  vi%02u, vi%02u, %d", rt & 0xFu, rs, (int)(((int32_t)(rd << 27)) >> 27)); return;
        case VULS_IOR:   snprintf(out, outsz, "IOR    vi%02u, vi%02u, vi%02u", rd & 0xFu, rs, rt & 0xFu); return;
        case VULS_IAND:  snprintf(out, outsz, "IAND   vi%02u, vi%02u, vi%02u", rd & 0xFu, rs, rt & 0xFu); return;
        default: break;
    }
    uint32_t fdslot = VU_L_RD(w);
    uint32_t bc2 = w & 0x3u;
    if (fdslot == VULS_FD_MOVE_GROUP) {
        if (bc2 == 0) { snprintf(out, outsz, "MOVE.%s vf%02u, vf%02u", dstr, rt, rs); return; }
        if (bc2 == 1) { snprintf(out, outsz, "MR32.%s vf%02u, vf%02u", dstr, rt, rs); return; }
    }
    if (fdslot == VULS_FD_LSQ_GROUP) {
        const char *nm = bc2 == 0 ? "LQI" : bc2 == 1 ? "SQI" : bc2 == 2 ? "LQD" : "SQD";
        if (bc2 == 0 || bc2 == 2)
            snprintf(out, outsz, "%s.%s  vf%02u, (vi%02u%s)", nm, dstr, rt, rs, bc2 == 0 ? "++" : "--");
        else
            snprintf(out, outsz, "%s.%s  vf%02u, (vi%02u%s)  [vi%02u aliased from raw 0x%x]", nm, dstr, rt, rs, bc2 == 1 ? "++" : "--", rs, VU_L_RS(w));
        return;
    }
    if (fdslot == VULS_FD_DIVQ_GROUP) {
        const char *nm = bc2 == 0 ? "DIV" : bc2 == 1 ? "SQRT" : bc2 == 2 ? "RSQRT" : "WAITQ";
        snprintf(out, outsz, "%s    Q, vf%02u.%s, vf%02u.%s", nm, rs, bc_name((w>>21)&3), rt, bc_name(bc2)); return;
    }
    if (fdslot == VULS_FD_MTIR_GROUP) {
        const char *nm = bc2 == 0 ? "MTIR" : bc2 == 1 ? "MFIR" : bc2 == 2 ? "ILWR" : "ISWR";
        snprintf(out, outsz, "%s   vi%02u, vf%02u", nm, rt & 0xFu, rs); return;
    }
    if (fdslot == VULS_FD_R_GROUP) {
        const char *nm = bc2 == 0 ? "RNEXT" : bc2 == 1 ? "RGET" : bc2 == 2 ? "RINIT" : "RXOR";
        snprintf(out, outsz, "%s.%s vf%02u", nm, dstr, rt); return;
    }
    if (fdslot == VULS_FD_XTOP_GROUP) {
        snprintf(out, outsz, "%s   vi%02u", bc2 == 0 ? "XTOP" : "XITOP", rt & 0xFu); return;
    }
    if (fdslot == VULS_FD_XGKICK_GROUP) {
        snprintf(out, outsz, "XGKICK vi%02u", rs); return;
    }
    if (fdslot == VULS_FD_WAITP) {
        snprintf(out, outsz, "WAITP"); return;
    }
    snprintf(out, outsz, "L.SPECIAL fdslot=0x%02x bc=%u", fdslot, bc2);
}

static void disasm_upper(uint32_t w, char *out, size_t outsz) {
    uint32_t i = VU_U_I(w), e = VU_U_E(w);
    uint32_t ft = VU_U_FT(w), fs = VU_U_FS(w), fd = VU_U_FD(w);
    uint32_t dest = VU_U_DEST(w);
    char dstr[5]; dest_str(dstr, dest);
    char flags[4] = {0};
    int fi = 0;
    if (i) flags[fi++] = 'I';
    if (e) flags[fi++] = 'E';
    flags[fi] = 0;

    uint32_t sub4 = VU_U_SUB4(w);
    uint32_t bc = VU_U_BC(w);
    if (sub4 == 0xFu) {
        snprintf(out, outsz, "U.SPECIAL%s sub=0x%02x bc=%u fs=vf%02u ft=vf%02u", flags, fd, bc, fs, ft);
        return;
    }
    uint32_t funct6 = VU_U_FUNCT6(w);
    const char *mnem = NULL;
    switch (funct6) {
        case 0x28: mnem = "ADD"; break;  case 0x2C: mnem = "SUB"; break;
        case 0x2A: mnem = "MUL"; break;  case 0x29: mnem = "MADD"; break;
        case 0x2D: mnem = "MSUB"; break; case 0x2B: mnem = "MAX"; break;
        case 0x2F: mnem = "MINI"; break; case 0x20: mnem = "ADDQ"; break;
        case 0x24: mnem = "SUBQ"; break; case 0x1C: mnem = "MULQ"; break;
        case 0x21: mnem = "MADDQ"; break; case 0x25: mnem = "MSUBQ"; break;
        case 0x22: mnem = "ADDI"; break; case 0x26: mnem = "SUBI"; break;
        case 0x1E: mnem = "MULI"; break; case 0x23: mnem = "MADDI"; break;
        case 0x27: mnem = "MSUBI"; break; case 0x1D: mnem = "MAXI"; break;
        case 0x1F: mnem = "MINII"; break;
        default: break;
    }
    if (mnem) { snprintf(out, outsz, "%s.%s%s vf%02u, vf%02u, vf%02u", mnem, dstr, flags, fd, fs, ft); return; }
    const char *bmnem = NULL;
    switch (sub4) {
        case 0x0: bmnem = "ADDbc"; break; case 0x1: bmnem = "SUBbc"; break;
        case 0x2: bmnem = "MADDbc"; break; case 0x3: bmnem = "MSUBbc"; break;
        case 0x4: bmnem = "MAXbc"; break; case 0x5: bmnem = "MINIbc"; break;
        case 0x6: bmnem = "MULbc"; break;
        default: break;
    }
    if (bmnem) { snprintf(out, outsz, "%s.%s%s.%s vf%02u, vf%02u, vf%02u", bmnem, dstr, flags, bc_name(bc), fd, fs, ft); return; }
    snprintf(out, outsz, "U.unk%s sub4=0x%x bc=%u fs=vf%02u ft=vf%02u fd=vf%02u", flags, sub4, bc, fs, ft, fd);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <vu1_micro_dump.bin> [start_offset_hex] [count]\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    static uint8_t buf[65536];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    fprintf(stderr, "[disasm] read %zu bytes from %s\n", n, argv[1]);

    uint32_t start = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 16) : 0;
    uint32_t count = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 10) : 64;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = start + i * 8;
        if (off + 8 > n) break;
        uint32_t lower, upper;
        memcpy(&lower, buf + off, 4);
        memcpy(&upper, buf + off + 4, 4);
        char lbuf[128], ubuf[128];
        disasm_lower(lower, off, lbuf, sizeof(lbuf));
        disasm_upper(upper, ubuf, sizeof(ubuf));
        printf("0x%04x: lo=%08x hi=%08x | %-40s | %s\n", off, lower, upper, lbuf, ubuf);
        if (VU_U_E(upper)) printf("        ^-- E flag set (end of microprogram, executes one more instruction pair)\n");
    }
    return 0;
}
