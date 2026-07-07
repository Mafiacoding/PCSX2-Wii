#ifndef PCSX2WII_VU_OPCODES_H
#define PCSX2WII_VU_OPCODES_H

/*
 * vu_opcodes.h - real VU "micro mode" instruction encoding (task #94).
 *
 * SOURCE: the original Sony "PlayStation 2 Vector Unit Instruction
 * Manual" (text by "BigBoss", layout by "Jules", 2003) - a primary
 * hardware reference, publicly available (e.g.
 * lukasz.dk/files/vu-instruction-manual.pdf). This project could not
 * locate PCSX2's own opcode-number-to-mnemonic dispatch tables in any
 * previously-fetched PCSX2 source file (see vu.h's prior-round note),
 * so this round went to the original manual instead. The manual's
 * tables are hand-drawn ASCII bit-boxes; the text extraction used to
 * read it linearizes those boxes, so some fields required
 * reconstruction from context. Every value below is either (a)
 * directly, verbatim quoted from the extracted text ("solid"), or (b)
 * explicitly marked as inferred/reconstructed with the reasoning
 * given, per this project's no-fabrication policy - nothing here is a
 * silent guess.
 *
 * INSTRUCTION FORMAT (64 bits = one lower word + one upper word,
 * fetched as a pair - ptr[0]=lower, ptr[1]=upper, matching vu.c's
 * existing vu_rd_le32 convention):
 *
 * Upper word (FMAC arithmetic):
 *   bit  31    = I flag (already implemented in vu.c before this round)
 *   bit  30    = E flag (already implemented)
 *   bits 29-27 = M/D/T flags (not modeled - no VU debug-interrupt path exists)
 *   bits 26-25 = always 0 in every example in the manual
 *   bits 24-21 = dest (x,y,z,w write-mask nibble, bit24=x..bit21=w)
 *   bits 20-16 = ft (5-bit source/broadcast vector register)
 *   bits 15-11 = fs (5-bit source vector register)
 *   bits 10-6  = fd (5-bit dest vector register for "Class A" ops;
 *                repurposed as a 5-bit sub-opcode for "SPECIAL" ops - see below)
 *   bits 5-0   = either a full 6-bit "Class A" funct value, or a 4-bit
 *                "Class B" broadcast sub-op (bits 5-2) + 2-bit bc (bits 1-0)
 *
 * Lower word (integer ALU / branch / load-store / misc):
 *   bits 31-25 = primary 7-bit opcode (direct dispatch), OR the fixed
 *                value 0x40 flagging "SPECIAL" (real sub-op in bits 5-0
 *                and/or bits 10-6, mirroring the upper word's mechanism)
 *   bits 24-21 = dest mask (ILW/ISW/MOVE/MR32/etc) or literal 0 (branches)
 *   bits 20-16 = rt / it / ft
 *   bits 15-11 = rs / is / fs / base
 *   bits 10-6  = rd / id / fd, OR a SPECIAL sub-opcode slot
 *   bits 5-0   = SPECIAL sub-funct (6 bits), when opcode==0x40
 *
 * bc (2-bit broadcast/element selector, upper AND lower): the manual
 * verbatim confirms 00=x, 01=y, 11=w at line ~724 of the extracted
 * text but the "10=z" row is missing from that specific table (an
 * apparent OCR/table-linearization dropout - the surrounding rows are
 * clean and consecutive). x=00,y=01,z=10,w=11 is filled in by pattern
 * and is also the universally-used encoding in every other PS2 VU
 * reference this project is aware of, so it is used throughout - but
 * flagged here as the one field this specific source document does
 * not verbatim confirm for all 4 values.
 */

/* ---------------- upper word field extraction ---------------- */
#define VU_U_DEST(w)   (((w) >> 21) & 0xFu)
#define VU_U_FT(w)     (((w) >> 16) & 0x1Fu)
#define VU_U_FS(w)     (((w) >> 11) & 0x1Fu)
#define VU_U_FD(w)     (((w) >> 6)  & 0x1Fu)   /* Class A dest reg, or SPECIAL sub-op */
#define VU_U_FUNCT6(w) ((w) & 0x3Fu)
#define VU_U_SUB4(w)   (((w) >> 2)  & 0xFu)
#define VU_U_BC(w)     ((w) & 0x3u)
#define VU_U_IS_SPECIAL(w) (VU_U_SUB4(w) == 0xFu)

/* Class A: full 6-bit funct (bits 5-0), fd (bits 10-6) is a real
 * vector register. Solid - each value directly quoted from the
 * manual's per-instruction diagram. NOTE: this project's extraction
 * also found a value for "OPMSUB" (0x1B) placed in this same table -
 * but 0x1B's low 4 bits (bits5-2=0110, bc=11) are bit-for-bit
 * IDENTICAL to Class B's MULbc with bc=w (see VUB_MULBC below), an
 * irreconcilable collision the source document does not explain (real
 * hardware cannot have two different instructions share one
 * encoding). Per the no-fabrication policy, this project does NOT
 * special-case 0x1B as OPMSUB (which would risk silently misdecoding
 * the far more common MULbc.w as the rare outer-product-subtract
 * instead) - 0x1B is treated uniformly as MULbc via the Class B path
 * below, and OPMSUB itself is left unimplemented (falls through to
 * unimplemented_opcodes_seen). */
enum {
    VUA_ADD    = 0x28, VUA_SUB    = 0x2C, VUA_MUL   = 0x2A,
    VUA_MADD   = 0x29, VUA_MSUB   = 0x2D,
    VUA_MAX    = 0x2B, VUA_MINI   = 0x2F,
    VUA_ADDQ   = 0x20, VUA_SUBQ   = 0x24, VUA_MULQ  = 0x1C,
    VUA_MADDQ  = 0x21, VUA_MSUBQ  = 0x25,
    VUA_ADDI   = 0x22, VUA_SUBI   = 0x26, VUA_MULI  = 0x1E,
    VUA_MADDI  = 0x23, VUA_MSUBI  = 0x27,
    VUA_MAXI   = 0x1D, VUA_MINII  = 0x1F
};

/* Class B: broadcast forms - bits5-2 sub-op (0x0-0x6), bc = bits1-0
 * selects which lane of ft is broadcast (x/y/z/w per the ordering
 * above). Solid, directly quoted (clean 4+2 bit split in the source). */
enum {
    VUB_ADDBC = 0x0, VUB_SUBBC  = 0x1, VUB_MADDBC = 0x2, VUB_MSUBBC = 0x3,
    VUB_MAXBC = 0x4, VUB_MINIBC = 0x5, VUB_MULBC  = 0x6
};

/* SPECIAL class (bits5-2==0xF): the real sub-operation lives in the fd
 * slot (bits 10-6, 5 bits), with bc (bits1-0) further discriminating a
 * small, internally-consistent family sharing one fd-slot value. Each
 * group below is solid (the manual shows an unambiguous, fully-
 * enumerated 4-way split by bc for every group actually implemented
 * here). Some *A-accumulator combinations found in the source
 * (ADDAbc, SUBAbc, MADDAbc, MSUBAbc, the AI, and AQ) reuse fd-slot values in a
 * way the manual's own diagrams don't cleanly resolve (e.g. ADDAI and
 * MADDAI show the identical fd-slot value with different bc, which is
 * plausible, but several other combinations look like a genuine
 * table-linearization casualty) - those are deliberately NOT
 * implemented here (left unimplemented rather than guessed). */
#define VU_SPEC_SUBOP(w) VU_U_FD(w) /* bits 10-6 when SPECIAL */

enum {
    VUS_FD_SUBA_GROUP = 0x0B, /* bc: 00=SUBA 01=MSUBA 10=OPMULA 11=NOP */
    VUS_FD_ADDA_GROUP = 0x0A, /* bc: 00=ADDA 01=MADDA (10/11 not in source) */
    VUS_FD_ABSCLIP    = 0x07, /* bc: 01=ABS   11=CLIPw (00/10 not in source) */
    VUS_FD_ITOF       = 0x04, /* bc: 00=ITOF0 01=ITOF4 10=ITOF12 11=ITOF15 */
    VUS_FD_FTOI       = 0x05  /* bc: 00=FTOI0 01=FTOI4 10=FTOI12 11=FTOI15 */
};

/* ---------------- lower word field extraction ---------------- */
#define VU_L_OPCODE(w) (((w) >> 25) & 0x7Fu)
#define VU_L_DEST(w)   (((w) >> 21) & 0xFu)
#define VU_L_RT(w)     (((w) >> 16) & 0x1Fu)
#define VU_L_RS(w)     (((w) >> 11) & 0x1Fu)
#define VU_L_RD(w)     (((w) >> 6)  & 0x1Fu)
#define VU_L_FUNCT6(w) ((w) & 0x3Fu)
#define VU_L_IMM11(w)  ((w) & 0x7FFu)
#define VU_L_SPECIAL_OPCODE 0x40u

/* Direct (non-SPECIAL) opcodes, bits 31-25. Solid unless noted. */
enum {
    VUL_LQ     = 0x00, VUL_SQ     = 0x01,
    VUL_ILW    = 0x04, VUL_ISW    = 0x05,
    VUL_IADDIU = 0x08, VUL_ISUBIU = 0x09,
    VUL_FCEQ   = 0x10, VUL_FCSET  = 0x11, VUL_FCAND = 0x12, VUL_FCOR = 0x13,
    VUL_FSEQ   = 0x14, VUL_FSSET  = 0x15, VUL_FSAND = 0x16, VUL_FSOR = 0x17,
    VUL_FMEQ   = 0x18, VUL_FMAND  = 0x1A, VUL_FMOR  = 0x1B, VUL_FCGET = 0x1C,
    VUL_B      = 0x20, VUL_BAL    = 0x21,
    VUL_JR     = 0x24, VUL_JALR   = 0x25,
    VUL_IBEQ   = 0x28, VUL_IBNE   = 0x29,
    VUL_IBLTZ  = 0x2C, /* NOT found verbatim in the source text - the
                         * manual documents IBEQ(0x28)/IBNE(0x29)/
                         * IBGTZ(0x2D)/IBLEZ(0x2E)/IBGEZ(0x2F) but no
                         * IBLTZ entry appeared anywhere in the fully-
                         * read document. 0x2C is the one gap in that
                         * numeric run and is used here on that
                         * inference alone - flagged, not verbatim. */
    VUL_IBGTZ  = 0x2D, VUL_IBLEZ  = 0x2E, VUL_IBGEZ = 0x2F
};

/* SPECIAL (opcode==0x40) sub-funct, full 6-bit bits5-0 value. Solid -
 * directly quoted (IADD/ISUB/IADDI/IOR/IAND). */
enum {
    VULS_IADD  = 0x30, VULS_ISUB = 0x31, VULS_IADDI = 0x32,
    VULS_IOR   = 0x33, VULS_IAND = 0x34
};

/* SPECIAL sub-groups identified by the fd slot (bits 10-6), each with
 * a clean, fully-enumerated 4-way bc split (bits1-0 of the combined
 * low bits) - solid, per the "concrete combined bits[10:0]" values
 * quoted directly from the source document. */
enum {
    VULS_FD_MOVE_GROUP  = 0x0C, /* bc: 00=MOVE 01=MR32 */
    VULS_FD_LSQ_GROUP   = 0x0D, /* bc: 00=LQI 01=SQI 10=LQD 11=SQD */
    VULS_FD_DIVQ_GROUP  = 0x0E, /* bc: 00=DIV 01=SQRT 10=RSQRT 11=WAITQ */
    VULS_FD_MTIR_GROUP  = 0x0F, /* bc: 00=MTIR 01=MFIR 10=ILWR 11=ISWR */
    VULS_FD_R_GROUP     = 0x10, /* bc: 00=RNEXT 01=RGET 10=RINIT 11=RXOR - NOT implemented (needs a real LFSR model) */
    VULS_FD_WAITP       = 0x1E  /* bc==11 only value found */
};

#endif
