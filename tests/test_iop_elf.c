/*
 * test_iop_elf.c - host-native test for the real ELF32/MIPS IOP
 * module loader (source/hw/iop_elf.c, task #92). See
 * include/core/hw/iop_elf.h for the full scope/citations.
 *
 * IMPORTANT: this test builds its own SYNTHETIC ELF32/MIPS module at
 * runtime (below) - it does NOT embed any bytes extracted from the
 * user's real, copyrighted PS2 BIOS. The synthetic module's
 * structure (ELF header, one PT_LOAD program header, a .rel.text
 * section with real MIPS relocation types, a real IRX export table,
 * a real IRX import table) is built from the same public,
 * independently-citable references documented in iop_elf.h
 * (ps2sdk's irx.h + the community "Writing a PS2 BIOS in Rust" IOP
 * chapter) - reproducing the FORMAT, not any of the real BIOS's own
 * bytes.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "core/hw/iop_elf.h"
#include "core/iop/iop_core.h"
#include "core/bios_loader.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static void wle32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wle16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/* Builds a small, synthetic ELF32/MIPS "IRX-shaped" module image into
 * `buf` (caller-provided, must be at least 512 bytes) and returns its
 * total size. Layout (all offsets chosen to be simple and readable,
 * not mirroring any real module's actual layout):
 *
 *   ELF header (52 bytes) @0
 *   1 program header (32 bytes) @52
 *   .text (96 bytes, 0x60) @84        - 4 words needing relocation,
 *                                        then a real export table,
 *                                        then a real import table
 *   .rel.text (4 entries, 32 bytes) @180
 *   section header table (3 * 40 = 120 bytes) @212
 *   .shstrtab bytes @332
 */
static uint32_t build_synthetic_module(uint8_t *buf)
{
    memset(buf, 0, 512);

    const uint32_t eh_size = 52;
    const uint32_t ph_off = eh_size;
    const uint32_t ph_size = 32;
    const uint32_t text_off = ph_off + ph_size;      /* 84 */
    const uint32_t text_size = 0x60;                 /* 96 bytes */
    const uint32_t rel_off = text_off + text_size;   /* 180 */
    const uint32_t rel_count = 4;
    const uint32_t rel_size = rel_count * 8;         /* 32 */
    const uint32_t sh_off = rel_off + rel_size;      /* 212 */
    const uint32_t sh_entsize = 40;
    const uint32_t sh_count = 3;
    const uint32_t shstr_off = sh_off + sh_count * sh_entsize; /* 332 */

    /* --- ELF header --- */
    buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 1; buf[5] = 1; buf[6] = 1; /* class=32, data=LE, version=1 */
    wle16(buf + 16, 0xff80);  /* e_type - real IOP-module vendor type (see iop_elf.h) */
    wle16(buf + 18, 8);       /* e_machine = EM_MIPS */
    wle32(buf + 20, 1);       /* e_version */
    wle32(buf + 24, 0x00000010u); /* e_entry - arbitrary offset within .text */
    wle32(buf + 28, ph_off);
    wle32(buf + 32, sh_off);
    wle32(buf + 36, 1);       /* e_flags */
    wle16(buf + 40, eh_size);
    wle16(buf + 42, ph_size);
    wle16(buf + 44, 1);       /* e_phnum */
    wle16(buf + 46, sh_entsize);
    wle16(buf + 48, sh_count);
    wle16(buf + 50, 2);       /* e_shstrndx */

    /* --- program header: 1 PT_LOAD, filesz < memsz to also exercise
     * the bss zero-fill path --- */
    wle32(buf + ph_off + 0, 1);            /* p_type = PT_LOAD */
    wle32(buf + ph_off + 4, text_off);     /* p_offset */
    wle32(buf + ph_off + 8, 0);            /* p_vaddr */
    wle32(buf + ph_off + 12, 0);           /* p_paddr */
    wle32(buf + ph_off + 16, text_size);   /* p_filesz */
    wle32(buf + ph_off + 20, text_size + 8); /* p_memsz - 8 extra bss bytes */
    wle32(buf + ph_off + 24, 7);            /* p_flags */
    wle32(buf + ph_off + 28, 4);            /* p_align */

    /* --- .text contents --- */
    uint8_t *text = buf + text_off;
    /* offset 0: J 0x00000000 (opcode 0x02) - R_MIPS_26 target */
    wle32(text + 0, 0x08000000u);
    /* offset 4: a raw "pointer" word - R_MIPS_32 target */
    wle32(text + 4, 0x12345678u);
    /* offset 8: LUI $t0, 0x0022 - R_MIPS_HI16 target */
    wle32(text + 8, 0x3C080022u);
    /* offset 12: ORI $t0,$t0,0x4000 - R_MIPS_LO16 target (paired with the HI16 above) */
    wle32(text + 12, 0x35084000u);

    /* offset 16: real IRX export table (magic 0x41c00000 - see
     * iop_elf.h citations) */
    const uint32_t exp_off = 16;
    wle32(text + exp_off + 0, 0x41c00000u); /* magic */
    wle32(text + exp_off + 4, 0);            /* zero */
    wle32(text + exp_off + 8, 0x0101u);      /* version */
    memcpy(text + exp_off + 12, "TESTMOD\0", 8);
    wle32(text + exp_off + 20, 0x00000060u); /* fptrs[0] */
    wle32(text + exp_off + 24, 0x00000070u); /* fptrs[1] */
    wle32(text + exp_off + 28, 0);            /* terminator */

    /* offset 48: real IRX import table (magic 0x41e00000) */
    const uint32_t imp_off = 48;
    wle32(text + imp_off + 0, 0x41e00000u); /* magic */
    wle32(text + imp_off + 4, 0);
    wle32(text + imp_off + 8, 0x0100u);
    memcpy(text + imp_off + 12, "OTHRMOD\0", 8);
    wle32(text + imp_off + 20, 0x03e00008u); /* stub 0: jr $ra */
    wle32(text + imp_off + 24, 0x24000005u); /* stub 0: addiu $zero,$zero,5 (ordinal 5) */
    wle32(text + imp_off + 28, 0x03e00008u); /* stub 1: jr $ra */
    wle32(text + imp_off + 32, 0x24000007u); /* stub 1: ordinal 7 */
    wle32(text + imp_off + 36, 0);            /* terminator */
    wle32(text + imp_off + 40, 0);

    /* --- .rel.text: 4 entries, r_sym=0 for all (see iop_elf.h - real
     * modules never use a named symbol, only rebase-from-0) --- */
    wle32(buf + rel_off + 0, 0);   wle32(buf + rel_off + 4, 4);  /* offset=0,  type=R_MIPS_26 */
    wle32(buf + rel_off + 8, 4);   wle32(buf + rel_off + 12, 2); /* offset=4,  type=R_MIPS_32 */
    wle32(buf + rel_off + 16, 8);  wle32(buf + rel_off + 20, 5); /* offset=8,  type=R_MIPS_HI16 */
    wle32(buf + rel_off + 24, 12); wle32(buf + rel_off + 28, 6); /* offset=12, type=R_MIPS_LO16 */

    /* --- section headers --- */
    /* [0] NULL */
    /* [1] .rel.text */
    wle32(buf + sh_off + sh_entsize * 1 + 0, 1);        /* sh_name (offset into shstrtab, arbitrary non-zero) */
    wle32(buf + sh_off + sh_entsize * 1 + 4, 9);        /* sh_type = SHT_REL */
    wle32(buf + sh_off + sh_entsize * 1 + 16, rel_off); /* sh_offset */
    wle32(buf + sh_off + sh_entsize * 1 + 20, rel_size);/* sh_size */
    /* [2] .shstrtab */
    wle32(buf + sh_off + sh_entsize * 2 + 0, 11);
    wle32(buf + sh_off + sh_entsize * 2 + 4, 3);        /* sh_type = SHT_STRTAB */
    wle32(buf + sh_off + sh_entsize * 2 + 16, shstr_off);
    wle32(buf + sh_off + sh_entsize * 2 + 20, 20);

    memcpy(buf + shstr_off, "\0.rel.text\0.shstrtab\0", 21);

    return shstr_off + 21;
}

int main(void)
{
    uint8_t image[512];
    uint32_t image_size = build_synthetic_module(image);

    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    iop_core_init(&bios);
    iop_state_t *st = iop_core_get_state();

    const uint32_t load_addr = 0x00100000u;
    iop_elf_load_result_t res;
    const char *err = NULL;
    int rc = iop_elf_load(st, image, image_size, load_addr, &res, &err);

    CHECK(rc == 0, "iop_elf_load() succeeds on a well-formed synthetic module");
    CHECK(err == NULL, "no error string set on success");
    CHECK(res.entry == load_addr + 0x10u, "entry point is load_addr + e_entry");
    CHECK(res.load_addr == load_addr, "load_addr recorded correctly");
    CHECK(res.load_end == load_addr + 0x60u + 8u, "load_end accounts for filesz + bss (memsz)");

    /* --- bss zero-fill --- */
    CHECK(iop_mem_read32(st, load_addr + 0x60u) == 0 && iop_mem_read32(st, load_addr + 0x64u) == 0,
          "trailing bss bytes (memsz beyond filesz) are zero-filled");

    /* --- R_MIPS_26 relocation --- */
    {
        uint32_t w = iop_mem_read32(st, load_addr + 0);
        uint32_t expect = 0x08000000u | (((load_addr) >> 2) & 0x03FFFFFFu);
        CHECK(w == expect, "R_MIPS_26 (J instruction) rebased correctly to the real load address");
    }

    /* --- R_MIPS_32 relocation --- */
    {
        uint32_t w = iop_mem_read32(st, load_addr + 4);
        CHECK(w == 0x12345678u + load_addr, "R_MIPS_32 (raw pointer word) rebased correctly");
    }

    /* --- R_MIPS_HI16/LO16 pair --- */
    {
        uint32_t combined_before = (0x0022u << 16) + 0x4000u;
        uint32_t newval = combined_before + load_addr;
        uint32_t expect_hi = ((newval + 0x8000u) >> 16) & 0xFFFFu;
        uint32_t expect_lo = newval & 0xFFFFu;
        uint32_t hi_instr = iop_mem_read32(st, load_addr + 8);
        uint32_t lo_instr = iop_mem_read32(st, load_addr + 12);
        CHECK((hi_instr & 0xFFFFu) == expect_hi, "R_MIPS_HI16 field rebased correctly (paired with the following LO16)");
        CHECK((lo_instr & 0xFFFFu) == expect_lo, "R_MIPS_LO16 field rebased correctly");
        CHECK((hi_instr & 0xFFFF0000u) == 0x3C080000u, "R_MIPS_HI16's opcode/register bits are untouched by the relocation");
        CHECK((lo_instr & 0xFFFF0000u) == 0x35080000u, "R_MIPS_LO16's opcode/register bits are untouched by the relocation");
    }

    /* --- export table found --- */
    CHECK(res.export_count == 1, "exactly one real export table found");
    if (res.export_count == 1) {
        CHECK(strcmp(res.exports[0].name, "TESTMOD") == 0, "export table name parsed correctly");
        CHECK(res.exports[0].fptr_count == 2, "export table fptr count correct (null-terminated scan)");
        CHECK(res.exports[0].addr == load_addr + 16u, "export table address is the real, relocated IOP RAM address");
    }

    /* --- import table found --- */
    CHECK(res.import_count == 1, "exactly one real import table found");
    if (res.import_count == 1) {
        CHECK(strcmp(res.imports[0].name, "OTHRMOD") == 0, "import table name parsed correctly");
        CHECK(res.imports[0].stub_count == 2, "import table stub count correct (all-zero-terminated scan)");
    }

    /* --- malformed image is rejected, not guessed at --- */
    {
        uint8_t bad[64];
        memset(bad, 0, sizeof(bad));
        bad[0] = 'X'; /* bad magic */
        iop_elf_load_result_t res2;
        const char *err2 = NULL;
        int rc2 = iop_elf_load(st, bad, sizeof(bad), load_addr, &res2, &err2);
        CHECK(rc2 == -1 && err2 != NULL, "a bad ELF magic is rejected with a clear error, not silently accepted");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
