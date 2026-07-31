/* Host-native test for Round 185's real SPU2 register offset table -
 * verifies the offset arithmetic matches the cited real layout and
 * that named registers read back what was written (plain passthrough,
 * no synthesis - matching this file's honest scope). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "hw/iop_spu2.c"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)

int main(void)
{
    iop_spu2_init();

    /* 1. Core0/Core1 base offset arithmetic (ps2tek/ZeroSPU2-cited +0x400). */
    CHECK(iop_spu2_core_reg_addr(0, SPU2_C_MMIX) == IOP_SPU2_BASE + SPU2_C_MMIX,
          "core0 MMIX address");
    CHECK(iop_spu2_core_reg_addr(1, SPU2_C_MMIX) == IOP_SPU2_BASE + SPU2_C_MMIX + SPU2_CORE1_OFFSET,
          "core1 MMIX address is core0 + 0x400");

    /* 2. Per-voice stride (0x10) - voice 0 vs voice 1 VOLL. */
    uint32_t v0_voll = iop_spu2_voice_reg_addr(0, 0, SPU2_V_VOLL);
    uint32_t v1_voll = iop_spu2_voice_reg_addr(0, 1, SPU2_V_VOLL);
    CHECK(v1_voll - v0_voll == SPU2_VOICE_STRIDE, "voice stride is 0x10");
    CHECK(v0_voll == IOP_SPU2_BASE, "voice 0 VOLL is at core base + 0");

    /* 3. Per-voice-address block stride (0x0C) at base+0x1C0. */
    uint32_t v0_ssa = iop_spu2_voice_addr_reg_addr(0, 0, SPU2_VA_SSA_HI);
    uint32_t v1_ssa = iop_spu2_voice_addr_reg_addr(0, 1, SPU2_VA_SSA_HI);
    CHECK(v0_ssa == IOP_SPU2_BASE + SPU2_VADDR_BASE, "voice 0 SSA at core base + 0x1C0");
    CHECK(v1_ssa - v0_ssa == SPU2_VADDR_STRIDE, "voice address stride is 0x0C");

    /* 4. Named registers are real, distinct, in-range read/write
     * passthrough (no behavior change from the pre-existing scaffold). */
    uint32_t addrs[] = {
        iop_spu2_voice_reg_addr(0, 0, SPU2_V_VOLL),
        iop_spu2_voice_reg_addr(0, 23, SPU2_V_VOLXR), /* last voice, last field */
        iop_spu2_core_reg_addr(0, SPU2_C_KON0),
        iop_spu2_core_reg_addr(1, SPU2_C_ENDX0),
        iop_spu2_core_reg_addr(0, SPU2_C_ADMAS),
        IOP_SPU2_BASE + SPU2_S_MVOLL,
        IOP_SPU2_BASE + SPU2_S_MVOLR,
    };
    for (int i = 0; i < 7; i++) {
        CHECK(addrs[i] >= IOP_SPU2_BASE && addrs[i] < IOP_SPU2_BASE + IOP_SPU2_SIZE,
              "named register address falls within the modeled 0x800-byte window");
        uint16_t wv = (uint16_t)(0x1000 + i);
        CHECK(iop_spu2_mmio_write16(addrs[i], wv) == 1, "write to named register succeeds");
        uint16_t rv = 0;
        CHECK(iop_spu2_mmio_read16(addrs[i], &rv) == 1, "read from named register succeeds");
        CHECK(rv == wv, "named register readback matches what was written");
    }

    /* 5. Last voice (23) + last per-voice field (VOLXR) must still be
     * within Core0's 0x400-byte window (real hardware: 24 voices *
     * 0x10 = 0x180, well within 0x400) and distinct from Core1. */
    uint32_t last_voice_reg = iop_spu2_voice_reg_addr(0, 23, SPU2_V_VOLXR);
    CHECK(last_voice_reg < IOP_SPU2_BASE + SPU2_CORE1_OFFSET,
          "voice 23's last field stays inside Core0's block, doesn't bleed into Core1");

    /* 6. Shared registers (MVOLL/MVOLR) are NOT offset by core -
     * single instance, per the citation. */
    CHECK((IOP_SPU2_BASE + SPU2_S_MVOLL) == 0x1F900760u, "MVOLL at the real cited absolute offset 0x760");

    if (g_fail == 0)
        printf("ALL CHECKS PASSED (0 failures)\n");
    else
        printf("%d check(s) FAILED\n", g_fail);
    return g_fail != 0;
}
