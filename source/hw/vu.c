/*
 * vu.c - see include/core/hw/vu.h for scope notes and references, and
 * source/hw/vu_opcodes.h for the real VU instruction encoding this
 * round (task #94) added.
 */

#include "core/hw/vu.h"
#include "vu_opcodes.h"
#include <string.h>
#include <math.h>

static vu1_state_t g_vu1;

void vu1_init(void)
{
    memset(&g_vu1, 0, sizeof(g_vu1));
    /* VF00 is hardwired to (0,0,0,1.0f) on real hardware, matching
     * this project's existing VU0 macro-mode convention (ee_core.c's
     * vu0_vf_read_lane) - see docs/STATUS.md's "round 13" section for
     * the same fact cited there. Stored directly here (rather than
     * via a read-time special case like VU0's helpers) since VU1
     * micro mode has no other read path yet to route through. */
    g_vu1.vf[0][3] = 0x3F800000u; /* float bit pattern of 1.0f */
}

vu1_state_t *vu1_get_state(void) { return &g_vu1; }

static inline uint32_t vu_rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void vu_wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void vu1_micro_write32(uint32_t addr, uint32_t value)
{
    uint32_t off = addr & (VU1_MICRO_SIZE - 1u);
    vu_wr_le32(g_vu1.micro + off, value);
}

/* VU1 local DATA memory (g_vu1.mem, distinct from micro[] above) -
 * called from vif.c's VIF1 UNPACK handling (task: "VIF UNPACK"). */
void vu1_mem_write32(uint32_t addr, uint32_t value)
{
    uint32_t off = addr & (VU1_MEM_SIZE - 1u);
    vu_wr_le32(g_vu1.mem + off, value);
}

/* raw-bits <-> float helpers (VF lanes are stored as raw bit
 * patterns throughout this project, matching ee_core.c's own
 * convention) */
static inline float vu_f(uint32_t bits) { union { uint32_t u; float f; } c; c.u = bits; return c.f; }
static inline uint32_t vu_u(float f)    { union { uint32_t u; float f; } c; c.f = f; return c.u; }

/* Writes result[lane] into vf[fd_idx][lane] for every lane selected
 * by dest_mask (bit3=x..bit0=w, see vu_opcodes.h). Register 0 (VF00)
 * is hardwired on real hardware - writes to it are always discarded,
 * matching the existing vu1_init()/ee_core.c convention. */
static void vu_write_dest(uint32_t vf[32][4], uint32_t fd_idx, uint32_t dest_mask, const float result[4])
{
    if (fd_idx == 0) return;
    for (int lane = 0; lane < 4; lane++) {
        int bit = (dest_mask >> (3 - lane)) & 1;
        if (bit) vf[fd_idx][lane] = vu_u(result[lane]);
    }
}

/* 16-bit VU integer register convention (real hardware: VI0-VI15 are
 * 16-bit, VI0 hardwired to 0 - see vu_opcodes.h). Registers >=16 in
 * this project's vi[32] array are the special/reserved slots
 * (REG_STATUS_FLAG etc, REG_I=21, REG_Q=22 - see vu.h) which are NOT
 * masked/zero-hardwired by this helper - only touch it for real
 * general-purpose integer-register writes (indices 1-15). */
static inline void vu_write_vi(uint32_t *vi, uint32_t idx, uint32_t value)
{
    if (idx == 0 || idx > 15) return;
    vi[idx] = value & 0xFFFFu;
}
static inline uint32_t vu_read_vi16(const uint32_t *vi, uint32_t idx)
{
    if (idx == 0) return 0;
    return vi[idx] & 0xFFFFu;
}
static inline int16_t vu_read_vi16_signed(const uint32_t *vi, uint32_t idx)
{
    return (int16_t)vu_read_vi16(vi, idx);
}

static inline int32_t vu_sext(uint32_t value, int bits)
{
    uint32_t signbit = 1u << (bits - 1);
    return (int32_t)((value ^ signbit) - signbit);
}

/* ============================================================
 * UPPER WORD (FMAC arithmetic) - see vu_opcodes.h for the full
 * citation trail and confidence notes for every case below.
 * Returns 1 if a real instruction was decoded and executed, 0 if not
 * (caller counts the pair as unimplemented in that case).
 * ============================================================ */
static int vu_exec_upper(uint32_t vf[32][4], uint32_t *vi, uint32_t acc[4], uint32_t w)
{
    uint32_t dest = VU_U_DEST(w);
    uint32_t ft   = VU_U_FT(w);
    uint32_t fs   = VU_U_FS(w);
    uint32_t fd   = VU_U_FD(w);
    float Q = vu_f(vi[22]);
    float I = vu_f(vi[21]);
    float r[4];

    if (VU_U_IS_SPECIAL(w)) {
        uint32_t sub = VU_SPEC_SUBOP(w);
        uint32_t bc  = VU_U_BC(w);

        if (sub == VUS_FD_ADDA_GROUP && bc == 0) { /* ADDA: ACC = Fs + Ft */
            for (int l = 0; l < 4; l++) r[l] = vu_f(vf[fs][l]) + vu_f(vf[ft][l]);
            for (int l = 0; l < 4; l++) if ((dest >> (3 - l)) & 1) acc[l] = vu_u(r[l]);
            return 1;
        }
        if (sub == VUS_FD_ADDA_GROUP && bc == 1) { /* MADDA: ACC = ACC + Fs*Ft */
            for (int l = 0; l < 4; l++) r[l] = vu_f(acc[l]) + vu_f(vf[fs][l]) * vu_f(vf[ft][l]);
            for (int l = 0; l < 4; l++) if ((dest >> (3 - l)) & 1) acc[l] = vu_u(r[l]);
            return 1;
        }
        if (sub == VUS_FD_SUBA_GROUP && bc == 0) { /* SUBA: ACC = Fs - Ft */
            for (int l = 0; l < 4; l++) r[l] = vu_f(vf[fs][l]) - vu_f(vf[ft][l]);
            for (int l = 0; l < 4; l++) if ((dest >> (3 - l)) & 1) acc[l] = vu_u(r[l]);
            return 1;
        }
        if (sub == VUS_FD_SUBA_GROUP && bc == 1) { /* MSUBA: ACC = ACC - Fs*Ft */
            for (int l = 0; l < 4; l++) r[l] = vu_f(acc[l]) - vu_f(vf[fs][l]) * vu_f(vf[ft][l]);
            for (int l = 0; l < 4; l++) if ((dest >> (3 - l)) & 1) acc[l] = vu_u(r[l]);
            return 1;
        }
        if (sub == VUS_FD_SUBA_GROUP && bc == 2) { /* OPMULA: ACC = Fs x Ft (outer/cross product terms, xyz only) */
            acc[0] = vu_u(vu_f(vf[fs][1]) * vu_f(vf[ft][2]));
            acc[1] = vu_u(vu_f(vf[fs][2]) * vu_f(vf[ft][0]));
            acc[2] = vu_u(vu_f(vf[fs][0]) * vu_f(vf[ft][1]));
            return 1;
        }
        if (sub == VUS_FD_SUBA_GROUP && bc == 3) { /* NOP (upper) */
            return 1;
        }
        if (sub == VUS_FD_ABSCLIP && bc == 1) { /* ABS: Fd(=ft slot) = |Fs| */
            for (int l = 0; l < 4; l++) r[l] = fabsf(vu_f(vf[fs][l]));
            vu_write_dest(vf, ft, dest, r);
            return 1;
        }
        if (sub == VUS_FD_ITOF) { /* ITOF0/4/12/15: Fd(=ft slot) = (int32)Fs / 2^scale */
            static const int shift[4] = {0, 4, 12, 15};
            for (int l = 0; l < 4; l++)
                r[l] = (float)(int32_t)vf[fs][l] / (float)(1u << shift[bc]);
            vu_write_dest(vf, ft, dest, r);
            return 1;
        }
        if (sub == VUS_FD_FTOI) { /* FTOI0/4/12/15: Fd(=ft slot) = (int32)(Fs * 2^scale), bits stored raw */
            static const int shift[4] = {0, 4, 12, 15};
            for (int l = 0; l < 4; l++) {
                int32_t iv = (int32_t)(vu_f(vf[fs][l]) * (float)(1u << shift[bc]));
                r[l] = vu_f((uint32_t)iv);
            }
            vu_write_dest(vf, ft, dest, r);
            return 1;
        }
        return 0; /* CLIPw and the accumulator-broadcast/AI/AQ forms - not implemented, see vu_opcodes.h */
    }

    /* Class B: broadcast forms (bits5-2 = 0x0-0x6) */
    {
        uint32_t sub4 = VU_U_SUB4(w);
        if (sub4 <= 0x6u) {
            uint32_t bc = VU_U_BC(w);
            float ftbc = vu_f(vf[ft][bc]);
            int matched = 1;
            switch (sub4) {
                case VUB_ADDBC:  for (int l = 0; l < 4; l++) r[l] = vu_f(vf[fs][l]) + ftbc; break;
                case VUB_SUBBC:  for (int l = 0; l < 4; l++) r[l] = vu_f(vf[fs][l]) - ftbc; break;
                case VUB_MULBC:  for (int l = 0; l < 4; l++) r[l] = vu_f(vf[fs][l]) * ftbc; break;
                case VUB_MAXBC:  for (int l = 0; l < 4; l++) r[l] = fmaxf(vu_f(vf[fs][l]), ftbc); break;
                case VUB_MINIBC: for (int l = 0; l < 4; l++) r[l] = fminf(vu_f(vf[fs][l]), ftbc); break;
                case VUB_MADDBC: for (int l = 0; l < 4; l++) r[l] = vu_f(acc[l]) + vu_f(vf[fs][l]) * ftbc; break;
                case VUB_MSUBBC: for (int l = 0; l < 4; l++) r[l] = vu_f(acc[l]) - vu_f(vf[fs][l]) * ftbc; break;
                default: matched = 0; break;
            }
            if (matched) { vu_write_dest(vf, fd, dest, r); return 1; }
        }
    }

    /* Class A: full 6-bit funct */
    {
        uint32_t funct = VU_U_FUNCT6(w);
        switch (funct) {
            case VUA_ADD:  for (int l = 0; l < 4; l++) r[l] = vu_f(vf[fs][l]) + vu_f(vf[ft][l]); break;
            case VUA_SUB:  for (int l = 0; l < 4; l++) r[l] = vu_f(vf[fs][l]) - vu_f(vf[ft][l]); break;
            case VUA_MUL:  for (int l = 0; l < 4; l++) r[l] = vu_f(vf[fs][l]) * vu_f(vf[ft][l]); break;
            case VUA_MAX:  for (int l = 0; l < 4; l++) r[l] = fmaxf(vu_f(vf[fs][l]), vu_f(vf[ft][l])); break;
            case VUA_MINI: for (int l = 0; l < 4; l++) r[l] = fminf(vu_f(vf[fs][l]), vu_f(vf[ft][l])); break;
            case VUA_MADD: for (int l = 0; l < 4; l++) r[l] = vu_f(acc[l]) + vu_f(vf[fs][l]) * vu_f(vf[ft][l]); break;
            case VUA_MSUB: for (int l = 0; l < 4; l++) r[l] = vu_f(acc[l]) - vu_f(vf[fs][l]) * vu_f(vf[ft][l]); break;

            case VUA_ADDQ:  for (int l = 0; l < 4; l++) r[l] = vu_f(vf[fs][l]) + Q; break;
            case VUA_SUBQ:  for (int l = 0; l < 4; l++) r[l] = vu_f(vf[fs][l]) - Q; break;
            case VUA_MULQ:  for (int l = 0; l < 4; l++) r[l] = vu_f(vf[fs][l]) * Q; break;
            case VUA_MADDQ: for (int l = 0; l < 4; l++) r[l] = vu_f(acc[l]) + vu_f(vf[fs][l]) * Q; break;
            case VUA_MSUBQ: for (int l = 0; l < 4; l++) r[l] = vu_f(acc[l]) - vu_f(vf[fs][l]) * Q; break;

            case VUA_ADDI:  for (int l = 0; l < 4; l++) r[l] = vu_f(vf[fs][l]) + I; break;
            case VUA_SUBI:  for (int l = 0; l < 4; l++) r[l] = vu_f(vf[fs][l]) - I; break;
            case VUA_MULI:  for (int l = 0; l < 4; l++) r[l] = vu_f(vf[fs][l]) * I; break;
            case VUA_MADDI: for (int l = 0; l < 4; l++) r[l] = vu_f(acc[l]) + vu_f(vf[fs][l]) * I; break;
            case VUA_MSUBI: for (int l = 0; l < 4; l++) r[l] = vu_f(acc[l]) - vu_f(vf[fs][l]) * I; break;
            case VUA_MAXI:  for (int l = 0; l < 4; l++) r[l] = fmaxf(vu_f(vf[fs][l]), I); break;
            case VUA_MINII: for (int l = 0; l < 4; l++) r[l] = fminf(vu_f(vf[fs][l]), I); break;

            default: return 0; /* includes the 0x1B collision case - see vu_opcodes.h */
        }
        vu_write_dest(vf, fd, dest, r);
        return 1;
    }
}

/* ============================================================
 * LOWER WORD (integer ALU / load-store / branch / misc)
 * Returns 1 if handled. pc is the byte address of THIS instruction
 * pair (needed for branch target computation); on a taken branch,
 * branch_delay/branch_target are set using the same 1-instruction-
 * delay-slot mechanism as the E-bit (see vu.h).
 * ============================================================ */
static int vu_exec_lower(uint32_t vf[32][4], uint32_t *vi,
                          uint8_t *mem, uint32_t mem_mask,
                          uint32_t w, uint32_t pc,
                          uint32_t *branch_delay, uint32_t *branch_target)
{
    uint32_t opcode = VU_L_OPCODE(w);
    uint32_t dest = VU_L_DEST(w);
    uint32_t rt = VU_L_RT(w), rs = VU_L_RS(w), rd = VU_L_RD(w);

    /* real branch offset: bits10-0, sign-extended, scaled by 8
     * (doublewords), target = pc + 8 (delay slot) + offset*8 */
    int32_t off11 = vu_sext(VU_L_IMM11(w), 11);

    switch (opcode) {
        case VUL_B:
            *branch_delay = 2u; *branch_target = (uint32_t)((int32_t)pc + 8 + off11 * 8);
            return 1;
        case VUL_BAL:
            vu_write_vi(vi, rt, (pc + 16) >> 3); /* link: instruction-pair index after the delay slot - this project's own convention, see vu_opcodes.h note */
            *branch_delay = 2u; *branch_target = (uint32_t)((int32_t)pc + 8 + off11 * 8);
            return 1;
        case VUL_JR:
            *branch_delay = 2u; *branch_target = vu_read_vi16(vi, rs) * 8u; /* Is holds an instruction-pair index, matching MSCAL's own *8 convention (vu.h) */
            return 1;
        case VUL_JALR:
            vu_write_vi(vi, rt, (pc + 16) >> 3);
            *branch_delay = 2u; *branch_target = vu_read_vi16(vi, rs) * 8u;
            return 1;
        case VUL_IBEQ:
            if (vu_read_vi16(vi, rs) == vu_read_vi16(vi, rt)) { *branch_delay = 2u; *branch_target = (uint32_t)((int32_t)pc + 8 + off11 * 8); }
            return 1;
        case VUL_IBNE:
            if (vu_read_vi16(vi, rs) != vu_read_vi16(vi, rt)) { *branch_delay = 2u; *branch_target = (uint32_t)((int32_t)pc + 8 + off11 * 8); }
            return 1;
        case VUL_IBLTZ:
            if (vu_read_vi16_signed(vi, rs) < 0)  { *branch_delay = 2u; *branch_target = (uint32_t)((int32_t)pc + 8 + off11 * 8); }
            return 1;
        case VUL_IBGTZ:
            if (vu_read_vi16_signed(vi, rs) > 0)  { *branch_delay = 2u; *branch_target = (uint32_t)((int32_t)pc + 8 + off11 * 8); }
            return 1;
        case VUL_IBLEZ:
            if (vu_read_vi16_signed(vi, rs) <= 0) { *branch_delay = 2u; *branch_target = (uint32_t)((int32_t)pc + 8 + off11 * 8); }
            return 1;
        case VUL_IBGEZ:
            if (vu_read_vi16_signed(vi, rs) >= 0) { *branch_delay = 2u; *branch_target = (uint32_t)((int32_t)pc + 8 + off11 * 8); }
            return 1;

        case VUL_LQ: { /* Ft = mem[(Is+imm11)*16 .. +15] (full quadword, no dest-mask gating) */
            uint32_t addr = (uint32_t)((int32_t)vu_read_vi16(vi, rs) + off11) * 16u;
            for (int l = 0; l < 4; l++) {
                if (rt != 0) vf[rt][l] = vu_rd_le32(mem + ((addr + (uint32_t)l * 4u) & mem_mask));
            }
            return 1;
        }
        case VUL_SQ: { /* mem[(It+imm11)*16..] = Fs */
            uint32_t addr = (uint32_t)((int32_t)vu_read_vi16(vi, rt) + off11) * 16u;
            for (int l = 0; l < 4; l++)
                vu_wr_le32(mem + ((addr + (uint32_t)l * 4u) & mem_mask), vf[rs][l]);
            return 1;
        }
        case VUL_ILW: { /* It = mem[(Is+imm11)*16 + elem*4], truncated to 16 bits; elem = lowest set dest bit */
            int elem = 0;
            for (int l = 0; l < 4; l++) if ((dest >> (3 - l)) & 1) { elem = l; break; }
            uint32_t addr = (uint32_t)((int32_t)vu_read_vi16(vi, rs) + off11) * 16u + (uint32_t)elem * 4u;
            uint32_t word = vu_rd_le32(mem + (addr & mem_mask));
            vu_write_vi(vi, rt, word);
            return 1;
        }
        case VUL_ISW: { /* mem[(Is+imm11)*16 + elem*4] = It (zero-extended) */
            int elem = 0;
            for (int l = 0; l < 4; l++) if ((dest >> (3 - l)) & 1) { elem = l; break; }
            uint32_t addr = (uint32_t)((int32_t)vu_read_vi16(vi, rs) + off11) * 16u + (uint32_t)elem * 4u;
            vu_wr_le32(mem + (addr & mem_mask), vu_read_vi16(vi, rt));
            return 1;
        }

        case VU_L_SPECIAL_OPCODE: {
            uint32_t funct6 = VU_L_FUNCT6(w);
            switch (funct6) {
                case VULS_IADD:  vu_write_vi(vi, rd, vu_read_vi16(vi, rs) + vu_read_vi16(vi, rt)); return 1;
                case VULS_ISUB:  vu_write_vi(vi, rd, vu_read_vi16(vi, rs) - vu_read_vi16(vi, rt)); return 1;
                case VULS_IAND:  vu_write_vi(vi, rd, vu_read_vi16(vi, rs) & vu_read_vi16(vi, rt)); return 1;
                case VULS_IOR:   vu_write_vi(vi, rd, vu_read_vi16(vi, rs) | vu_read_vi16(vi, rt)); return 1;
                case VULS_IADDI: {
                    int32_t imm5 = vu_sext(rd, 5); /* 5-bit signed immediate in the rd slot (bits10-6) - see vu_opcodes.h */
                    vu_write_vi(vi, rt, (uint32_t)((int32_t)vu_read_vi16(vi, rs) + imm5));
                    return 1;
                }
                default: break;
            }

            {
                uint32_t fdslot = VU_L_RD(w);
                uint32_t bc2 = w & 0x3u;
                if (fdslot == VULS_FD_MOVE_GROUP) {
                    float rr[4];
                    for (int l = 0; l < 4; l++) rr[l] = vu_f(vf[rs][l]);
                    if (bc2 == 0) { vu_write_dest(vf, rt, dest, rr); return 1; } /* MOVE */
                    if (bc2 == 1) { /* MR32: rotate lanes (x,y,z,w) -> (y,z,w,x) */
                        float rot[4] = { rr[1], rr[2], rr[3], rr[0] };
                        vu_write_dest(vf, rt, dest, rot);
                        return 1;
                    }
                }
                if (fdslot == VULS_FD_LSQ_GROUP) {
                    /* LQI/SQI (post-increment), LQD/SQD (pre-decrement) - base register is Is (rs) */
                    if (bc2 == 0) { /* LQI */
                        uint32_t addr = vu_read_vi16(vi, rs) * 16u;
                        for (int l = 0; l < 4; l++) if (rt != 0) vf[rt][l] = vu_rd_le32(mem + ((addr + (uint32_t)l * 4u) & mem_mask));
                        vu_write_vi(vi, rs, vu_read_vi16(vi, rs) + 1u);
                        return 1;
                    }
                    if (bc2 == 1) { /* SQI */
                        uint32_t addr = vu_read_vi16(vi, rs) * 16u;
                        for (int l = 0; l < 4; l++) vu_wr_le32(mem + ((addr + (uint32_t)l * 4u) & mem_mask), vf[rt][l]);
                        vu_write_vi(vi, rs, vu_read_vi16(vi, rs) + 1u);
                        return 1;
                    }
                    if (bc2 == 2) { /* LQD: pre-decrement */
                        vu_write_vi(vi, rs, vu_read_vi16(vi, rs) - 1u);
                        uint32_t addr = vu_read_vi16(vi, rs) * 16u;
                        for (int l = 0; l < 4; l++) if (rt != 0) vf[rt][l] = vu_rd_le32(mem + ((addr + (uint32_t)l * 4u) & mem_mask));
                        return 1;
                    }
                    if (bc2 == 3) { /* SQD: pre-decrement */
                        vu_write_vi(vi, rs, vu_read_vi16(vi, rs) - 1u);
                        uint32_t addr = vu_read_vi16(vi, rs) * 16u;
                        for (int l = 0; l < 4; l++) vu_wr_le32(mem + ((addr + (uint32_t)l * 4u) & mem_mask), vf[rt][l]);
                        return 1;
                    }
                }
                if (fdslot == VULS_FD_MTIR_GROUP) {
                    if (bc2 == 0) { /* MTIR: It = Fs.elem (elem via bits22-21, see vu_opcodes.h) */
                        uint32_t elem = (w >> 21) & 0x3u;
                        vu_write_vi(vi, rt, vf[rs][elem]);
                        return 1;
                    }
                    if (bc2 == 1) { /* MFIR: Fd(masked lanes) = sign-extended It, raw bits (not numeric conversion) */
                        float rr[4];
                        uint32_t bits = (uint32_t)(int32_t)vu_read_vi16_signed(vi, rs);
                        for (int l = 0; l < 4; l++) rr[l] = vu_f(bits);
                        vu_write_dest(vf, rt, dest, rr);
                        return 1;
                    }
                    if (bc2 == 2) { /* ILWR: It = mem[Is*16 + elem*4], no immediate */
                        int elem = 0;
                        for (int l = 0; l < 4; l++) if ((dest >> (3 - l)) & 1) { elem = l; break; }
                        uint32_t addr = vu_read_vi16(vi, rs) * 16u + (uint32_t)elem * 4u;
                        vu_write_vi(vi, rt, vu_rd_le32(mem + (addr & mem_mask)));
                        return 1;
                    }
                    if (bc2 == 3) { /* ISWR: mem[Is*16 + elem*4] = It, no immediate */
                        int elem = 0;
                        for (int l = 0; l < 4; l++) if ((dest >> (3 - l)) & 1) { elem = l; break; }
                        uint32_t addr = vu_read_vi16(vi, rs) * 16u + (uint32_t)elem * 4u;
                        vu_wr_le32(mem + (addr & mem_mask), vu_read_vi16(vi, rt));
                        return 1;
                    }
                }
                if (fdslot == VULS_FD_DIVQ_GROUP) {
                    /* DIV/SQRT/RSQRT: exact fsf/ftf element-selector bit positions
                     * were not confidently recovered from the source document
                     * (see vu_opcodes.h) - this project uses element x (lane 0)
                     * of Fs/Ft as a documented, honest simplification rather
                     * than fabricate the real selector bits. Q is vi[22]. */
                    if (bc2 == 0) { vi[22] = vu_u(vu_f(vf[rs][0]) / vu_f(vf[rt][0])); return 1; } /* DIV */
                    if (bc2 == 1) { vi[22] = vu_u(sqrtf(vu_f(vf[rt][0]))); return 1; } /* SQRT (Ft only) */
                    if (bc2 == 2) { vi[22] = vu_u(vu_f(vf[rs][0]) / sqrtf(vu_f(vf[rt][0]))); return 1; } /* RSQRT */
                    if (bc2 == 3) { return 1; } /* WAITQ - non-pipelined interpreter, Q is always ready: true no-op */
                }
                if (fdslot == VULS_FD_WAITP && bc2 == 3) return 1; /* WAITP - same as WAITQ, no-op here */
                return 0; /* R-group (RNEXT/RGET/RINIT/RXOR - needs a real LFSR, not modeled) and anything else unmatched */
            }
        }

        default:
            return 0; /* FC-/FS-/FM-family flag ops, IADDIU/ISUBIU (uncertain immediate packing - see vu_opcodes.h) */
    }
}

/* See vu.h's header comment for the full citation and scope. */
int vu_micro_step(uint32_t vf[32][4], uint32_t *vi, uint32_t acc[4],
                   uint8_t *mem, uint32_t mem_mask,
                   uint8_t *micro, uint32_t micro_mask,
                   uint32_t *tpc, uint32_t *branch_delay, uint32_t *branch_target,
                   uint32_t *ebit_delay,
                   uint64_t *instructions_executed, uint64_t *unimplemented_opcodes_seen)
{
    uint32_t off = *tpc & micro_mask;
    uint32_t lower = vu_rd_le32(micro + off);       /* ptr[0] */
    uint32_t upper = vu_rd_le32(micro + off + 4u);  /* ptr[1] */
    uint32_t this_pc = off;

    *tpc = (off + 8u) & micro_mask;

    /* E flag (bit 30 of the upper word) - real hardware executes
     * exactly one more instruction after this one before actually
     * stopping (the classic VU "E-bit delay slot") - see header
     * comment for the exact countdown arithmetic this mirrors. */
    if (upper & 0x40000000u)
        *ebit_delay = 2u;

    int upper_ok, lower_ok;

    /* I flag (bit 31 of the upper word): only the upper instruction
     * executes this pair; the lower word's raw bits become the real
     * $I$ register (VI[21], REG_I per PCSX2's VU.h VURegFlags enum). */
    if (upper & 0x80000000u) {
        vi[21] = lower;
        upper_ok = vu_exec_upper(vf, vi, acc, upper);
        lower_ok = 1; /* the lower word was consumed as data (the I-immediate), not an instruction - correctly decoded, not "unimplemented" */
    } else {
        upper_ok = vu_exec_upper(vf, vi, acc, upper);
        lower_ok = vu_exec_lower(vf, vi, mem, mem_mask, lower, this_pc, branch_delay, branch_target);
    }

    if (!upper_ok || !lower_ok)
        (*unimplemented_opcodes_seen)++;
    (*instructions_executed)++;

    /* Branch delay-slot mechanism (same 1-instruction-delay pattern as
     * the E-bit, per PCSX2's _vu0Exec) - vu_exec_lower() above may
     * have just set branch_delay=2 and branch_target for a taken branch
     * this instruction; this countdown (shared with any branch armed
     * on a PRIOR step) is what actually redirects *tpc once the one
     * real delay-slot instruction has retired. */
    if (*branch_delay > 0) {
        if (--(*branch_delay) == 0)
            *tpc = *branch_target & micro_mask;
    }

    if (*ebit_delay > 0) {
        if (--(*ebit_delay) == 0)
            return 1; /* stopped */
    }
    return 0;
}

/* Safety cap on a single MSCAL/MSCNT run - this project's own guard
 * against a genuinely-infinite microprogram (e.g. malformed/garbage
 * micro memory with no E-bit ever set), not a real hardware behavior. */
#define VU_EXEC_STEP_CAP 65536u

void vu1_exec_micro(uint32_t start_addr)
{
    g_vu1.tpc = (start_addr << 3) & (VU1_MICRO_SIZE - 1u);
    g_vu1.running = 1;

    for (uint32_t i = 0; i < VU_EXEC_STEP_CAP; i++) {
        int stopped = vu_micro_step(g_vu1.vf, g_vu1.vi, g_vu1.acc,
                                     g_vu1.mem, VU1_MEM_SIZE - 1u,
                                     g_vu1.micro, VU1_MICRO_SIZE - 1u,
                                     &g_vu1.tpc, &g_vu1.branch_delay, &g_vu1.branch_target,
                                     &g_vu1.ebit_delay,
                                     &g_vu1.instructions_executed, &g_vu1.unimplemented_opcodes_seen);
        if (stopped)
            break;
    }

    g_vu1.running = 0;
}
