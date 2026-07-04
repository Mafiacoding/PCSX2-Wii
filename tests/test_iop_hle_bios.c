/*
 * test_iop_hle_bios.c - host-native test for the IOP BIOS syscall
 * trap (source/hw/iop_hle_bios.c), wired into iop_core.c's iop_step().
 *
 * Hand-encodes a small IOP program that sets $t1 (the real PS1/PS2
 * BIOS call-number register) to a chosen function number, then does
 * `JAL 0xA0` - a real jump-and-link instruction targeting the A0
 * trap address. If the trap fires correctly, control should return
 * immediately to the instruction after JAL's delay slot (as if a
 * real `JR $ra` had executed at 0xA0), $v0 should read back as the
 * generic default (0), and the call should be visible in
 * iop_hle_bios_get_state().
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include "core/iop/iop_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t enc_addiu(int rt, int rs, int16_t imm) { return (0x09 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_jal(uint32_t target)                { return (0x03 << 26) | ((target >> 2) & 0x03FFFFFFu); }
static uint32_t enc_nop(void)                            { return 0x00000000u; }
static uint32_t enc_break(void)                          { return 0x0D; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm)    { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

int main(void)
{
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    iop_core_init(&bios);
    iop_state_t *st_setup = iop_core_get_state();

    /* JAL is a MIPS J-type (pseudo-direct) jump: the target address
     * is built from the CURRENT pc's upper 4 bits plus a 26-bit field
     * from the instruction - it can only reach addresses within the
     * same 256MB segment as the JAL instruction itself. Real IOP code
     * that calls the A0/B0/C0 traps runs from RAM (segment 0x000000xx/
     * 0x800000xx/0xA00000xx), which shares the same upper-nibble
     * segment as the trap addresses (0xA0/0xB0/0xC0, also near
     * address zero) - so a real JAL from RAM reaches them correctly.
     * Placing this test's caller code at the IOP's BIOS reset vector
     * (0xBFC00000, segment 0xB) instead would make JAL's pseudo-direct
     * target become 0xB00000A0, NOT 0xA0 - a test-setup mismatch, not
     * a bug in the trap handler - so this test writes its program into
     * IOP RAM (segment 0) and starts execution there instead, exactly
     * matching how real calling code is actually positioned. */
    uint8_t *p = st_setup->ram;
    int pc = 0;
    wle32(p+pc, enc_addiu(9, 0, 5)); pc += 4;      /* $t1 = 5 (function number) */
    wle32(p+pc, enc_jal(0xA0));      pc += 4;      /* JAL 0xA0 - the trap */
    wle32(p+pc, enc_nop());          pc += 4;      /* delay slot */
    /* execution should resume here (pc=12) if the trap correctly
     * simulated JR $ra */
    wle32(p+pc, enc_ori(10, 2, 0));  pc += 4;      /* $t2 = $v0 (copy, so BREAK doesn't disturb it) */
    wle32(p+pc, enc_break());        pc += 4;

    st_setup->pc = 0;
    st_setup->next_pc = 4;

    /* Run exactly 6 steps: ADDIU, JAL, NOP (branch delay slot), the
     * trapped call at 0xA0 (consumes one step but executes no real
     * MIPS instruction), ORI, BREAK. Using iop_core_step() directly
     * (rather than iop_core_run()) so we can inspect state
     * deterministically after a known number of instructions, same
     * style other tests use when step-level precision matters. */
    for (int i = 0; i < 6 && !g_iop.halted; i++)
        iop_core_step();

    iop_state_t *st = iop_core_get_state();
    iop_hle_bios_state_t *hle = iop_hle_bios_get_state();

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core reached and halted on BREAK (control flow returned correctly from the trap)");
    CHECK(hle->calls_seen == 1, "exactly one HLE BIOS call was recorded");
    CHECK(hle->last_table == IOP_HLE_TABLE_A0, "call was recorded against the A0 table");
    CHECK(hle->last_function == 5, "call's function number ($t1) was recorded correctly");
    CHECK(st->gpr[10] == 0, "$v0 read back as the generic default (0) after the trap, copied into $t2 before BREAK");

    /* Confirm the trap doesn't fire for addresses that aren't one of
     * the three vectors. */
    CHECK(iop_hle_bios_try_handle(st, 0x1000u) == 0, "non-trap address is not claimed");
    CHECK(iop_hle_bios_try_handle(st, IOP_HLE_TABLE_B0) == 1, "B0 table address is claimed");
    CHECK(hle->last_table == IOP_HLE_TABLE_B0, "second call correctly recorded against the B0 table");
    CHECK(hle->calls_seen == 2, "call counter incremented for the second (direct) call");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
