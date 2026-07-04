/*
 * test_system_handshake.c - host-native test for the interleaved
 * EE/IOP scheduler (source/core/system.c) AND the IOP-side SIF mirror
 * (sif_iop_mmio_read32/write32 in source/hw/sif.c).
 *
 * This is the first test in the project that actually proves two
 * separate CPU cores exchanging data through shared hardware state -
 * everything before this tested one core (or one hardware model) in
 * isolation. It hand-encodes a small MIPS program for EACH core that
 * performs a real (if minimal) SIF mailbox handshake:
 *
 *   EE:  MSCOM = 0x1234ABCD
 *        MSFLAG |= 1                  (signal: "message ready")
 *        poll SMFLAG until bit 0 set  (wait for IOP's reply)
 *        r9 = SMCOM                   (read IOP's echoed reply)
 *        BREAK
 *
 *   IOP: poll MSFLAG until bit 0 set  (wait for EE's message)
 *        r5 = MSCOM                   (read EE's message)
 *        SMCOM = r5                   (echo it back)
 *        SMFLAG |= 1  (IOP side write is plain/flat - see sif.h - so
 *                       this is really "SMFLAG = 1", not an OR, but
 *                       the effect is the same starting from zero)
 *        BREAK
 *
 * Neither side can complete alone: the EE's poll loop only exits once
 * the IOP has actually executed its half, and the IOP's poll loop
 * only exits once the EE has executed its half. If system_init()/
 * system_run_interleaved() didn't really interleave both cores (e.g.
 * if it silently only ran one), this test would hang against the
 * slice cap and fail cleanly instead of deadlocking, since the cap
 * makes system_run_interleaved() return 0 rather than loop forever.
 *
 * Uses two independent fake BIOS images (one per core) purely for
 * test convenience - real hardware shares one physical ROM and the
 * two CPUs diverge based on which one is fetching, which is out of
 * scope to model here (see main.c, which does share one real bios
 * image between both system_init() arguments for the actual boot
 * path - this test just needs independent instruction streams).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

#include "core/system.c"
#include "core/hw/sif.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t enc_lui(int rt, uint16_t imm)             { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm)      { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_andi(int rt, int rs, uint16_t imm)     { return (0x0C << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_addiu(int rt, int rs, int16_t imm)     { return (0x09 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_sw(int rt, int rs, int16_t imm)        { return (0x2B << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_lw(int rt, int rs, int16_t imm)        { return (0x23 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_beq(int rs, int rt, int16_t imm)       { return (0x04 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_nop(void)                              { return 0x00000000u; }
static uint32_t enc_break(void)                             { return 0x0D; }

static void wle32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

int main(void)
{
    /* --- EE program: BIOS_RESET_VECTOR = 0xBFC00000, offset 0 --- */
    bios_image_t ee_bios;
    memset(&ee_bios, 0, sizeof(ee_bios));
    ee_bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(ee_bios.data, 0, BIOS_MAX_SIZE);
    ee_bios.size = BIOS_MAX_SIZE;
    ee_bios.loaded = 1;

    {
        uint8_t *p = ee_bios.data;
        int pc = 0;
        /* r1 = 0x1000F200 (MSCOM) */
        wle32(p+pc, enc_lui(1, 0x1000)); pc += 4;
        wle32(p+pc, enc_ori(1, 1, 0xF200)); pc += 4;
        /* r2 = 0x1234 (payload) */
        wle32(p+pc, enc_addiu(2, 0, 0x1234)); pc += 4;
        /* MSCOM = r2 */
        wle32(p+pc, enc_sw(2, 1, 0)); pc += 4;
        /* r3 = 0x1000F220 (MSFLAG) */
        wle32(p+pc, enc_lui(3, 0x1000)); pc += 4;
        wle32(p+pc, enc_ori(3, 3, 0xF220)); pc += 4;
        /* r4 = 1; MSFLAG |= r4 (real OR semantics on EE side) */
        wle32(p+pc, enc_addiu(4, 0, 1)); pc += 4;
        wle32(p+pc, enc_sw(4, 3, 0)); pc += 4;
        /* r5 = 0x1000F230 (SMFLAG) - poll target */
        wle32(p+pc, enc_lui(5, 0x1000)); pc += 4;
        wle32(p+pc, enc_ori(5, 5, 0xF230)); pc += 4;
        /* poll_loop: */
        int poll_loop_pc = pc;
        wle32(p+pc, enc_lw(6, 5, 0)); pc += 4;      /* r6 = SMFLAG */
        wle32(p+pc, enc_andi(7, 6, 1)); pc += 4;     /* r7 = r6 & 1 */
        int beq_pc = pc;
        wle32(p+pc, enc_beq(7, 0, 0)); pc += 4;       /* placeholder imm, patched below */
        wle32(p+pc, enc_nop()); pc += 4;              /* delay slot */
        /* patch BEQ's branch offset: target = poll_loop_pc */
        {
            int32_t off_words = (poll_loop_pc - (beq_pc + 4)) / 4;
            wle32(p+beq_pc, enc_beq(7, 0, (int16_t)off_words));
        }
        /* r8 = 0x1000F210 (SMCOM) */
        wle32(p+pc, enc_lui(8, 0x1000)); pc += 4;
        wle32(p+pc, enc_ori(8, 8, 0xF210)); pc += 4;
        /* r9 = SMCOM (IOP's reply) */
        wle32(p+pc, enc_lw(9, 8, 0)); pc += 4;
        wle32(p+pc, enc_break()); pc += 4;
    }

    /* --- IOP program: IOP_RESET_VECTOR = 0xBFC00000, offset 0 (own image) --- */
    bios_image_t iop_bios;
    memset(&iop_bios, 0, sizeof(iop_bios));
    iop_bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(iop_bios.data, 0, BIOS_MAX_SIZE);
    iop_bios.size = BIOS_MAX_SIZE;
    iop_bios.loaded = 1;

    {
        uint8_t *p = iop_bios.data;
        int pc = 0;
        /* r1 = 0x1D000020 (MSFLAG mirror) - poll target */
        wle32(p+pc, enc_lui(1, 0x1D00)); pc += 4;
        wle32(p+pc, enc_ori(1, 1, 0x0020)); pc += 4;
        int poll_loop_pc = pc;
        wle32(p+pc, enc_lw(2, 1, 0)); pc += 4;      /* r2 = MSFLAG */
        wle32(p+pc, enc_andi(3, 2, 1)); pc += 4;     /* r3 = r2 & 1 */
        int beq_pc = pc;
        wle32(p+pc, enc_beq(3, 0, 0)); pc += 4;       /* placeholder, patched below */
        wle32(p+pc, enc_nop()); pc += 4;              /* delay slot */
        {
            int32_t off_words = (poll_loop_pc - (beq_pc + 4)) / 4;
            wle32(p+beq_pc, enc_beq(3, 0, (int16_t)off_words));
        }
        /* r4 = 0x1D000000 (MSCOM mirror) */
        wle32(p+pc, enc_lui(4, 0x1D00)); pc += 4;
        wle32(p+pc, enc_ori(4, 4, 0x0000)); pc += 4;
        wle32(p+pc, enc_lw(5, 4, 0)); pc += 4;       /* r5 = MSCOM (EE's message) */
        /* r6 = 0x1D000010 (SMCOM mirror) */
        wle32(p+pc, enc_lui(6, 0x1D00)); pc += 4;
        wle32(p+pc, enc_ori(6, 6, 0x0010)); pc += 4;
        wle32(p+pc, enc_sw(5, 6, 0)); pc += 4;       /* SMCOM = r5 (echo) */
        /* r7 = 0x1D000030 (SMFLAG mirror) */
        wle32(p+pc, enc_lui(7, 0x1D00)); pc += 4;
        wle32(p+pc, enc_ori(7, 7, 0x0030)); pc += 4;
        wle32(p+pc, enc_addiu(8, 0, 1)); pc += 4;
        wle32(p+pc, enc_sw(8, 7, 0)); pc += 4;       /* SMFLAG = 1 (plain write, IOP side) */
        wle32(p+pc, enc_break()); pc += 4;
    }

    CHECK(system_init(&ee_bios, &iop_bios) == 0, "system_init succeeds with two independent images");

    /* Slice cap chosen generously (each side's program is well under
     * 100 instructions and the poll loops only need to spin a few
     * times waiting for the other side) - if the scheduler didn't
     * really interleave, this would hit the cap and return 0 instead
     * of both cores halting cleanly. */
    int both_halted = system_run_interleaved(2000);
    CHECK(both_halted == 1, "both cores halted before the slice cap (real interleaving happened)");

    ee_state_t  *ee  = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();
    sif_state_t *sif = sif_get_state();

    CHECK(ee->halted == 1 && strstr(ee->halt_reason, "BREAK") != NULL,
          "EE core halted cleanly on BREAK");
    CHECK(iop->halted == 1 && strstr(iop->halt_reason, "BREAK") != NULL,
          "IOP core halted cleanly on BREAK");

    CHECK(sif->mscom == 0x1234u, "EE's MSCOM write landed in shared SIF state");
    CHECK((sif->msflag & 1u) == 1u, "EE's MSFLAG OR-write set bit 0");
    CHECK(sif->smcom == 0x1234u, "IOP read MSCOM and echoed the SAME value into SMCOM");
    CHECK((sif->smflag & 1u) == 1u, "IOP's SMFLAG write set bit 0");

    CHECK(ee->gpr[9].ud0 == 0x1234u,
          "EE polled SMFLAG, then read SMCOM and got the IOP's echoed value back - full round trip");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
