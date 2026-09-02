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

/* Task #735 test-harness compatibility helper: BREAK now raises a
 * genuine Breakpoint exception (ExcCode 9, see iop_core.c's SPECIAL
 * funct 0x0D case) instead of unconditionally halting the emulated
 * core - real R3000A/MIPS I hardware never stops executing just
 * because it hit a BREAK. This project's existing IOP test suite
 * used a trailing BREAK + st->halted as a convenient "run to
 * completion, then inspect final state" marker, and several tests
 * (e.g. test_iop_hw_interrupt.c) specifically inspect Cause/EPC/
 * Status left behind by an EARLIER, already-tested real exception (a
 * hardware interrupt) - a naive "run until the Breakpoint exception
 * fires" replacement would let BREAK's own exception delivery
 * silently overwrite exactly that state right before the test could
 * inspect it.
 *
 * This drop-in replacement for iop_core_run() therefore steps
 * normally (preserving the pre-existing syscall-recovery special
 * case inside the BREAK handler, task #156, completely unaffected)
 * and detects, AFTER each step, the precise signature of "a BREAK
 * instruction was just reached and, since it was NOT an unresolved
 * syscall, took the new real-Breakpoint-exception path" - namely
 * exception_pending && Cause.ExcCode==9 && EPC == the pc this step
 * started at. When that fires, it restores COP0 Status/Cause/EPC,
 * pc/next_pc, and instructions_executed to their values from
 * immediately BEFORE this step, undoing the exception delivery, then
 * synthesizes the exact same st->halted=1 / halt_reason convention
 * the old unconditional-halt code produced. The net effect is
 * bit-for-bit identical to the old halt("BREAK") timing: execution
 * freezes in exactly the state that existed the instant BREAK was
 * decoded, with none of BREAK's own (now real, but not wanted by
 * these specific tests) side effects applied. Mirrors ee_core.c's
 * task #178 run_until_break() test helper in spirit, adapted for the
 * IOP's different COP0 layout (no Status.EXL - R3000A pre-dates that
 * MIPS III addition) and for the pre-existing syscall-recovery
 * special case this file's BREAK handler still has to keep working
 * through. */
static void iop_run_until_break(void) {
    iop_state_t *st = iop_core_get_state();
    long guard;
    for (guard = 0; guard < 2000000L; guard++) {
        if (st->halted) return;
        uint32_t pre_pc = st->pc;
        uint32_t pre_next_pc = st->next_pc;
        uint32_t pre_status = st->cop0[12];
        uint32_t pre_cause = st->cop0[13];
        uint32_t pre_epc = st->cop0[14];
        uint8_t  pre_pending = st->exception_pending;
        uint64_t pre_count = st->instructions_executed;

        if (iop_step()) return; /* genuine halt - not a BREAK, leave as-is */

        if (st->exception_pending && (st->cop0[13] & 0x7Cu) == 0x24u && st->cop0[14] == pre_pc) {
            st->cop0[12] = pre_status;
            st->cop0[13] = pre_cause;
            st->cop0[14] = pre_epc;
            st->exception_pending = pre_pending;
            st->pc = pre_pc;
            st->next_pc = pre_next_pc;
            st->instructions_executed = pre_count;
            st->halted = 1;
            snprintf(st->halt_reason, sizeof(st->halt_reason),
                     "BREAK (task #735: real Breakpoint exception raised and unwound by the test harness, ExcCode 9)");
            return;
        }
    }
    st->halted = 1;
    snprintf(st->halt_reason, sizeof(st->halt_reason),
             "iop_run_until_break() safety cap reached without a Breakpoint exception");
}

static uint32_t enc_lui(int rt, uint16_t imm) { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_addiu(int rt, int rs, int16_t imm) { return (0x09 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_sw(int rt, int rs, int16_t imm) { return (0x2B << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_lwl(int rt, int rs, int16_t imm) { return (0x22 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_lwr(int rt, int rs, int16_t imm) { return (0x26 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_break(void) { return 0x0D; }

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    uint8_t *p = bios.data;
    int pc = 0;
    /* r1 = 0x11223344 (RAM base addr holder), store into RAM at addr 0x100 via r2=0 base */
    wle32(p+pc, enc_lui(1, 0x1122)); pc+=4;
    wle32(p+pc, enc_ori(1, 1, 0x3344)); pc+=4;
    wle32(p+pc, enc_sw(1, 0, 0x100)); pc+=4;       /* MEM[0x100] = 0x11223344 (LE bytes: 44 33 22 11) */
    wle32(p+pc, enc_lwl(2, 0, 0x103)); pc+=4;       /* LWL r2, 0x103(r0): addr=0x103, unaligned */
    wle32(p+pc, enc_lwr(2, 0, 0x100)); pc+=4;       /* LWR r2, 0x100(r0) completes it -> r2 should == 0x11223344 */
    wle32(p+pc, enc_break()); pc+=4;

    iop_core_init(&bios);
    iop_run_until_break();
    iop_state_t *st = iop_core_get_state();

    CHECK(st->gpr[1] == 0x11223344u, "LUI+ORI built r1 = 0x11223344");
    uint32_t stored = iop_mem_read32(st, 0x100);
    CHECK(stored == 0x11223344u, "SW stored r1 correctly (little-endian roundtrip)");
    CHECK(st->gpr[2] == 0x11223344u, "LWL+LWR reconstruct unaligned word correctly");
    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL, "halted cleanly on BREAK");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
