/*
 * test_iop_loadcore_panic_bypass.c - host-native test for Round 29
 * continued's 28th change: source/hw/iop_module_loader.c's
 * is_loadcore_panic_loop() recognition mechanism (task #124/#132/#148
 * - see docs/STATUS.md's 27th finding for the full root-cause story).
 *
 * WHAT THIS GUARDS AGAINST: real LOADCORE module-loader code reaches
 * a genuine, deliberate real-BIOS "panic: write status code 2 to
 * physical RAM address 0, then spin forever" sequence
 * (lui $v1,0x8000; addiu $v0,zero,2; sb $v0,($v1); j <self>) when its
 * own internal multi-phase module/library self-registration list
 * turns up empty - which happens in this project's emulation because
 * this project's own loader runs exactly one module's ELF and entry
 * point at a time. This project cannot safely fabricate real entries
 * for that list (they get called via a real `jalr`, so a wrong guess
 * doesn't fail safely), so instead it recognizes the exact panic
 * instruction sequence by its literal encoded bytes and treats
 * reaching it exactly like a module returning through this loader's
 * own trampoline: advances to the next module in the real IOPBTCONF
 * list instead of letting the real panic sequence spin forever.
 *
 * IMPORTANT: like every other test in this suite, this uses an
 * entirely SYNTHETIC ROMDIR + ELF module image - no real PS2 BIOS
 * bytes. The synthetic ELF builder is copied from test_iop_elf.c /
 * test_iop_module_loader_bootinfo.c (same citations apply).
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

/* --- copied verbatim from test_iop_elf.c / test_iop_module_loader_bootinfo.c
 * (see those files for the full ELF/export-table citation trail) ---
 * text_words[0..3] let each caller plant a different 4-word payload
 * (the module's own "code") at the very start of its .text section,
 * which becomes byte-exact IOP RAM content at the module's load
 * address once iop_elf_load() relocates it - this is how this test
 * plants the exact panic-loop signature at a known, predictable
 * address without needing any real BIOS bytes. */
static uint32_t build_synthetic_module(uint8_t *buf, const char *modname, const uint32_t text_words[4])
{
    memset(buf, 0, 512);
    const uint32_t eh_size = 52;
    const uint32_t ph_off = eh_size;
    const uint32_t ph_size = 32;
    const uint32_t text_off = ph_off + ph_size;
    const uint32_t text_size = 0x60;
    const uint32_t rel_off = text_off + text_size;
    const uint32_t rel_count = 0; /* no relocations needed for this test */
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

    /* No export table for this test (offset it references stays all
     * zero / unused) - modname isn't embedded anywhere real modules
     * would put it; it's only a ROMDIR/IOPBTCONF label here. */
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
    /* MODA's own "code" begins with the exact real panic-loop
     * signature (lui $v1,0x8000; addiu $v0,zero,2; sb $v0,($v1);
     * j <self, i.e. back to the sb instruction 8 bytes earlier>).
     * The self-jump target is computed relative to MODA's own load
     * address once we know it (see below) - so this test first loads
     * with a placeholder, then re-derives and re-writes the exact
     * target the same way the interpreter itself would decode it,
     * to avoid hardcoding any address this synthetic test doesn't
     * actually control. */
    uint32_t moda_words[4] = {
        0x3C038000u, /* lui $v1, 0x8000 */
        0x24020002u, /* addiu $v0, $zero, 2 */
        0xA0620000u, /* sb $v0, 0($v1) */
        0x00000000u, /* placeholder for "j <self>", filled in below */
    };
    uint8_t moda_image[512];
    uint32_t moda_size = build_synthetic_module(moda_image, "MODA", moda_words);

    uint32_t modb_words[4] = {
        0x03e00008u, /* jr $ra */
        0x00000000u, /* nop */
        0x00000000u,
        0x00000000u,
    };
    uint8_t modb_image[512];
    uint32_t modb_size = build_synthetic_module(modb_image, "MODB", modb_words);

    const uint32_t reset_sz = 48;
    const uint32_t table_off = (reset_sz + 15u) & ~15u;
    const uint32_t n_entries = 7; /* RESET, ROMDIR, EXTINFO, ROMVER, IOPBTCONF, MODA, MODB */
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
    CHECK(ok == 1, "iop_module_loader_boot() successfully loaded MODA (first module)");

    uint32_t moda_entry = st->pc; /* MODA's real entry, as redirected by boot() */

    /* Re-derive and patch in the real self-jump target now that we
     * know MODA's actual load/text address: the "j" instruction sits
     * at moda_entry+12 and must target moda_entry+8 (the sb
     * instruction), matching the exact real BIOS panic sequence this
     * mechanism recognizes. */
    uint32_t target = moda_entry + 8u;
    uint32_t j_instr = 0x08000000u | ((target >> 2) & 0x03FFFFFFu);
    iop_mem_write32(st, moda_entry + 12u, j_instr);

    CHECK(is_loadcore_panic_loop(st, moda_entry) == 1,
          "is_loadcore_panic_loop() recognizes the exact real panic-loop byte signature at MODA's entry");

    /* Negative control: a near-miss (SB using a DIFFERENT base
     * register than $v1, e.g. $at) must NOT be recognized - proves
     * this isn't a loose/overbroad match (this is exactly the
     * register-encoding bug this project's own first implementation
     * attempt made and caught by re-disassembling the real bytes -
     * see docs/STATUS.md's 28th finding). */
    iop_mem_write32(st, moda_entry + 8u, 0xA0220000u); /* sb $v0, 0($at) - wrong base reg */
    CHECK(is_loadcore_panic_loop(st, moda_entry) == 0,
          "is_loadcore_panic_loop() correctly rejects a near-miss (wrong base register in the sb)");
    iop_mem_write32(st, moda_entry + 8u, 0xA0620000u); /* restore the real sequence */

    /* Negative control: a jump that does NOT loop back to the sb
     * instruction (e.g. targets somewhere else entirely) must not be
     * recognized either - proves the self-loop check is load-bearing,
     * not just checking the first 3 words. */
    uint32_t not_self_target = moda_entry + 0x100u;
    uint32_t not_self_j = 0x08000000u | ((not_self_target >> 2) & 0x03FFFFFFu);
    iop_mem_write32(st, moda_entry + 12u, not_self_j);
    CHECK(is_loadcore_panic_loop(st, moda_entry) == 0,
          "is_loadcore_panic_loop() correctly rejects a jump that does not loop back to the sb instruction");
    iop_mem_write32(st, moda_entry + 12u, j_instr); /* restore the real self-loop */

    /* Now drive the actual interpreter-facing entry point: reaching
     * MODA's entry (which IS the panic-loop signature) through
     * iop_module_loader_try_handle() must advance straight to MODB
     * instead of ever letting the panic sequence execute. */
    int handled = iop_module_loader_try_handle(st, moda_entry);
    CHECK(handled == 1, "iop_module_loader_try_handle() recognizes and handles the panic-loop signature");
    CHECK(st->halted == 0, "the bypass does NOT halt the core - it advances to the next module");
    CHECK(st->pc != moda_entry, "pc moved away from MODA's panic-loop entry");

    iop_module_loader_stats_t *stats = iop_module_loader_get_stats();
    CHECK(stats->panic_loops_bypassed == 1, "panic_loops_bypassed stat incremented exactly once");
    CHECK(stats->modules_loaded == 2, "both MODA and MODB (the bypass target) are now loaded");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
