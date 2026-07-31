#include <stdio.h>
#include <string.h>
#include "hw/gs.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    gs_init();
    uint64_t v;

    CHECK(gs_mmio_write64(0x12000070u, 0x123456789ABCDEFull) == 1, "write to DISPFB1 handled");
    CHECK(gs_mmio_read64(0x12000070u, &v) == 1 && v == 0x123456789ABCDEFull, "DISPFB1 roundtrip");

    CHECK(gs_mmio_write64(0x120000E0u, 0xAABBCCull) == 1, "write to BGCOLOR handled");
    CHECK(gs_mmio_read64(0x120000E0u, &v) == 1 && v == 0xAABBCCull, "BGCOLOR roundtrip");

    /* CSR write-1-to-clear: set some status bits, then write those
     * same bits back and confirm they clear instead of just being
     * stored verbatim (which would be the plain-register bug). */
    CHECK(gs_get_state()->csr == 0, "CSR starts at 0");
    gs_get_state()->csr = 0x1F; /* simulate some interrupt status bits being set */
    CHECK(gs_mmio_write64(0x12001000u, 0x05) == 1, "write to CSR handled");
    CHECK(gs_mmio_read64(0x12001000u, &v) == 1 && v == 0x1A, "CSR write-1-to-clear cleared exactly bits 0x05 (0x1F & ~0x05 = 0x1A)");

    /* GS_IMR is a plain register, not write-1-to-clear */
    CHECK(gs_mmio_write64(0x12001010u, 0xFF) == 1, "write to IMR handled");
    CHECK(gs_mmio_read64(0x12001010u, &v) == 1 && v == 0xFF, "IMR is a plain read/write register (not W1C)");

    /* address outside the GS privileged range is unclaimed */
    CHECK(gs_mmio_read64(0x11FFFFFFu, &v) == 0, "address just below GS range unclaimed");
    CHECK(gs_mmio_read64(0x12002000u, &v) == 0, "address just above GS range unclaimed");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
