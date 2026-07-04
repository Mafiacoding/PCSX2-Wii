/*
 * test_iop_hle_exception_install.c - host-native test for
 * iop_hle_bios.c's InstallExceptionHandlers (C0h/0x07) real behavior
 * (see the header comment in include/core/hw/iop_hle_bios.h for the
 * full rationale - this was added after real-BIOS testing showed the
 * IOP's generic default-return HLE stub was leaving the hardware
 * exception vector at RAM address 0x80 empty, causing a later halt).
 *
 * This test builds a SYNTHETIC BIOS image (no real BIOS bytes) that
 * embeds the well-known, distinctive 16-byte exception-handler
 * trampoline signature (LUI $k0,0 / ADDIU $k0,$k0,imm / JR $k0 / NOP)
 * at an arbitrary offset, and confirms that triggering a C0h/0x07 HLE
 * call locates it and writes it, verbatim, to RAM address 0x80 (and
 * mirrors it to address 0).
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
static uint32_t enc_jal(uint32_t target)   { return (0x03 << 26) | ((target >> 2) & 0x03FFFFFFu); }
static uint32_t enc_break(void)            { return 0x0D; }
static uint32_t enc_nop(void)              { return 0x00000000u; }

static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }

int main(void) {
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = calloc(1, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;
    strncpy(bios.name, "synthetic-test-bios", sizeof(bios.name) - 1);

    /* Embed the real trampoline signature at an arbitrary ROM offset,
     * with a deliberately distinctive jump-target immediate (0x0ABC)
     * to prove the actual found bytes get used, not some hardcoded
     * guess. */
    uint32_t sig_off = 0x12340;
    wle32(bios.data + sig_off + 0,  0x3C1A0000u);            /* LUI $k0,0x0000 */
    wle32(bios.data + sig_off + 4,  0x275A0ABCu);             /* ADDIU $k0,$k0,0x0ABC */
    wle32(bios.data + sig_off + 8,  0x03400008u);             /* JR $k0 */
    wle32(bios.data + sig_off + 12, 0x00000000u);             /* NOP */

    /* Test program in IOP RAM (segment 0, same JAL-segment-addressing
     * reasoning as test_iop_hle_bios.c), but placed at 0x1000 rather
     * than address 0 - the low addresses 0x00-0x0F (Garbage Area
     * mirror) and 0x80-0x8F (exception vector) are exactly what this
     * feature writes to, so a test program placed there would get
     * its own instructions overwritten by the install. 0x1000 is
     * comfortably clear of every reserved region in the documented
     * BIOS RAM map (see the header comment / psx-spx). Program:
     * set $t1=7, JAL 0x000000C0, delay-slot NOP, then BREAK to
     * halt. */
    uint32_t prog_base = 0x1000u;
    uint8_t *prog = calloc(1, 0x100);
    wle32(prog + 0x00, enc_addiu(9, 0, 0x07));  /* $t1 = 7 (InstallExceptionHandlers) */
    wle32(prog + 0x04, enc_jal(0x000000C0));
    wle32(prog + 0x08, enc_nop());              /* JAL delay slot */
    wle32(prog + 0x0C, enc_break());

    if (iop_core_init(&bios) != 0) { printf("iop_core_init failed\n"); return 1; }
    iop_state_t *st = iop_core_get_state();

    memcpy(st->ram + prog_base, prog, 0x100);
    st->pc = prog_base;
    st->next_pc = prog_base + 4;

    iop_hle_bios_state_t *hle = iop_hle_bios_get_state();
    CHECK(hle->exception_handler_template_found == 0,
          "template not yet scanned/found before any C0h/0x07 call happens");
    CHECK(hle->exception_handler_installed == 0,
          "exception handler not yet installed before any C0h/0x07 call happens");

    iop_core_run();

    CHECK(st->halted == 1 && strstr(st->halt_reason, "BREAK") != NULL,
          "core halted cleanly on BREAK after the call returned");
    CHECK(hle->exception_handler_template_found == 1,
          "the real 16-byte signature was located in the loaded BIOS image");
    CHECK(hle->exception_handler_installed == 1,
          "InstallExceptionHandlers wrote the real template to the exception vector");

    uint32_t w0 = iop_mem_read32(st, 0x80);
    uint32_t w1 = iop_mem_read32(st, 0x84);
    uint32_t w2 = iop_mem_read32(st, 0x88);
    uint32_t w3 = iop_mem_read32(st, 0x8C);
    CHECK(w0 == 0x3C1A0000u, "word 0 at the exception vector (0x80) matches the real ROM bytes (LUI $k0,0)");
    CHECK(w1 == 0x275A0ABCu, "word 1 at the exception vector uses this dump's ACTUAL jump-target immediate (0x0ABC), not a hardcoded guess");
    CHECK(w2 == 0x03400008u, "word 2 at the exception vector matches (JR $k0)");
    CHECK(w3 == 0x00000000u, "word 3 at the exception vector matches (NOP delay slot)");

    uint32_t m0 = iop_mem_read32(st, 0x00);
    uint32_t m1 = iop_mem_read32(st, 0x04);
    uint32_t m2 = iop_mem_read32(st, 0x08);
    uint32_t m3 = iop_mem_read32(st, 0x0C);
    CHECK(m0 == w0 && m1 == w1 && m2 == w2 && m3 == w3,
          "the same 16 bytes are mirrored to address 0 (psx-spx's documented \"Garbage Area\" echo)");

    /* Function numbers other than 0x07 on the C0 table must NOT
     * trigger an install - this is a narrow, specific exception, not
     * a blanket behavior change for the whole table. */
    iop_core_init(&bios);
    st = iop_core_get_state();
    memcpy(st->ram + prog_base, prog, 0x100);
    wle32(st->ram + prog_base, enc_addiu(9, 0, 0x08)); /* different function number */
    st->pc = prog_base;
    st->next_pc = prog_base + 4;
    iop_core_run();
    hle = iop_hle_bios_get_state();
    CHECK(hle->exception_handler_installed == 0,
          "a different C0-table function number (0x08) does NOT trigger the install");

    printf("\n%d check(s) failed\n", failures);
    return failures != 0;
}
