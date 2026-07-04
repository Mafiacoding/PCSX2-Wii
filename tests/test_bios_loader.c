/*
 * test_bios_loader.c - host-native test for source/core/bios_loader.c
 *
 * IMPORTANT: this test uses an entirely SYNTHETIC, hand-built ROMDIR
 * structure - it does NOT embed or reference any real PS2 BIOS bytes
 * (those are copyrighted Sony firmware, never committed to this repo
 * - see docs/STATUS.md and data/pcsx2/bios/README.txt). The bug this
 * test guards against was found using a real, legally-dumped BIOS
 * image (SCPH-10000) provided directly by this project's user for
 * local testing only, but the test fixture below is fabricated to
 * exercise the same structural pattern without shipping any of that
 * copyrighted data.
 *
 * The bug: an earlier version of bios_loader.c assumed the ROMDIR
 * table always lives at a fixed file offset (0x100). That's wrong -
 * the real SCPH-10000 dump has it at file offset 0x2700. This test's
 * fixture deliberately places its synthetic ROMDIR table at YET
 * ANOTHER offset (wherever the RESET entry's declared size happens
 * to put it - see below), specifically NOT 0x100 and NOT 0x2700, to
 * prove the fix is a genuine scan rather than a second hardcoded
 * offset that happens to match one known real dump.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

#include "core/bios_loader.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else printf("ok:   %s\n", msg); \
} while (0)

static void write_entry(uint8_t *buf, uint32_t off, const char *name, uint16_t extinfo, uint32_t size)
{
    uint8_t *e = buf + off;
    memset(e, 0, ROMDIR_ENTRY_SZ);
    memcpy(e, name, strlen(name)); /* remaining name bytes stay zero-padded */
    e[10] = (uint8_t)(extinfo & 0xFF);
    e[11] = (uint8_t)((extinfo >> 8) & 0xFF);
    e[12] = (uint8_t)(size & 0xFF);
    e[13] = (uint8_t)((size >> 8) & 0xFF);
    e[14] = (uint8_t)((size >> 16) & 0xFF);
    e[15] = (uint8_t)((size >> 24) & 0xFF);
}

int main(void)
{
    const uint32_t buf_size = 4096;
    uint8_t *buf = malloc(buf_size);
    memset(buf, 0, buf_size);

    /* Synthetic module layout (sizes chosen to be unlike any real
     * BIOS revision's numbers, and to exercise non-16-aligned-size
     * rounding via EXTINFO's size of 20):
     *   RESET   @ 0    size 48  (already 16-aligned)
     *   ROMDIR  @ 48   size 64  (= 4 entries x 16 bytes: RESET/
     *                            ROMDIR/EXTINFO/ROMVER - the table's
     *                            own bytes ARE this entry's payload,
     *                            which is why the table always ends
     *                            up positioned exactly at the
     *                            preceding entry's aligned size)
     *   EXTINFO @ 112  size 20  (aligned up to 32)
     *   ROMVER  @ 144  size 16
     */
    const uint32_t table_off = 48;
    write_entry(buf, table_off + 0 * ROMDIR_ENTRY_SZ, "RESET",   0, 48);
    write_entry(buf, table_off + 1 * ROMDIR_ENTRY_SZ, "ROMDIR",  0, 64);
    write_entry(buf, table_off + 2 * ROMDIR_ENTRY_SZ, "EXTINFO", 0, 20);
    write_entry(buf, table_off + 3 * ROMDIR_ENTRY_SZ, "ROMVER",  0, 16);
    /* entry 4 (offset table_off + 64) stays all-zero: the empty-name terminator */

    const char *fake_version = "0099ZZ99999999\n"; /* 15 chars + implicit NUL padding to 16 */
    memcpy(buf + 144, fake_version, strlen(fake_version));

    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = buf;
    bios.size = buf_size;
    bios.loaded = 1;

    uint32_t found_table_off;
    CHECK(find_romdir_table(&bios, &found_table_off) == 1, "ROMDIR table located via RESET+ROMDIR signature scan");
    CHECK(found_table_off == table_off, "located table offset matches where it was actually placed (48, not 0x100 or any other fixed guess)");

    try_parse_romver(&bios);
    CHECK(strncmp(bios.version_string, "0099ZZ99999999", 14) == 0,
          "ROMVER string parsed correctly from the synthetic fixture");
    CHECK(strchr(bios.version_string, '\n') == NULL,
          "trailing newline was trimmed from the parsed version string");

    /* A buffer with no RESET/ROMDIR signature at all must not match
     * anything (and must not crash/read out of bounds). */
    uint8_t *empty_buf = malloc(buf_size);
    memset(empty_buf, 0, buf_size);
    bios_image_t empty_bios;
    memset(&empty_bios, 0, sizeof(empty_bios));
    empty_bios.data = empty_buf;
    empty_bios.size = buf_size;
    empty_bios.loaded = 1;
    try_parse_romver(&empty_bios);
    CHECK(strcmp(empty_bios.version_string, "unknown") == 0,
          "a BIOS image with no ROMDIR signature reports version \"unknown\" rather than crashing or misreading");
    free(empty_buf);

    free(buf);

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
