/*
 * test_iop_module_loader_p_twin_skip.c - Round 763 (task #758 follow-up):
 * host-native regression coverage for is_skippable_fixed_p_twin() /
 * the new guard in load_only_one() (source/hw/iop_module_loader.c).
 *
 * The bug this guards against: Round 762 extended
 * kernel_tier_fixed_address() with 9 real fixed addresses, but
 * deliberately did NOT include TIMEMANP/TIMEMANI despite both citing
 * the same real load start ("00007D00" in their own uploaded source
 * headers) - doing so naively would have made the "I" twin's later
 * load silently overwrite the "P" twin's already-relocated code at
 * the same address before the P-twin's own already-computed entry
 * point ever ran (load_all_modules() front-loads every module before
 * invoking any entry point). Round 763 fixes this by skipping the
 * P-twin's load entirely (not just its export registration, which
 * module_has_i_twin() already handled) whenever it would collide with
 * its I-twin at a shared fixed address - this test proves that skip
 * actually happens and that the I-twin still loads correctly at the
 * real address.
 *
 * IMPORTANT: like the other iop_module_loader.c tests, this test uses
 * an entirely SYNTHETIC ROMDIR + IOPBTCONF + ELF module images - it
 * does NOT embed or reference any real PS2 BIOS bytes. The synthetic
 * ELF builder is copied verbatim from test_iop_module_loader_bootinfo.c
 * (itself copied from test_iop_elf.c) - see those files for the full
 * IRX-format citation trail.
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

/* --- copied verbatim from test_iop_module_loader_bootinfo.c/test_iop_elf.c --- */
static uint32_t build_synthetic_module(uint8_t *buf)
{
    memset(buf, 0, 512);
    const uint32_t eh_size = 52;
    const uint32_t ph_off = eh_size;
    const uint32_t ph_size = 32;
    const uint32_t text_off = ph_off + ph_size;
    const uint32_t text_size = 0x60;
    const uint32_t rel_off = text_off + text_size;
    const uint32_t rel_count = 4;
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
    wle32(buf + 24, 0x00000010u);
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
    wle32(text + 0, 0x08000000u);
    wle32(text + 4, 0x12345678u);
    wle32(text + 8, 0x3C080022u);
    wle32(text + 12, 0x35084000u);

    const uint32_t exp_off = 16;
    wle32(text + exp_off + 0, 0x41c00000u);
    wle32(text + exp_off + 4, 0);
    wle32(text + exp_off + 8, 0x0101u);
    memcpy(text + exp_off + 12, "TESTMOD\0", 8);
    wle32(text + exp_off + 20, 0x00000060u);
    wle32(text + exp_off + 24, 0x00000070u);
    wle32(text + exp_off + 28, 0);

    const uint32_t imp_off = 48;
    wle32(text + imp_off + 0, 0x41e00000u);
    wle32(text + imp_off + 4, 0);
    wle32(text + imp_off + 8, 0x0100u);
    memcpy(text + imp_off + 12, "OTHRMOD\0", 8);
    wle32(text + imp_off + 20, 0x03e00008u);
    wle32(text + imp_off + 24, 0x24000005u);
    wle32(text + imp_off + 28, 0x03e00008u);
    wle32(text + imp_off + 32, 0x24000007u);
    wle32(text + imp_off + 36, 0);
    wle32(text + imp_off + 40, 0);

    wle32(buf + rel_off + 0, 0);   wle32(buf + rel_off + 4, 4);
    wle32(buf + rel_off + 8, 4);   wle32(buf + rel_off + 12, 2);
    wle32(buf + rel_off + 16, 8);  wle32(buf + rel_off + 20, 5);
    wle32(buf + rel_off + 24, 12); wle32(buf + rel_off + 28, 6);

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
    /* Unit-level check first: is_skippable_fixed_p_twin() itself,
     * independent of a full synthetic boot - exercises the exact
     * logic this round added without needing a whole ROMDIR/ELF
     * fixture for every case. */
    iop_module_loader_reset();
    g.modlist_count = 0;
    strcpy(g.modlist[g.modlist_count++], "TIMEMANP");
    strcpy(g.modlist[g.modlist_count++], "TIMEMANI");
    CHECK(is_skippable_fixed_p_twin("TIMEMANP") == 1,
          "TIMEMANP is flagged skippable when TIMEMANI (same fixed addr) is also in the modlist");
    CHECK(is_skippable_fixed_p_twin("TIMEMANI") == 0,
          "TIMEMANI itself is never flagged skippable (it's the 'I' twin, not a 'P' name)");
    CHECK(is_skippable_fixed_p_twin("SSBUSC") == 0,
          "a non-P-twin fixed-address module (SSBUSC) is never flagged skippable");

    g.modlist_count = 0;
    strcpy(g.modlist[g.modlist_count++], "TIMEMANP");
    /* no TIMEMANI this time */
    CHECK(is_skippable_fixed_p_twin("TIMEMANP") == 0,
          "TIMEMANP is NOT flagged skippable when no TIMEMANI is present in the modlist (must actually load)");

    g.modlist_count = 0;
    strcpy(g.modlist[g.modlist_count++], "INTRMANP");
    strcpy(g.modlist[g.modlist_count++], "INTRMANI");
    CHECK(is_skippable_fixed_p_twin("INTRMANP") == 0,
          "INTRMANP is NOT flagged skippable - this pair has no fixed address (kernel_tier_fixed_address()==0 for both), so there's no collision to guard against; existing bump_alloc()-based P/I behavior (module_has_i_twin(), verified in every prior round) is completely unaffected by this round's change");

    /* Full synthetic-boot check: a two-module IOPBTCONF ("TIMEMANP",
     * "TIMEMANI"), both pointing at loadable synthetic ELF images.
     * Verifies the end-to-end effect: TIMEMANP's own modlist slot
     * gets entry point 0 (never loaded), TIMEMANI's slot gets a real,
     * non-zero entry point whose value falls inside the real fixed
     * address range [0x7D00, 0x7D00+module_size) - i.e. TIMEMANI, not
     * TIMEMANP, is the one that actually ends up resident at the real
     * address, matching real hardware's own "I twin always wins"
     * convention (module_has_i_twin()'s existing citation trail). */
    uint8_t module_image[512];
    uint32_t module_size = build_synthetic_module(module_image);

    const uint32_t reset_sz = 48;
    const uint32_t table_off = (reset_sz + 15u) & ~15u;
    const uint32_t n_entries = 7; /* RESET, ROMDIR, EXTINFO, ROMVER, IOPBTCONF, TIMEMANP, TIMEMANI */
    const uint32_t romdir_sz = n_entries * ROMDIR_ENTRY_SZ;
    const uint32_t extinfo_sz = 20;
    const uint32_t romver_sz = 16;
    const char *btconf_text = "@800\nTIMEMANP\nTIMEMANI\n";
    const uint32_t btconf_sz = (uint32_t)strlen(btconf_text);

    uint32_t p_reset   = 0;
    uint32_t p_romdir  = p_reset  + ((reset_sz + 15u) & ~15u);
    uint32_t p_extinfo = p_romdir + ((romdir_sz + 15u) & ~15u);
    uint32_t p_romver  = p_extinfo+ ((extinfo_sz + 15u) & ~15u);
    uint32_t p_btconf  = p_romver + ((romver_sz + 15u) & ~15u);
    uint32_t p_timemanp = p_btconf + ((btconf_sz + 15u) & ~15u);
    uint32_t p_timemani = p_timemanp + ((module_size + 15u) & ~15u);
    uint32_t total_sz  = p_timemani + ((module_size + 15u) & ~15u);

    uint8_t *buf = calloc(1, total_sz);
    write_romdir_entry(buf, table_off + 0 * ROMDIR_ENTRY_SZ, "RESET",     reset_sz);
    write_romdir_entry(buf, table_off + 1 * ROMDIR_ENTRY_SZ, "ROMDIR",    romdir_sz);
    write_romdir_entry(buf, table_off + 2 * ROMDIR_ENTRY_SZ, "EXTINFO",   extinfo_sz);
    write_romdir_entry(buf, table_off + 3 * ROMDIR_ENTRY_SZ, "ROMVER",    romver_sz);
    write_romdir_entry(buf, table_off + 4 * ROMDIR_ENTRY_SZ, "IOPBTCONF", btconf_sz);
    write_romdir_entry(buf, table_off + 5 * ROMDIR_ENTRY_SZ, "TIMEMANP",  module_size);
    write_romdir_entry(buf, table_off + 6 * ROMDIR_ENTRY_SZ, "TIMEMANI",  module_size);

    memcpy(buf + p_btconf, btconf_text, btconf_sz);
    memcpy(buf + p_timemanp, module_image, module_size);
    memcpy(buf + p_timemani, module_image, module_size);

    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = buf;
    bios.size = total_sz;
    bios.loaded = 1;

    iop_core_init(&bios);
    iop_module_loader_reset();
    iop_state_t *st = iop_core_get_state();

    int ok = iop_module_loader_boot(st);
    CHECK(ok == 1, "iop_module_loader_boot() succeeded with a synthetic TIMEMANP+TIMEMANI modlist");

    CHECK(iop_module_loader_get_module_count() == 2,
          "modlist has exactly 2 entries (TIMEMANP, TIMEMANI)");
    CHECK(strcmp(iop_module_loader_get_module_name(0), "TIMEMANP") == 0,
          "modlist[0] is TIMEMANP");
    CHECK(strcmp(iop_module_loader_get_module_name(1), "TIMEMANI") == 0,
          "modlist[1] is TIMEMANI");

    uint32_t p_entry = iop_module_loader_get_module_entry(0);
    uint32_t i_entry = iop_module_loader_get_module_entry(1);
    CHECK(p_entry == 0,
          "TIMEMANP's own entry point is 0 - THE FIX: its load was skipped entirely, not just its export registration");
    CHECK(i_entry != 0,
          "TIMEMANI's own entry point is non-zero - it DID load successfully");
    CHECK(i_entry >= 0x00007D00u && i_entry < (0x00007D00u + module_size + 0x1000u),
          "TIMEMANI's entry point falls inside the real fixed address range starting at 0x00007D00 - it, not TIMEMANP, is the one resident at the real address");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
