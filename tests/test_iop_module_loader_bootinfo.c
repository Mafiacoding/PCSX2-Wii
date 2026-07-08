/*
 * test_iop_module_loader_bootinfo.c - host-native test for Round 29
 * continued's 12th change: source/hw/iop_module_loader.c's boot_info
 * struct construction (the $a0 argument every loaded module's entry
 * point receives - see BOOT_INFO_RAM_MB's original comment and the
 * new BOOT_INFO_STRUCT_SIZE/BOOT_INFO_OFF_SCRATCH_PTR comment right
 * above it).
 *
 * The bug this guards against: a live-traced disassembly of the real
 * SCPH-10000 BIOS's own SYSMEM init code (docs/STATUS.md's "Round 29
 * continued (12th change)" section) showed it reads a boot_info
 * struct LARGER than the single RAM-MB word this project previously
 * allocated (bump_alloc(4)), and that it actively DEREFERENCES offset
 * 0x0C as a pointer, writing zero through it. Before this fix, offset
 * 0x0C was always 0 (out-of-bounds of the old 4-byte allocation), so
 * that store landed on real RAM address 0 instead of somewhere safe -
 * a genuine, live-confirmed bug. This test verifies the fix: offset
 * 0x0C now holds a valid, non-null, dedicated scratch address instead.
 *
 * IMPORTANT: like test_bios_loader.c and test_iop_elf.c, this test
 * uses an entirely SYNTHETIC ROMDIR + ELF module image - it does NOT
 * embed or reference any real PS2 BIOS bytes. The synthetic ELF
 * builder below is copied from test_iop_elf.c (same file, same
 * citations - see that file for the full IRX-format citation trail);
 * it's duplicated here rather than shared because every test file in
 * this project's suite is compiled as an independent translation
 * unit (see tests/README.md's build blocks).
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

/* --- copied verbatim from tests/test_iop_elf.c (see that file for
 * the full citation trail on the ELF/export-table format) --- */
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

/* --- synthetic ROMDIR, same convention as test_bios_loader.c: RESET/
 * ROMDIR/EXTINFO/ROMVER, plus IOPBTCONF (plain-text boot list) and one
 * loadable module ("SYSMEM" - name is arbitrary here, this synthetic
 * module doesn't run any real SYSMEM logic, it just needs to load
 * successfully so iop_module_loader_boot() reaches the boot_info
 * construction step). --- */
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
    uint8_t module_image[512];
    uint32_t module_size = build_synthetic_module(module_image);

    const uint32_t reset_sz = 48;
    const uint32_t table_off = (reset_sz + 15u) & ~15u; /* 48 */
    const uint32_t n_entries = 6; /* RESET, ROMDIR, EXTINFO, ROMVER, IOPBTCONF, SYSMEM */
    const uint32_t romdir_sz = n_entries * ROMDIR_ENTRY_SZ; /* 96 */
    const uint32_t extinfo_sz = 20;
    const uint32_t romver_sz = 16;
    const char *btconf_text = "@800\nSYSMEM\n";
    const uint32_t btconf_sz = (uint32_t)strlen(btconf_text);

    uint32_t p_reset   = 0;
    uint32_t p_romdir  = p_reset  + ((reset_sz + 15u) & ~15u);
    uint32_t p_extinfo = p_romdir + ((romdir_sz + 15u) & ~15u);
    uint32_t p_romver  = p_extinfo+ ((extinfo_sz + 15u) & ~15u);
    uint32_t p_btconf  = p_romver + ((romver_sz + 15u) & ~15u);
    uint32_t p_sysmem  = p_btconf + ((btconf_sz + 15u) & ~15u);
    uint32_t total_sz  = p_sysmem + ((module_size + 15u) & ~15u);

    uint8_t *buf = calloc(1, total_sz);
    write_romdir_entry(buf, table_off + 0 * ROMDIR_ENTRY_SZ, "RESET",     reset_sz);
    write_romdir_entry(buf, table_off + 1 * ROMDIR_ENTRY_SZ, "ROMDIR",    romdir_sz);
    write_romdir_entry(buf, table_off + 2 * ROMDIR_ENTRY_SZ, "EXTINFO",   extinfo_sz);
    write_romdir_entry(buf, table_off + 3 * ROMDIR_ENTRY_SZ, "ROMVER",    romver_sz);
    write_romdir_entry(buf, table_off + 4 * ROMDIR_ENTRY_SZ, "IOPBTCONF", btconf_sz);
    write_romdir_entry(buf, table_off + 5 * ROMDIR_ENTRY_SZ, "SYSMEM",    module_size);
    /* entry 6 (implicit, all-zero) is the terminator */

    memcpy(buf + p_btconf, btconf_text, btconf_sz);
    memcpy(buf + p_sysmem, module_image, module_size);

    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = buf;
    bios.size = total_sz;
    bios.loaded = 1;

    iop_core_init(&bios);
    iop_module_loader_reset();
    iop_state_t *st = iop_core_get_state();

    int ok = iop_module_loader_boot(st);
    CHECK(ok == 1, "iop_module_loader_boot() successfully loaded the synthetic SYSMEM module");

    uint32_t boot_info_addr = st->gpr[4]; /* $a0, as handed to the module's entry point */
    CHECK(boot_info_addr != 0, "boot_info_addr ($a0) is non-zero");

    uint32_t ram_mb = iop_mem_read32(st, boot_info_addr + 0x00);
    CHECK(ram_mb == 2, "boot_info[0x00] (RAM_MB) == 2, unchanged by this round's fix");

    uint32_t off04 = iop_mem_read32(st, boot_info_addr + 0x04);
    uint32_t off08 = iop_mem_read32(st, boot_info_addr + 0x08);
    uint32_t off10 = iop_mem_read32(st, boot_info_addr + 0x10);
    uint32_t off14 = iop_mem_read32(st, boot_info_addr + 0x14);
    CHECK(off04 == 0 && off08 == 0 && off10 == 0 && off14 == 0,
          "boot_info offsets 0x04/0x08/0x10/0x14 stay honestly zero (real values still unknown, not fabricated)");

    /* Round 29 continued (task #151/#155): offsets 0x18/0x1C are no
     * longer honestly-zero placeholders - see
     * build_real_registration_list()'s header comment in
     * iop_module_loader.c for the full derivation (docs/STATUS.md's
     * 34th/35th findings). 0x18 is a real word count minus one;
     * 0x1C is a real pointer to the registration-list array this
     * loader now builds (2 leading placeholder words, one pointer
     * per successfully-loaded module, one zero terminator). This
     * synthetic single-module test only loads one module (SYSMEM),
     * so the array is exactly 2+1+1 = 4 words: count-1 = 3. */
    uint32_t off18 = iop_mem_read32(st, boot_info_addr + 0x18);
    uint32_t off1c = iop_mem_read32(st, boot_info_addr + 0x1C);
    CHECK(off18 == 3, "boot_info[0x18] == 3 (real word count minus one: 2 placeholder + 1 pointer + 1 terminator, THE FIX task #151/#155)");
    CHECK(off1c != 0, "boot_info[0x1C] is a real, non-zero pointer to the registration-list array");
    if (off1c != 0) {
        uint32_t w0 = iop_mem_read32(st, off1c + 0);
        uint32_t w1 = iop_mem_read32(st, off1c + 4);
        uint32_t w2 = iop_mem_read32(st, off1c + 8);  /* the one real module pointer entry */
        uint32_t w3 = iop_mem_read32(st, off1c + 12); /* terminator */
        CHECK(w0 == 0 && w1 == 0, "the two leading placeholder words are honestly zero (real meaning not determined this round)");
        CHECK(w2 != 0 && (w2 & 1u) == 0, "the one real entry is a non-zero, bit0=0 (real header pointer, not a phase-tag) word");
        CHECK(w3 == 0, "the array is zero-terminated");
    }

    uint32_t scratch_ptr = iop_mem_read32(st, boot_info_addr + BOOT_INFO_OFF_SCRATCH_PTR);
    CHECK(scratch_ptr != 0, "boot_info[0x0C] (the field the real BIOS's SYSMEM init code dereferences and writes through) is non-zero - THE FIX");
    CHECK(scratch_ptr != boot_info_addr, "the scratch pointer target is a distinct address from the struct itself");

    uint32_t scratch_val = iop_mem_read32(st, scratch_ptr);
    CHECK(scratch_val == 0, "the scratch word itself reads back 0 (zero-initialized, matching what real code writes through it: sw $zero,(a0))");

    /* This is the actual regression this test guards: the live-traced
     * real BIOS code reads offset 0x0C into a1 and eventually does
     * `sw $zero,(a0)` where a0 == that value (see docs/STATUS.md's
     * "Round 29 continued (12th change)" section for the full
     * disassembly). Before this fix, offset 0x0C was always 0, so
     * that store's target was real RAM address 0. Asserting
     * scratch_ptr != 0 above already proves the fix - if this
     * regresses back to 0, that assertion fails and this test catches
     * it immediately without needing to actually execute IOP code. */

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
