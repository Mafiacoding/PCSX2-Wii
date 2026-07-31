#include <stdio.h>
#include <string.h>
#include "hw/gs_mem.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    gs_mem_init();

    gs_mem_write_psmct32(0, 640, 10, 20, 0xFF00FF00u);
    CHECK(gs_mem_read_psmct32(0, 640, 10, 20) == 0xFF00FF00u, "pixel roundtrip at (10,20)");
    CHECK(gs_mem_read_psmct32(0, 640, 11, 20) == 0, "adjacent pixel (11,20) untouched");
    CHECK(gs_mem_read_psmct32(0, 640, 10, 21) == 0, "adjacent pixel (10,21) untouched");

    gs_mem_write_psmct32(0, 640, 0, 0, 0x11223344u);
    gs_mem_write_psmct32(0, 640, 639, 0, 0x55667788u);
    CHECK(gs_mem_read_psmct32(0, 640, 0, 0) == 0x11223344u, "top-left corner of scanline 0");
    CHECK(gs_mem_read_psmct32(0, 640, 639, 0) == 0x55667788u, "last pixel of scanline 0 (bw-1)");
    CHECK(gs_mem_read_psmct32(0, 640, 0, 1) != 0x11223344u, "scanline 1 doesn't alias scanline 0");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
