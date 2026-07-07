/*
 * test_vu_micro.c - host-native test for the VU0/VU1 "micro mode"
 * microcode interpreter (source/hw/vu.c's vu_micro_step()/
 * vu1_exec_micro()/vu1_micro_write32(), and ee_core.c's
 * vu0_exec_micro()/vu0_micro_write32()). See include/core/hw/vu.h and
 * source/hw/vu_opcodes.h for the full scope/citation.
 *
 * UPDATE (task #94): vu_micro_step() now really decodes a substantial
 * subset of real VU upper/lower instructions (see vu_opcodes.h). One
 * consequence, verified here rather than glossed over: an all-zero
 * instruction pair is NOT "no real opcode" anymore - bits5-2==0 of the
 * upper word is the real ADDbc encoding (bc=x), and bits31-25==0 of
 * the lower word is the real LQ encoding - both degenerate (dest
 * mask=0000, or a VF00-targeted load, which real hardware discards)
 * but genuinely matched, real instructions. The old "every all-zero
 * pair counts as unimplemented" assertion has been updated to reflect
 * this (0 unimplemented, not 4) - this is a real, verified behavior
 * change from the fix, not a loosened test.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "core/hw/vu.h"
#include "core/ee/ee_core.h"
#include "core/bios_loader.h"
#include "hw/vu_opcodes.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

/* Builds one 8-byte VU instruction pair (lower word, upper word) at
 * micro-memory byte offset `off`, via the public micro_write32 API
 * (exactly how vif.c's MPG handling would populate it in practice). */
static void put_vu1_instr(uint32_t off, uint32_t lower, uint32_t upper)
{
    vu1_micro_write32(off, lower);
    vu1_micro_write32(off + 4, upper);
}

static void put_vu0_instr(ee_state_t *st, uint32_t off, uint32_t lower, uint32_t upper)
{
    vu0_micro_write32(st, off, lower);
    vu0_micro_write32(st, off + 4, upper);
}

/* ----------------------------------------------------------------
 * Task #94: raw-word encoders mirroring source/hw/vu_opcodes.h's bit
 * layout, used below to build real (not all-zero) VU instructions and
 * verify actual arithmetic/branch/load-store semantics.
 * ---------------------------------------------------------------- */
union vu_conv { uint32_t u; float f; };
static uint32_t vu_bits(float f) { union vu_conv c; c.f = f; return c.u; }
static float vu_floatof(uint32_t u) { union vu_conv c; c.u = u; return c.f; }

static uint32_t enc_upper_a(int e, uint32_t dest, uint32_t ft, uint32_t fs, uint32_t fd, uint32_t funct6)
{
    uint32_t w = 0;
    if (e) w |= 0x40000000u;
    w |= (dest & 0xFu) << 21; w |= (ft & 0x1Fu) << 16; w |= (fs & 0x1Fu) << 11;
    w |= (fd & 0x1Fu) << 6; w |= (funct6 & 0x3Fu);
    return w;
}
static uint32_t enc_upper_special(int e, uint32_t dest, uint32_t ft, uint32_t fs, uint32_t sub5, uint32_t bc)
{
    uint32_t w = 0;
    if (e) w |= 0x40000000u;
    w |= (dest & 0xFu) << 21; w |= (ft & 0x1Fu) << 16; w |= (fs & 0x1Fu) << 11;
    w |= (sub5 & 0x1Fu) << 6; w |= 0x3Cu /* 1111 00, bc filled below */;
    w = (w & ~0x3u) | (bc & 0x3u);
    return w;
}
static uint32_t enc_upper_b(int e, uint32_t dest, uint32_t ft, uint32_t fs, uint32_t fd, uint32_t sub4, uint32_t bc)
{
    uint32_t w = 0;
    if (e) w |= 0x40000000u;
    w |= (dest & 0xFu) << 21; w |= (ft & 0x1Fu) << 16; w |= (fs & 0x1Fu) << 11;
    w |= (fd & 0x1Fu) << 6; w |= ((sub4 & 0xFu) << 2) | (bc & 0x3u);
    return w;
}
static uint32_t enc_lower_direct(uint32_t opcode7, uint32_t dest, uint32_t rt, uint32_t rs, int32_t imm11)
{
    uint32_t w = (opcode7 & 0x7Fu) << 25;
    w |= (dest & 0xFu) << 21; w |= (rt & 0x1Fu) << 16; w |= (rs & 0x1Fu) << 11;
    w |= ((uint32_t)imm11) & 0x7FFu;
    return w;
}
static uint32_t enc_lower_special(uint32_t dest, uint32_t rt, uint32_t rs, uint32_t rd, uint32_t funct6)
{
    uint32_t w = (0x40u) << 25;
    w |= (dest & 0xFu) << 21; w |= (rt & 0x1Fu) << 16; w |= (rs & 0x1Fu) << 11;
    w |= (rd & 0x1Fu) << 6; w |= (funct6 & 0x3Fu);
    return w;
}
static int approx(float a, float b) { float d = a - b; if (d < 0) d = -d; return d < 0.0001f; }

int main(void)
{
    /* --- VU1: a 3-instruction program, E-bit on the 3rd --- */
    vu1_init();
    put_vu1_instr(0, 0x00000000u, 0x00000000u); /* plain instruction */
    put_vu1_instr(8, 0x00000000u, 0x00000000u); /* plain instruction */
    put_vu1_instr(16, 0x00000000u, 0x40000000u); /* E-bit set on the upper word */
    put_vu1_instr(24, 0x00000000u, 0x00000000u); /* the real "E-bit delay slot" instruction - still executes */
    put_vu1_instr(32, 0x00000000u, 0x00000000u); /* must NOT execute - program already stopped by here */

    vu1_exec_micro(0); /* start_addr=0 -> byte offset 0 */

    vu1_state_t *vu1 = vu1_get_state();
    CHECK(vu1->running == 0, "VU1: program is not running after vu1_exec_micro() returns");
    CHECK(vu1->instructions_executed == 4,
          "VU1: executed exactly 4 instructions (the E-bit one plus its real one-instruction delay slot, then stopped)");
    CHECK(vu1->unimplemented_opcodes_seen == 0,
          "VU1: all-zero words now genuinely decode as real (degenerate, no-effect) ADDbc/LQ instructions - see task #94 note above");
    CHECK(vu1->tpc == 32u, "VU1: TPC ended up pointing at the (unexecuted) 5th instruction, not wrapped/misplaced");

    /* --- VU1: MSCAL start address is in instruction-pair units (*8 to get bytes) --- */
    vu1_init();
    put_vu1_instr(0, 0, 0);   /* would run forever without ever setting E if started here */
    put_vu1_instr(8, 0, 0x40000000u); /* E-bit instruction at instruction-pair index 1 (byte offset 8) */
    put_vu1_instr(16, 0, 0);  /* delay slot */
    vu1_exec_micro(1); /* start_addr=1 -> byte offset 8, per vu.h's documented *8 convention */
    vu1 = vu1_get_state();
    CHECK(vu1->instructions_executed == 2,
          "VU1: MSCAL start_addr is correctly interpreted as an instruction-pair index (*8 for the byte offset)");

    /* --- VU1: the safety cap prevents a genuinely infinite program from hanging the test --- */
    vu1_init();
    /* Every byte left at 0 (memset by vu1_init) - an all-zero
     * "program" never sets the E bit, so this only terminates via
     * the safety cap, not a real E-bit stop. */
    vu1_exec_micro(0);
    vu1 = vu1_get_state();
    CHECK(vu1->running == 0, "VU1: safety cap run also leaves running=0 afterward");
    CHECK(vu1->instructions_executed == 65536u, "VU1: safety cap (65536 instructions) was hit, not a real E-bit stop");

    /* --- VU1: I-bit sets VI[21] (REG_I) from the lower word's raw bits, and skips the lower instruction --- */
    vu1_init();
    put_vu1_instr(0, 0x12345678u, 0x80000000u); /* I flag set */
    put_vu1_instr(8, 0, 0x40000000u); /* E-bit, to stop cleanly */
    put_vu1_instr(16, 0, 0);
    vu1_exec_micro(0);
    vu1 = vu1_get_state();
    CHECK(vu1->vi[21] == 0x12345678u, "VU1: I-flag instruction correctly loaded VI[21] (REG_I) from the lower word's raw bits");

    /* --- VU1: MPG writes real bytes (via the public API vif.c uses) --- */
    vu1_init();
    vu1_micro_write32(0x100, 0xDEADBEEFu);
    vu1 = vu1_get_state();
    CHECK(vu1->micro[0x100] == 0xEFu && vu1->micro[0x101] == 0xBEu &&
          vu1->micro[0x102] == 0xADu && vu1->micro[0x103] == 0xDEu,
          "VU1: vu1_micro_write32 stores little-endian, matching this project's other memory helpers");

    /* --- VU1: micro-memory address wraps at VU1_MICRO_SIZE (16KB), matching real hardware --- */
    vu1_init();
    vu1_micro_write32(VU1_MICRO_SIZE, 0x11223344u); /* wraps to offset 0 */
    vu1 = vu1_get_state();
    CHECK(vu1->micro[0] == 0x44u, "VU1: micro-instruction memory address wraps at the real 16KB boundary");

    /* --- VU0: same control-flow correctness, reusing ee_state_t's shared VF/VI/mem fields --- */
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();

    put_vu0_instr(st, 0, 0, 0);
    put_vu0_instr(st, 8, 0, 0x40000000u); /* E-bit */
    put_vu0_instr(st, 16, 0, 0);          /* delay slot */
    put_vu0_instr(st, 24, 0, 0);          /* must not execute */

    vu0_exec_micro(st, 0);

    CHECK(st->vu0_running == 0, "VU0: program is not running after vu0_exec_micro() returns");
    CHECK(st->vu0_instructions_executed == 3,
          "VU0: executed exactly 3 instructions (the plain one, the E-bit one, and its real delay slot)");
    CHECK(st->cop2_ctrl[26] == 24u,
          "VU0: real TPC register (cop2_ctrl[26], REG_TPC) reflects the live program counter after execution");

    /* --- VU0: micro-instruction memory is genuinely separate from VU1's --- */
    CHECK(st->vu0_micro[0] == 0 && st->vu0_micro[4] == 0,
          "VU0: micro memory at offset 0 is untouched by the earlier VU1-only writes (separate memories)");

    /* ================================================================
     * Task #94: real VU opcode table - targeted arithmetic/branch/
     * load-store checks (not just control flow), against the exact
     * encodings documented in source/hw/vu_opcodes.h.
     * ================================================================ */

    /* --- real ADD: Fd = Fs + Ft, full dest mask --- */
    vu1_init();
    vu1 = vu1_get_state();
    vu1->vf[1][0] = vu_bits(1.0f); vu1->vf[1][1] = vu_bits(2.0f); vu1->vf[1][2] = vu_bits(3.0f); vu1->vf[1][3] = vu_bits(4.0f);
    vu1->vf[2][0] = vu_bits(10.0f); vu1->vf[2][1] = vu_bits(20.0f); vu1->vf[2][2] = vu_bits(30.0f); vu1->vf[2][3] = vu_bits(40.0f);
    put_vu1_instr(0, 0, enc_upper_a(0, 0xF, 2, 1, 3, VUA_ADD));
    put_vu1_instr(8, 0, enc_upper_a(1, 0, 0, 0, 0, 0)); /* E-bit, otherwise inert */
    put_vu1_instr(16, 0, 0);                            /* E-bit delay slot */
    vu1_exec_micro(0);
    vu1 = vu1_get_state();
    CHECK(approx(vu_floatof(vu1->vf[3][0]), 11.0f) && approx(vu_floatof(vu1->vf[3][1]), 22.0f) &&
          approx(vu_floatof(vu1->vf[3][2]), 33.0f) && approx(vu_floatof(vu1->vf[3][3]), 44.0f),
          "VU1: real ADD (Class A funct 0x28) computes Fd = Fs + Ft per-lane");

    /* --- real ADDA then MADD: ACC = Fs+Ft, then Fd = ACC + Fs2*Ft2 --- */
    vu1_init();
    vu1 = vu1_get_state();
    vu1->vf[4][0] = vu_bits(1.0f); vu1->vf[4][1] = vu_bits(1.0f); vu1->vf[4][2] = vu_bits(1.0f); vu1->vf[4][3] = vu_bits(1.0f);
    vu1->vf[5][0] = vu_bits(2.0f); vu1->vf[5][1] = vu_bits(2.0f); vu1->vf[5][2] = vu_bits(2.0f); vu1->vf[5][3] = vu_bits(2.0f);
    put_vu1_instr(0, 0, enc_upper_special(0, 0xF, 5, 4, VUS_FD_ADDA_GROUP, 0)); /* ADDA: ACC = vf4+vf5 = (3,3,3,3) */
    put_vu1_instr(8, 0, enc_upper_a(0, 0xF, 5, 4, 6, VUA_MADD));               /* MADD: vf6 = ACC + vf4*vf5 = 3+2 = (5,5,5,5) */
    put_vu1_instr(16, 0, enc_upper_a(1, 0, 0, 0, 0, 0));                      /* E-bit */
    put_vu1_instr(24, 0, 0);
    vu1_exec_micro(0);
    vu1 = vu1_get_state();
    CHECK(approx(vu_floatof(vu1->vf[6][0]), 5.0f) && approx(vu_floatof(vu1->vf[6][3]), 5.0f),
          "VU1: real ADDA (writes ACC) followed by real MADD (reads ACC) - the accumulator register genuinely works");

    /* --- real ADDbc: broadcast one lane of Ft (bc=2 -> z) to all lanes --- */
    vu1_init();
    vu1 = vu1_get_state();
    vu1->vf[7][0] = vu_bits(1.0f); vu1->vf[7][1] = vu_bits(2.0f); vu1->vf[7][2] = vu_bits(3.0f); vu1->vf[7][3] = vu_bits(4.0f);
    vu1->vf[8][0] = vu_bits(100.0f); vu1->vf[8][1] = vu_bits(200.0f); vu1->vf[8][2] = vu_bits(300.0f); vu1->vf[8][3] = vu_bits(400.0f);
    put_vu1_instr(0, 0, enc_upper_b(0, 0xF, 8, 7, 9, VUB_ADDBC, 2 /* bc=z */));
    put_vu1_instr(8, 0, enc_upper_a(1, 0, 0, 0, 0, 0));
    put_vu1_instr(16, 0, 0);
    vu1_exec_micro(0);
    vu1 = vu1_get_state();
    CHECK(approx(vu_floatof(vu1->vf[9][0]), 301.0f) && approx(vu_floatof(vu1->vf[9][1]), 302.0f) &&
          approx(vu_floatof(vu1->vf[9][2]), 303.0f) && approx(vu_floatof(vu1->vf[9][3]), 304.0f),
          "VU1: real ADDbc broadcasts the selected lane (bc=2=z here) of Ft to every dest lane");

    /* --- real ADDQ: uses the Q register (vi[22]), not Ft --- */
    vu1_init();
    vu1 = vu1_get_state();
    vu1->vf[10][0] = vu_bits(1.0f); vu1->vf[10][1] = vu_bits(2.0f); vu1->vf[10][2] = vu_bits(3.0f); vu1->vf[10][3] = vu_bits(4.0f);
    vu1->vi[22] = vu_bits(5.0f); /* simulating a prior DIV/SQRT having set Q */
    put_vu1_instr(0, 0, enc_upper_a(0, 0xF, 0, 10, 11, VUA_ADDQ));
    put_vu1_instr(8, 0, enc_upper_a(1, 0, 0, 0, 0, 0));
    put_vu1_instr(16, 0, 0);
    vu1_exec_micro(0);
    vu1 = vu1_get_state();
    CHECK(approx(vu_floatof(vu1->vf[11][0]), 6.0f) && approx(vu_floatof(vu1->vf[11][3]), 9.0f),
          "VU1: real ADDQ uses the Q register (vi[22]) as the second operand, not Ft");

    /* --- real branch (B): the delay-slot instruction always executes, the skipped one never does --- */
    vu1_init();
    vu1 = vu1_get_state();
    vu1->vf[20][0] = vu_bits(9.0f); vu1->vf[20][1] = vu_bits(9.0f); vu1->vf[20][2] = vu_bits(9.0f); vu1->vf[20][3] = vu_bits(9.0f);
    vu1->vf[21][0] = vu_bits(7.0f); vu1->vf[21][1] = vu_bits(7.0f); vu1->vf[21][2] = vu_bits(7.0f); vu1->vf[21][3] = vu_bits(7.0f);
    put_vu1_instr(0, enc_lower_direct(VUL_B, 0, 0, 0, 2), 0);                    /* B target = pc(0)+8+2*8 = 24 */
    put_vu1_instr(8, enc_lower_special(0xF, 11, 20, VULS_FD_MOVE_GROUP, 0x3C), 0); /* delay slot: MOVE vf20->vf11 - ALWAYS executes */
    put_vu1_instr(16, enc_lower_special(0xF, 11, 21, VULS_FD_MOVE_GROUP, 0x3C), 0);/* poison: MOVE vf21->vf11 - must be SKIPPED */
    put_vu1_instr(24, 0, enc_upper_a(1, 0, 0, 0, 0, 0));                          /* E-bit at the branch target */
    put_vu1_instr(32, 0, 0);
    vu1_exec_micro(0);
    vu1 = vu1_get_state();
    CHECK(approx(vu_floatof(vu1->vf[11][0]), 9.0f),
          "VU1: real B branch - the delay-slot instruction (MOVE vf20) executed, the skipped one (MOVE vf21) did not");
    CHECK(vu1->instructions_executed == 4,
          "VU1: real B branch - exactly 4 instructions retired (branch, delay slot, E-bit instr at target, its own delay slot)");

    /* --- real IADD: 16-bit integer register arithmetic --- */
    vu1_init();
    vu1 = vu1_get_state();
    vu1->vi[1] = 100; vu1->vi[2] = 23;
    put_vu1_instr(0, enc_lower_special(0, 2, 1, 3, VULS_IADD), 0); /* vi3 = vi1 + vi2 */
    put_vu1_instr(8, 0, enc_upper_a(1, 0, 0, 0, 0, 0));
    put_vu1_instr(16, 0, 0);
    vu1_exec_micro(0);
    vu1 = vu1_get_state();
    CHECK(vu1->vi[3] == 123, "VU1: real IADD computes vi[rd] = vi[rs] + vi[rt]");

    /* --- real LQ/SQ round-trip through VU1 data memory --- */
    vu1_init();
    vu1 = vu1_get_state();
    vu1->vf[15][0] = vu_bits(1.5f); vu1->vf[15][1] = vu_bits(2.5f); vu1->vf[15][2] = vu_bits(3.5f); vu1->vf[15][3] = vu_bits(4.5f);
    vu1->vi[1] = 2; /* quadword index 2 -> byte address 32 */
    put_vu1_instr(0, enc_lower_direct(VUL_SQ, 0, 1, 15, 0), 0); /* SQ vf15, 0(vi1) */
    put_vu1_instr(8, enc_lower_direct(VUL_LQ, 0, 16, 1, 0), 0); /* LQ vf16, 0(vi1) */
    put_vu1_instr(16, 0, enc_upper_a(1, 0, 0, 0, 0, 0));
    put_vu1_instr(24, 0, 0);
    vu1_exec_micro(0);
    vu1 = vu1_get_state();
    CHECK(vu1->mem[34] != 0 && vu1->mem[35] == 0x3F, "VU1: real SQ actually wrote the correct raw bytes into data memory (1.5f = 0x3FC00000 little-endian)");
    CHECK(approx(vu_floatof(vu1->vf[16][0]), 1.5f) && approx(vu_floatof(vu1->vf[16][3]), 4.5f),
          "VU1: real LQ reads back exactly what SQ wrote (full quadword round-trip through VU1 data memory)");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
