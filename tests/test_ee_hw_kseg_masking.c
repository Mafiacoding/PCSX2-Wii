/* test_ee_hw_kseg_masking.c - verifies real KSEG1/KSEG0 hardware-
 * register accesses (0xB000xxxx / 0x9000xxxx) actually reach the
 * dma_mmio_.../sif_mmio_.../mch_mmio_... dispatch in ee_core.c's
 * ee_mem_read32/write32, not just the bare-physical-looking literal
 * address the pre-existing test_ee_dma_bus.c happens to use.
 *
 * Found via round 11's live-trace investigation: real BIOS/game code
 * always addresses hardware registers through KSEG1 (uncached) or
 * KSEG0 (cached) mirrors, never through the raw 0x1000xxxx value as a
 * virtual address - but the mmio dispatch checks used to compare
 * against that raw, unmasked address directly, so they silently never
 * matched any real CPU-issued access. See ee_hw_mmio_addr() in
 * ee_core.c and docs/STATUS.md's "round 11" section.
 */
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include "core/ee/ee_core.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static uint32_t enc_lui(int rt, uint16_t imm) { return (0x0F << 26) | (rt << 16) | imm; }
static uint32_t enc_ori(int rt, int rs, uint16_t imm) { return (0x0D << 26) | (rs << 21) | (rt << 16) | imm; }
static uint32_t enc_sw(int rt, int rs, int16_t imm) { return (0x2B << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_lw(int rt, int rs, int16_t imm) { return (0x23 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_break(void) { return 0x0D; }
static void wle32(uint8_t *p, uint32_t v) { p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF; }
static uint32_t enc_sd(int rt, int rs, int16_t imm) { return (0x3F << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }
static uint32_t enc_ld(int rt, int rs, int16_t imm) { return (0x37 << 26) | (rs << 21) | (rt << 16) | (uint16_t)imm; }

/* Builds+runs a tiny program: r1=0xCAFEBABE; SW r1 -> hw_addr; LW r3 <- hw_addr; BREAK. */
static uint64_t run_roundtrip(uint32_t hw_addr)
{
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    uint8_t *p = bios.data;
    int pc = 0;
    wle32(p+pc, enc_lui(1, 0xCAFE)); pc += 4;
    wle32(p+pc, enc_ori(1, 1, 0xBABE)); pc += 4;
    wle32(p+pc, enc_lui(2, (hw_addr >> 16) & 0xFFFFu)); pc += 4;
    wle32(p+pc, enc_ori(2, 2, hw_addr & 0xFFFFu)); pc += 4;
    wle32(p+pc, enc_sw(1, 2, 0)); pc += 4;
    wle32(p+pc, enc_lw(3, 2, 0)); pc += 4;
    wle32(p+pc, enc_break()); pc += 4;

    ee_core_init(&bios);
    ee_core_run(&bios);
    ee_state_t *st = ee_core_get_state();
    return st->gpr[3].ud0;
}

int main(void)
{
    /* KSEG1 (uncached, 0xB000_xxxx) - the form real BIOS/game code
     * actually uses. SIF_MSCOM = 0x1000F200, so KSEG1 form = 0xB000F200. */
    uint64_t v = run_roundtrip(0xB000F200u);
    CHECK(v == (uint64_t)(int64_t)(int32_t)0xCAFEBABEu,
          "KSEG1 (0xB000F200) SW/LW round-trips through SIF MSCOM, not silently dropped into RAM-as-zero");

    sif_state_t *sif = sif_get_state();
    CHECK(sif->mscom == 0xCAFEBABEu, "SIF state actually got the write via the KSEG1 address");

    /* KSEG0 (cached, 0x9000_xxxx) mirror of the same register. */
    uint64_t v2 = run_roundtrip(0x9000F200u);
    CHECK(v2 == (uint64_t)(int64_t)(int32_t)0xCAFEBABEu,
          "KSEG0 (0x9000F200) SW/LW also round-trips through SIF MSCOM");

    /* And the new MCH registers, also via KSEG1 - the exact address
     * form the real BIOS boot path that motivated this fix uses
     * (0xB000F430/0xB000F440, see docs/STATUS.md's round 10/11). */
    {
        bios_image_t bios;
        memset(&bios, 0, sizeof(bios));
        bios.data = memalign(32, BIOS_MAX_SIZE);
        memset(bios.data, 0, BIOS_MAX_SIZE);
        bios.size = BIOS_MAX_SIZE;
        bios.loaded = 1;
        uint8_t *p = bios.data;
        int pc = 0;
        /* r1 = 0x00210000 (SA=0x21, SOP=0) */
        wle32(p+pc, enc_lui(1, 0x0021)); pc += 4;
        wle32(p+pc, enc_ori(1, 1, 0x0000)); pc += 4;
        /* r2 = 0xB000F430 (MCH_RICM, KSEG1) */
        wle32(p+pc, enc_lui(2, 0xB000)); pc += 4;
        wle32(p+pc, enc_ori(2, 2, 0xF430)); pc += 4;
        wle32(p+pc, enc_sw(1, 2, 0)); pc += 4; /* select SA=0x21 */
        /* r4 = 0xB000F440 (MCH_DRD, KSEG1) */
        wle32(p+pc, enc_lui(4, 0xB000)); pc += 4;
        wle32(p+pc, enc_ori(4, 4, 0xF440)); pc += 4;
        wle32(p+pc, enc_lw(5, 4, 0)); pc += 4; /* r5 = MCH_DRD read -> should be 0x1F */
        wle32(p+pc, enc_break()); pc += 4;

        ee_core_init(&bios);
        ee_core_run(&bios);
        ee_state_t *st = ee_core_get_state();
        CHECK((uint32_t)st->gpr[5].ud0 == 0x1Fu,
              "real BIOS-style KSEG1 access (0xB000F430/0xB000F440) reaches MCH and returns 0x1F");
    }

    /* Task #171/#172 (GS audit): GS privileged registers (PMODE/
     * DISPFB/DISPLAY, 0x12000000-0x12001FFF) are only reachable via
     * 64-bit LD/SD, and ee_mem_read64/write64 were found (this
     * session's GS-path review) to be missing the same
     * ee_hw_mmio_addr() masking the 32-bit path above already has -
     * meaning a real KSEG1-mirrored SD to DISPFB1 (0xB2000070) would
     * have silently missed gs_mmio_write64() entirely. Verifies the
     * fix the same way the SIF/MCH cases above verify theirs. */
    {
        bios_image_t bios;
        memset(&bios, 0, sizeof(bios));
        bios.data = memalign(32, BIOS_MAX_SIZE);
        memset(bios.data, 0, BIOS_MAX_SIZE);
        bios.size = BIOS_MAX_SIZE;
        bios.loaded = 1;
        uint8_t *p = bios.data;
        int pc = 0;
        /* r1 = 0x00001234 (an arbitrary nonzero DISPFB1-shaped value) */
        wle32(p+pc, enc_lui(1, 0x0000)); pc += 4;
        wle32(p+pc, enc_ori(1, 1, 0x1234)); pc += 4;
        /* r2 = 0xB2000070 (DISPFB1, KSEG1 mirror) */
        wle32(p+pc, enc_lui(2, 0xB200)); pc += 4;
        wle32(p+pc, enc_ori(2, 2, 0x0070)); pc += 4;
        wle32(p+pc, enc_sd(1, 2, 0)); pc += 4;
        wle32(p+pc, enc_ld(3, 2, 0)); pc += 4;
        wle32(p+pc, enc_break()); pc += 4;

        ee_core_init(&bios);
        ee_core_run(&bios);
        ee_state_t *st = ee_core_get_state();
        CHECK((uint64_t)st->gpr[3].ud0 == 0x1234u,
              "KSEG1 (0xB2000070) SD/LD round-trips through GS DISPFB1, not silently dropped");
        gs_state_t *gs = gs_get_state();
        CHECK(gs->dispfb1 == 0x1234u,
              "GS state actually got the write via the KSEG1 address");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
