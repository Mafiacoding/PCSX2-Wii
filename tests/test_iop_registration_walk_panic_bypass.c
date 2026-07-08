/*
 * test_iop_registration_walk_panic_bypass.c - host-native test for
 * Round 29 continued's task #157: source/hw/iop_module_loader.c's
 * is_registration_walk_panic_loop() recognition mechanism (see
 * docs/STATUS.md's 36th finding for the full story).
 *
 * WHAT THIS GUARDS AGAINST: once task #151/#155's
 * build_real_registration_list() supplies LOADCORE's real init code
 * with a genuine, non-empty boot_info[0x18]/[0x1C] registration list
 * (real pointers to each loaded module's own real ELF header), live
 * real-BIOS testing showed LOADCORE genuinely walks those entries
 * (no immediate rejection) but some deeper validation this project
 * has not fully characterized ultimately still fails, landing in a
 * SECOND, distinct real "write a status byte, then spin forever"
 * panic idiom - structurally the same shape as
 * is_loadcore_panic_loop()'s own target (task #148: write a status
 * code, self-jump forever) but reached from a DIFFERENT real call
 * site: only the tail three words repeat here (`sb $v0,($v1)` /
 * `j <self>` / NOP delay slot) - the $v1 "panic status address" and
 * $v0 "status code" registers are already set up by whatever earlier
 * real code led here, unlike the original panic sequence's own
 * inline `lui $v1,0x8000; addiu $v0,zero,2` setup immediately before
 * it. So is_loadcore_panic_loop() (which matches those specific 4
 * words) correctly does NOT recognize this one - this is a distinct,
 * separately-verified detector for a real, different dead end.
 *
 * IMPORTANT: like every other test in this suite, this uses an
 * entirely SYNTHETIC ROMDIR + ELF module image - no real PS2 BIOS
 * bytes. The synthetic ELF builder and ROMDIR-writing helpers are
 * copied from test_iop_loadcore_panic_bypass.c (same citations
 * apply).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

#include "hw/iop_module_loader.c"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static void wle32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static void wle16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }

/* --- copied verbatim from test_iop_loadcore_panic_bypass.c (see that
 * file for the full ELF/export-table citation trail) --- */
static uint32_t build_synthetic_module(uint8_t *buf, const char *modname, const uint32_t text_words[4])
{
    memset(buf, 0, 512);
    const uint32_t eh_size = 52;
    const uint32_t ph_off = eh_size;
    const uint32_t ph_size = 32;
    const uint32_t text_off = ph_off + ph_size;
    const uint32_t text_size = 0x60;
    const uint32_t rel_off = text_off + text_size;
    const uint32_t rel_count = 0;
    const uint32_t rel_size = rel_count * 8;
    const uint32_t sh_off = rel_off + rel_size;
    const uint32_t sh_entsize = 40;
    const uint32_t sh_count = 3;
    const uint32_t shstr_off = sh_off + sh_count * sh_entsize;

    buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 1; buf[5] = 1; buf[6] = 1;
    wle16(buf + 16, 0xff80);
    wle16(buf + 18, 8);
    wle32(buf + 20, 1);
    wle32(buf + 24, 0x00000000u);
    wle32(buf + 28, ph_off);
    wle32(buf + 32, sh_off);
    wle32(buf + 36, 1);
    wle16(buf + 40, eh_size);
    wle16(buf + 42, ph_size);
    wle16(buf + 44, 1);
    wle16(buf + 46, sh_entsize);
    wle16(buf + 48, sh_count);
    wle16(buf + 50, 2);

    wle32(buf + ph_off + 0, 1);
    wle32(buf + ph_off + 4, text_off);
    wle32(buf + ph_off + 8, 0);
    wle32(buf + ph_off + 12, 0);
    wle32(buf + ph_off + 16, text_size);
    wle32(buf + ph_off + 20, text_size + 8);
    wle32(buf + ph_off + 24, 7);
    wle32(buf + ph_off + 28, 4);

    uint8_t *text = buf + text_off;
    wle32(text + 0, text_words[0]);
    wle32(text + 4, text_words[1]);
    wle32(text + 8, text_words[2]);
    wle32(text + 12, text_words[3]);

    (void)modname;

    wle32(buf + sh_off + sh_entsize * 1 + 0, 1);
    wle32(buf + sh_off + sh_entsize * 1 + 4, 9);
    wle32(buf + sh_off + sh_entsize * 1 + 16, rel_off);
    wle32(buf + sh_off + sh_entsize * 1 + 20, rel_size);
    wle32(buf + sh_off + sh_entsize * 2 + 0, 11);
    wle32(buf + sh_off + sh_entsize * 2 + 4, 3);
    wle32(buf + sh_off + sh_entsize * 2 + 16, shstr_off);
    wle32(buf + sh_off + sh_entsize * 2 + 20, 20);

    memcpy(buf + shstr_off, "\0.rel.text\0.shstrtab\0", 21);
    return shstr_off + 21;
}

static void write_romdir_entry(uint8_t *buf, uint32_t off, const char *name, uint32_t size)
{
    uint8_t *e = buf + off;
    memset(e, 0, ROMDIR_ENTRY_SZ);
    memcpy(e, name, strlen(name));
    e[12] = (uint8_t)(size & 0xFF);
    e[13] = (uint8_t)((size >> 8) & 0xFF);
    e[14] = (uint8_t)((size >> 16) & 0xFF);
    e[15] = (uint8_t)((size >> 24) & 0xFF);
}

int main(void)
{
    /* MODA's own "code" is the real registration-walk panic tail:
     * sb $v0,($v1) ; j <self> ; nop (delay slot). Unlike the original
     * 4-word panic sequence, there is no inline lui/addiu setup here
     * - v0/v1 are assumed already set up by whatever real call site
     * led here, matching what live real-BIOS testing showed (see
     * docs/STATUS.md's 36th finding). Word 3 is an unrelated filler
     * instruction (mfhi $v0) proving the detector only looks at
     * the first 3 words, not a fixed 4-word window. */
    uint32_t moda_words[4] = {
        0xA0620000u, /* sb $v0, 0($v1) */
        0x00000000u, /* placeholder for "j <self>", filled in below */
        0x00000000u, /* nop (delay slot) */
        0x00000010u, /* mfhi $v0 - unrelated filler, must not affect detection */
    };
    uint8_t moda_image[512];
    uint32_t moda_size = build_synthetic_module(moda_image, "MODA", moda_words);

    uint32_t modb_words[4] = {
        0x03e00008u, /* jr $ra */
        0x00000000u,
        0x00000000u,
        0x00000000u,
    };
    uint8_t modb_image[512];
    uint32_t modb_size = build_synthetic_module(modb_image, "MODB", modb_words);

    const uint32_t reset_sz = 48;
    const uint32_t table_off = (reset_sz + 15u) & ~15u;
    const uint32_t n_entries = 7;
    const uint32_t romdir_sz = n_entries * ROMDIR_ENTRY_SZ;
    const uint32_t extinfo_sz = 20;
    const uint32_t romver_sz = 16;
    const char *btconf_text = "@800\nMODA\nMODB\n";
    const uint32_t btconf_sz = (uint32_t)strlen(btconf_text);

    uint32_t p_reset   = 0;
    uint32_t p_romdir  = p_reset  + ((reset_sz + 15u) & ~15u);
    uint32_t p_extinfo = p_romdir + ((romdir_sz + 15u) & ~15u);
    uint32_t p_romver  = p_extinfo+ ((extinfo_sz + 15u) & ~15u);
    uint32_t p_btconf  = p_romver + ((romver_sz + 15u) & ~15u);
    uint32_t p_moda    = p_btconf + ((btconf_sz + 15u) & ~15u);
    uint32_t p_modb    = p_moda   + ((moda_size + 15u) & ~15u);
    uint32_t total_sz  = p_modb   + ((modb_size + 15u) & ~15u);

    uint8_t *buf = calloc(1, total_sz);
    write_romdir_entry(buf, table_off + 0 * ROMDIR_ENTRY_SZ, "RESET",     reset_sz);
    write_romdir_entry(buf, table_off + 1 * ROMDIR_ENTRY_SZ, "ROMDIR",    romdir_sz);
    write_romdir_entry(buf, table_off + 2 * ROMDIR_ENTRY_SZ, "EXTINFO",   extinfo_sz);
    write_romdir_entry(buf, table_off + 3 * ROMDIR_ENTRY_SZ, "ROMVER",    romver_sz);
    write_romdir_entry(buf, table_off + 4 * ROMDIR_ENTRY_SZ, "IOPBTCONF", btconf_sz);
    write_romdir_entry(buf, table_off + 5 * ROMDIR_ENTRY_SZ, "MODA",      moda_size);
    write_romdir_entry(buf, table_off + 6 * ROMDIR_ENTRY_SZ, "MODB",      modb_size);

    memcpy(buf + p_btconf, btconf_text, btconf_sz);
    memcpy(buf + p_moda, moda_image, moda_size);
    memcpy(buf + p_modb, modb_image, modb_size);

    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = buf;
    bios.size = total_sz;
    bios.loaded = 1;

    iop_core_init(&bios);
    iop_module_loader_reset();
    iop_state_t *st = iop_core_get_state();

    int ok = iop_module_loader_boot(st);
    CHECK(ok == 1, "iop_module_loader_boot() successfully front-loaded and started MODA");

    /* Task #155's build_real_registration_list() must have run and
     * populated a real, non-empty list (2 loaded modules -> 2+3=5
     * words: 2 placeholder + 2 pointers + 1 terminator, count-1=4). */
    iop_module_loader_stats_t *lstats = iop_module_loader_get_stats();
    CHECK(lstats->registration_list_entries == 2,
          "build_real_registration_list() recorded 2 real entries (MODA+MODB both loaded)");

    uint32_t moda_entry = st->pc;

    uint32_t target = moda_entry;
    uint32_t j_instr = 0x08000000u | ((target >> 2) & 0x03FFFFFFu);
    iop_mem_write32(st, moda_entry + 4u, j_instr);

    CHECK(is_registration_walk_panic_loop(st, moda_entry) == 1,
          "is_registration_walk_panic_loop() recognizes the real sb+self-j+nop tail at MODA's entry");

    /* Negative control 1: near-miss - SB using a different base
     * register ($at instead of $v1) must NOT be recognized. */
    iop_mem_write32(st, moda_entry, 0xA0220000u); /* sb $v0, 0($at) - wrong base */
    CHECK(is_registration_walk_panic_loop(st, moda_entry) == 0,
          "is_registration_walk_panic_loop() correctly rejects a near-miss (wrong base register)");
    iop_mem_write32(st, moda_entry, 0xA0620000u); /* restore */

    /* Negative control 2: the jump must target itself (the sb
     * instruction) - a jump to some OTHER address must NOT be
     * recognized as this specific panic idiom. */
    uint32_t wrong_target = moda_entry + 64u;
    uint32_t wrong_j = 0x08000000u | ((wrong_target >> 2) & 0x03FFFFFFu);
    iop_mem_write32(st, moda_entry + 4u, wrong_j);
    CHECK(is_registration_walk_panic_loop(st, moda_entry) == 0,
          "is_registration_walk_panic_loop() correctly rejects a jump to a DIFFERENT address (not self)");
    iop_mem_write32(st, moda_entry + 4u, j_instr); /* restore */

    /* Negative control 3: a non-zero delay slot (not a real nop)
     * must NOT be recognized. */
    iop_mem_write32(st, moda_entry + 8u, 0x00000001u); /* not a real nop */
    CHECK(is_registration_walk_panic_loop(st, moda_entry) == 0,
          "is_registration_walk_panic_loop() correctly rejects a non-nop delay slot");
    iop_mem_write32(st, moda_entry + 8u, 0x00000000u); /* restore */

    /* Confirm this is genuinely DISTINCT from is_loadcore_panic_loop()
     * - the 3-word tail alone (no inline lui/addiu setup before it)
     * must NOT be recognized as the ORIGINAL 4-word panic sequence. */
    CHECK(is_loadcore_panic_loop(st, moda_entry) == 0,
          "is_loadcore_panic_loop() (the ORIGINAL, task #148 detector) does NOT recognize this different tail - confirms these are separate, non-overlapping mechanisms");

    /* Now confirm the bypass actually advances execution instead of
     * halting, exactly like the other two bypasses. */
    uint32_t stats_before = iop_module_loader_get_stats()->registration_walk_panics_bypassed;
    int handled = iop_module_loader_try_handle(st, moda_entry);
    CHECK(handled == 1, "iop_module_loader_try_handle() recognized and handled the registration-walk panic tail");
    CHECK(iop_module_loader_get_stats()->registration_walk_panics_bypassed == stats_before + 1,
          "registration_walk_panics_bypassed incremented exactly once");
    CHECK(st->halted == 0, "core did NOT halt - advanced to the next module instead");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
