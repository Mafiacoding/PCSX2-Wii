/*
 * test_system_ratio.c - host-native test for the EE:IOP clock-ratio
 * scheduling in source/core/system.c (system_run_interleaved()).
 *
 * This used to be strict 1:1 instruction stepping (one EE instruction,
 * one IOP instruction, repeat) - a documented simplification, since
 * real hardware's EE (294.912 MHz) and IOP (36.864 MHz) clocks are in
 * an exact 8:1 ratio. system_run_interleaved() now runs up to
 * EE_IOP_CLOCK_RATIO (8) EE instructions per IOP instruction each
 * slice. This test proves that ratio directly by giving each core a
 * long, plain NOP-then-BREAK program and checking real instruction
 * counts at a partial slice cap (where the ratio must hold exactly,
 * since neither core has halted yet to disturb the count) and again
 * after a full run to completion (where the IOP - the long-running
 * side here - ends up as the bottleneck, exactly as real hardware's
 * clock difference would predict: the EE finishes its short program
 * many slices before the IOP finishes its longer one).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

#include "core/system.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t enc_break(void) { return 0x0D; }
static void wle32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

/* Fills a BIOS image with nop_count NOPs (already zeroed, so this is
 * mostly documentation) followed by one BREAK, at the reset vector's
 * file offset (0). */
static void make_nop_break_program(bios_image_t *bios, int nop_count)
{
    memset(bios, 0, sizeof(*bios));
    bios->data = memalign(32, BIOS_MAX_SIZE);
    memset(bios->data, 0, BIOS_MAX_SIZE);
    bios->size = BIOS_MAX_SIZE;
    bios->loaded = 1;
    wle32(bios->data + (nop_count * 4), enc_break());
}

int main(void)
{
    CHECK(EE_IOP_CLOCK_RATIO == 8, "EE_IOP_CLOCK_RATIO matches real hardware's 294.912/36.864 MHz ratio");

    bios_image_t ee_bios, iop_bios;
    const int ee_nops = 100;   /* 100 NOP + 1 BREAK = 101 real instructions */
    const int iop_nops = 20;   /* 20 NOP + 1 BREAK = 21 real instructions */
    make_nop_break_program(&ee_bios, ee_nops);
    make_nop_break_program(&iop_bios, iop_nops);

    CHECK(system_init(&ee_bios, &iop_bios) == 0, "system_init succeeds");

    ee_state_t  *ee  = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    /* Partial run: 5 slices, well before either core halts. Per-slice
     * budget is exactly 8 EE : 1 IOP while neither core is halted, so
     * the counts after 5 slices must be exact, not approximate. */
    int result = system_run_interleaved(5);
    CHECK(result == 0, "partial run hits the slice cap (neither core halted yet)");
    CHECK(ee->instructions_executed == 40,
          "EE executed exactly 8 instructions/slice x 5 slices = 40");
    CHECK(iop->instructions_executed == 5,
          "IOP executed exactly 1 instruction/slice x 5 slices = 5");
    CHECK(ee->halted == 0 && iop->halted == 0, "neither core halted yet");

    /* Resume, unlimited, to completion. The IOP (21 real instructions,
     * 1/slice) needs 21 total slices to finish; the EE (101 real
     * instructions, up to 8/slice) needs only 13. Because the
     * scheduler doesn't stop just because one side halted, the EE
     * should finish first and the IOP should end up the bottleneck -
     * exactly the real-hardware-motivated point of this ratio. */
    result = system_run_interleaved(0);
    CHECK(result == 1, "full run: both cores halt on their own");
    /* Both counts equal the NOP count, not NOP count + 1: halt()'s
     * early return skips the shared epilogue's instructions_executed++
     * (see CLAUDE.md's "Known sharp edges" - a documented convention,
     * not a bug), so the BREAK that actually triggers the halt is
     * executed but not counted. */
    CHECK(ee->instructions_executed == (uint64_t)ee_nops,
          "EE's final instruction count matches its NOP count (BREAK executes but isn't counted)");
    CHECK(iop->instructions_executed == (uint64_t)iop_nops,
          "IOP's final instruction count matches its NOP count (BREAK executes but isn't counted)");
    CHECK(strstr(ee->halt_reason, "BREAK") != NULL, "EE halted via BREAK, not a spurious opcode fault");
    CHECK(strstr(iop->halt_reason, "BREAK") != NULL, "IOP halted via BREAK, not a spurious opcode fault");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
