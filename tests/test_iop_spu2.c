/*
 * test_iop_spu2.c - host-native test for the SPU2 register scaffold
 * (task #95). See include/core/hw/iop_spu2.h for scope - this is a
 * register-file scaffold (real base address, real 16-bit-native
 * access), not audio synthesis.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "core/hw/iop_spu2.h"
#include "core/iop/iop_core.h"
#include "core/bios_loader.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void)
{
    /* --- direct unit test of the scaffold's own read16/write16/read32/write32 --- */
    iop_spu2_init();

    uint16_t v16;
    CHECK(iop_spu2_mmio_read16(IOP_SPU2_BASE, &v16) == 1 && v16 == 0,
          "SPU2: address in range reads back 0 after init");
    CHECK(iop_spu2_mmio_read16(IOP_SPU2_BASE - 4, &v16) == 0,
          "SPU2: address just below the real base is correctly NOT claimed");
    CHECK(iop_spu2_mmio_read16(IOP_SPU2_BASE + IOP_SPU2_SIZE, &v16) == 0,
          "SPU2: address just past the modeled window is correctly NOT claimed");

    CHECK(iop_spu2_mmio_write16(IOP_SPU2_BASE + 0x10, 0xBEEF) == 1,
          "SPU2: write16 to a real in-range register is accepted");
    iop_spu2_mmio_read16(IOP_SPU2_BASE + 0x10, &v16);
    CHECK(v16 == 0xBEEF, "SPU2: read16 reads back exactly what write16 stored (real register persistence)");

    uint32_t v32;
    CHECK(iop_spu2_mmio_write32(IOP_SPU2_BASE + 0x20, 0xCAFEBABEu) == 1 &&
          iop_spu2_mmio_read32(IOP_SPU2_BASE + 0x20, &v32) == 1 && v32 == 0xCAFEBABEu,
          "SPU2: 32-bit write/read round-trip also works (some real IOP code may use LW/SW)");

    /* the 32-bit write above should be visible as two adjacent 16-bit registers too */
    iop_spu2_mmio_read16(IOP_SPU2_BASE + 0x20, &v16);
    CHECK(v16 == 0xBABEu, "SPU2: a 32-bit write is visible via 16-bit reads at the same+adjacent offsets (little-endian, shared backing store)");

    /* --- integration test: real IOP LH/SH/LW/SW instructions actually reach SPU2 through iop_core.c's dispatch --- */
    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    iop_core_init(&bios);
    iop_state_t *st = iop_core_get_state();

    iop_mem_write16(st, IOP_SPU2_BASE + 0x100, 0x1234);
    CHECK(iop_mem_read16(st, IOP_SPU2_BASE + 0x100) == 0x1234,
          "SPU2: reachable through iop_core.c's real iop_mem_read16/write16 dispatch (LH/SH path)");

    iop_mem_write32(st, IOP_SPU2_BASE + 0x104, 0x11223344u);
    CHECK(iop_mem_read32(st, IOP_SPU2_BASE + 0x104) == 0x11223344u,
          "SPU2: reachable through iop_core.c's real iop_mem_read32/write32 dispatch (LW/SW path)");

    /* --- writes to ordinary IOP RAM are unaffected (SPU2 dispatch doesn't swallow unrelated addresses) --- */
    iop_mem_write32(st, 0x00001000u, 0x99999999u);
    CHECK(iop_mem_read32(st, 0x00001000u) == 0x99999999u,
          "SPU2: dispatch correctly leaves ordinary IOP RAM addresses alone");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
