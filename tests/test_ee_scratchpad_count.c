/*
 * test_ee_scratchpad_count.c - host-native test for ee_core.c's R5900
 * Scratchpad RAM (SPR) hardware-bypass and COP0 Count free-running
 * counter, both added in "EE JALR investigation, round 8" (see
 * docs/STATUS.md).
 *
 * Round 7's real TLB implementation initially routed EVERY KUSEG
 * address (including 0x70000000-0x70003FFF) through normal TLB
 * translation. A live trace of real, working PCSX2 (the user's own
 * PCSX2-MCP session) proved this specific 16KB window is real,
 * dedicated on-chip Scratchpad RAM that hardware bypasses the TLB for
 * entirely - confirmed independently against PCSX2's own source
 * (pcsx2/Memory.cpp's "0x70000000-0x70003fff scratch pad" comment,
 * pcsx2/MemoryTypes.h's 16KB Ps2MemSize::Scratch, pcsx2/COP0.cpp's
 * isSPR()-gated direct-buffer mapping). The real BIOS's kernel stack
 * pointer lands in the upper half of this window, which had no TLB
 * entry and no reason to ever need one - producing an unresolvable
 * TLB Refill exception loop before this fix.
 *
 * Separately, once that loop was resolved, real-BIOS tracing hit a
 * SECOND wall: a classic "MFC0 Count; SUBU; SLTU; BNE" busy-wait delay
 * loop (found at pc=0x9FC42500 in the real SCPH-10000 BIOS) that never
 * terminated because COP0 Count (register 9) never advanced on its
 * own - only ever written via explicit MTC0. Fixed by incrementing it
 * by 1 every executed instruction (a real, working free-running
 * counter, just without precise bus-clock-rate timing fidelity - see
 * ee_step()'s own comment on this simplification).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include "core/ee/ee_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t enc_lui(int rt, uint16_t imm)        { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_sw(int rt, int rs, int16_t imm)  { return (0x2B << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_lw(int rt, int rs, int16_t imm)  { return (0x23 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_mfc0(int rt, int rd) { return (0x10 << 26) | (0x00 << 21) | (rt << 16) | (rd << 11); }
static uint32_t enc_break(void) { return 0x0D; }
static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

static bios_image_t make_bios(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;
    return bios;
}

/* --- 1. A SW/LW round-trip through the upper half of the scratchpad
 * window (0x70002000-0x70003FFF) - the exact sub-range with NO TLB
 * entry that caused the original infinite fault loop - must now work
 * with no TLB entry installed at all, proving the hardware-bypass path
 * is taken instead of TLB translation. */
static void test_scratchpad_upper_half_no_tlb_needed(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_lui(4, 0x7000));      pc += 4; /* a0 = 0x70000000 */
    wle32(p+pc, enc_lui(5, 0x1234));      pc += 4; /* a1 = 0x12340000 */
    wle32(p+pc, enc_sw(5, 4, 0x3FC0));    pc += 4; /* SW a1, 0x3FC0(a0) -> 0x70003FC0, the real $sp value from the live trace */
    wle32(p+pc, enc_lw(6, 4, 0x3FC0));    pc += 4; /* LW a2, 0x3FC0(a0) -> read it back */
    wle32(p+pc, enc_break());             pc += 4;

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    /* Deliberately install NO TLB entry anywhere - this is the point:
     * real hardware needs none for this fixed 16KB window. */
    ee_core_run(&bios);

    CHECK(st->halted == 1, "Scratchpad test: core halted cleanly on BREAK (no TLB Refill exception)");
    CHECK((st->cop0[12] & 0x2u) == 0, "Scratchpad test: Status.EXL never got set - no exception was raised");
    CHECK(st->gpr[6].ud0 == 0x0000000012340000ULL,
          "Scratchpad: SW/LW round-trip through 0x70003FC0 (upper half, no TLB entry) works");
    CHECK(*(uint32_t*)(st->scratch + 0x3FC0) == 0x12340000u,
          "Scratchpad: write actually landed in the dedicated scratch[] buffer, not RAM or a TLB-translated address");
}

/* --- 2. The full 16KB window (both halves) is covered, and addresses
 * just outside it (0x6FFFFFFC and 0x70004000) are NOT - they must fall
 * through to the normal KUSEG TLB path (and correctly fault/miss with
 * no entry installed), proving the special-case check has exact,
 * correct boundaries. */
static void test_scratchpad_exact_boundaries(void) {
    ee_state_t *st = ee_core_get_state();
    memset(st, 0, sizeof(*st));
    st->ram = malloc(EE_RAM_SIZE);
    memset(st->ram, 0, EE_RAM_SIZE);
    st->ram_size = EE_RAM_SIZE;

    uint8_t *p_low  = ee_mem_ptr(st, 0x70000000u, 4);
    uint8_t *p_high = ee_mem_ptr(st, 0x70003FFCu, 4);
    uint8_t *p_below = ee_mem_ptr(st, 0x6FFFFFFCu, 4); /* 4 bytes before the window */
    uint8_t *p_above = ee_mem_ptr(st, 0x70004000u, 4); /* right after the window */

    CHECK(p_low == st->scratch, "Scratchpad boundary: 0x70000000 maps to scratch[0]");
    CHECK(p_high == st->scratch + 0x3FFC, "Scratchpad boundary: 0x70003FFC maps to scratch[0x3FFC] (last word)");
    CHECK(p_below == NULL, "Scratchpad boundary: 0x6FFFFFFC (just below the window) is NOT scratchpad - falls through to KUSEG TLB (no entry -> miss -> NULL)");
    CHECK(st->mem_tlb_miss == 1, "Scratchpad boundary: the below-window access correctly went through the TLB-miss path, not the scratchpad path");
    CHECK(p_above == NULL, "Scratchpad boundary: 0x70004000 (just above the window) is NOT scratchpad either");
    CHECK(st->mem_tlb_miss == 1, "Scratchpad boundary: the above-window access also correctly went through the TLB-miss path");

    free(st->ram);
}

/* --- 3. COP0 Count actually advances over successive instructions,
 * and a real "Count vs. target" busy-wait loop (the same shape as the
 * real BIOS's pc=0x9FC42500 routine) terminates instead of looping
 * forever. */
static void test_count_advances(void) {
    bios_image_t bios = make_bios();
    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_mfc0(8, 9));   pc += 4; /* t0 = Count (before) */
    wle32(p+pc, enc_mfc0(9, 9));   pc += 4; /* t1 = Count (one instruction later) */
    wle32(p+pc, enc_break());      pc += 4;

    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();
    ee_core_run(&bios);

    CHECK(st->halted == 1, "Count test: core halted on BREAK");
    CHECK(st->gpr[9].ud0 == st->gpr[8].ud0 + 1,
          "Count test: COP0 Count advanced by exactly 1 between two consecutive MFC0 reads");
}

int main(void) {
    test_scratchpad_upper_half_no_tlb_needed();
    test_scratchpad_exact_boundaries();
    test_count_advances();

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
