/*
 * test_iop_trap_stub_bypass.c - host-native test for Round 29
 * continued's 32nd change: source/hw/iop_module_loader.c's
 * is_unconditional_trap_stub() recognition mechanism (task #151/#152
 * - see docs/STATUS.md's 29th/30th/31st findings for the full
 * root-cause story).
 *
 * WHAT THIS GUARDS AGAINST: a real syscall from a later-loaded module
 * (first observed: INTRMANP calling ExitCriticalSection) falls
 * through to the still-unclaimed general exception vector, which
 * LOADCORE's own real init code has by then installed with a
 * ten-real-instruction prologue (saves $k0/$at, reads Status, masks
 * $k0) ending in an UNCONDITIONAL TGE (Trap if Greater or Equal,
 * rs==rt so it always traps) - which immediately re-vectors back to
 * the same address forever with zero observable state change. This
 * is the SAME underlying architectural gap as the LOADCORE panic loop
 * (task #124/#132/#148) - LOADCORE's real registration list is empty
 * - surfacing through a real syscall-driven path instead of a direct
 * self-jump. This project cannot safely fabricate a real registration
 * entry (see the panic-loop bypass's own header comment for why), so
 * instead it recognizes this exact byte-for-byte prologue plus the
 * STRUCTURAL shape of "always traps" (rather than one hardcoded trap
 * "code" value - the same stub template was observed reused at a
 * nearby address with a different code field) and treats reaching it
 * exactly like a module returning through this loader's own
 * trampoline: advances to the next module in the real IOPBTCONF list.
 *
 * IMPORTANT: like every other test in this suite, this uses an
 * entirely SYNTHETIC ROMDIR + ELF module image - no real PS2 BIOS
 * bytes. The synthetic ELF builder and ROMDIR-writing helpers are
 * copied from test_iop_loadcore_panic_bypass.c (same citations
 * apply), extended here to plant an 11-word payload instead of 4.
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

/* Same synthetic-ELF builder as test_iop_loadcore_panic_bypass.c, but
 * with an 11-word .text payload (up from 4) so the whole prologue +
 * trap instruction fits in one module's planted code. */
static uint32_t build_synthetic_module11(uint8_t *buf, const char *modname, const uint32_t text_words[11])
{
    memset(buf, 0, 512);
    const uint32_t eh_size = 52;
    const uint32_t ph_off = eh_size;
    const uint32_t ph_size = 32;
    const uint32_t text_off = ph_off + ph_size;
    const uint32_t text_size = 0x60; /* 96 bytes - covers 11 words (44 bytes) plus headroom */
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
    for (int i = 0; i < 11; i++) wle32(text + i * 4, text_words[i]);

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
    /* MODA's own "code" is the exact real trap-stub prologue (see
     * is_unconditional_trap_stub()'s own header comment for the full
     * derivation), ending in an unconditional TGE $k1,$k1 (rs==rt=27,
     * an arbitrary register choice distinct from the real BIOS's own
     * $zero,$zero/code=2 - proving the check is structural, not tied
     * to one specific encoding). */
    uint32_t moda_words[11] = {
        0x00000000u, /* nop */
        0xAC1A0410u, /* sw $k0, 0x410($zero) */
        0x00000090u, /* mfhi $zero (sa=2) */
        0x40016000u, /* mfc0 $at, $12 (Status) */
        0x00000000u, /* nop */
        0xAC010408u, /* sw $at, 0x408($zero) */
        0x000000A0u, /* add $zero,$zero,$zero (sa=5) */
        0x00000000u, /* nop */
        0x00000000u, /* nop */
        0x335A003Cu, /* andi $k0, $k0, 0x3c */
        (27u << 21) | (27u << 16) | 0x30u, /* tge $k1, $k1 (rs==rt, unconditional) */
    };
    uint8_t moda_image[512];
    uint32_t moda_size = build_synthetic_module11(moda_image, "MODA", moda_words);

    uint32_t modb_words[11] = {
        0x03e00008u, /* jr $ra */
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    uint8_t modb_image[512];
    uint32_t modb_size = build_synthetic_module11(modb_image, "MODB", modb_words);

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

    uint32_t moda_entry = st->pc;

    CHECK(is_unconditional_trap_stub(st, moda_entry) == 1,
          "is_unconditional_trap_stub() recognizes the exact real prologue + an unconditional TGE (rs==rt)");

    /* Negative control 1: a near-miss in the prologue itself (wrong
     * SW base register, e.g. $at instead of $zero for the second
     * store) must NOT be recognized. */
    iop_mem_write32(st, moda_entry + 20u, 0xAC210408u); /* sw $at, 0x408($at) - wrong base */
    CHECK(is_unconditional_trap_stub(st, moda_entry) == 0,
          "is_unconditional_trap_stub() correctly rejects a near-miss in the prologue (wrong base register)");
    iop_mem_write32(st, moda_entry + 20u, 0xAC010408u); /* restore */

    /* Negative control 2: a CONDITIONAL trap (rs != rt) at the same
     * position must NOT be recognized - proves the check isn't just
     * "any TGE here", but specifically an unconditional one. */
    uint32_t conditional_tge = (8u << 21) | (9u << 16) | 0x30u; /* tge $t0, $t1 - rs != rt */
    iop_mem_write32(st, moda_entry + 40u, conditional_tge);
    CHECK(is_unconditional_trap_stub(st, moda_entry) == 0,
          "is_unconditional_trap_stub() correctly rejects a conditional TGE (rs != rt)");

    /* Negative control 3: a different SPECIAL funct at the same
     * position (e.g. TEQ, funct 0x34) must NOT be recognized either -
     * proves the check specifically requires funct 0x30 (TGE). */
    uint32_t teq_instr = (27u << 21) | (27u << 16) | 0x34u;
    iop_mem_write32(st, moda_entry + 40u, teq_instr);
    CHECK(is_unconditional_trap_stub(st, moda_entry) == 0,
          "is_unconditional_trap_stub() correctly rejects a different SPECIAL funct (TEQ) at the same position");
    iop_mem_write32(st, moda_entry + 40u, (27u << 21) | (27u << 16) | 0x30u); /* restore */

    /* Now drive the actual interpreter-facing entry point: reaching
     * MODA's entry (the trap-stub signature) through
     * iop_module_loader_try_handle() must advance straight to MODB
     * instead of ever letting the trap execute (which would otherwise
     * recurse forever via the CPU's own exception delivery). */
    int handled = iop_module_loader_try_handle(st, moda_entry);
    CHECK(handled == 1, "iop_module_loader_try_handle() recognizes and handles the trap-stub signature");
    CHECK(st->halted == 0, "the bypass does NOT halt the core - it advances to the next module");
    CHECK(st->pc != moda_entry, "pc moved away from MODA's trap-stub entry");

    iop_module_loader_stats_t *stats = iop_module_loader_get_stats();
    CHECK(stats->trap_stubs_bypassed == 1, "trap_stubs_bypassed stat incremented exactly once");
    CHECK(stats->modules_loaded == 2, "both MODA and MODB (the bypass target) were front-loaded");

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
