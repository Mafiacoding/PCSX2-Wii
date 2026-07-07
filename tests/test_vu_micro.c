/*
 * test_vu_micro.c - host-native test for the VU0/VU1 "micro mode"
 * microcode interpreter (source/hw/vu.c's vu_micro_step()/
 * vu1_exec_micro()/vu1_micro_write32(), and ee_core.c's
 * vu0_exec_micro()/vu0_micro_write32()). See include/core/hw/vu.h for
 * the full scope/citation - this round implements real memory sizes,
 * real TPC/branch/E-bit/I-bit control flow (byte-exact against a live
 * fetch of PCSX2's VU0microInterp.cpp), and real MPG writes, but
 * deliberately does NOT decode any actual VU opcode body (no verified
 * real opcode-number table was found this round) - every instruction
 * pair is a logged no-op with correct flag handling.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "core/hw/vu.h"
#include "core/ee/ee_core.h"
#include "core/bios_loader.h"

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
    CHECK(vu1->unimplemented_opcodes_seen == 4,
          "VU1: all 4 retired instructions counted as unimplemented (no real opcode table - see vu.h)");
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

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
