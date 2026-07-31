/*
 * test_ee_elf_loader.c - host-native test for the real EE-side ELF32/
 * MIPS game-boot loader (source/core/ee_elf_loader.c, Round 171, task
 * #172 continuation). See include/core/ee_elf_loader.h for the full
 * scope/citations.
 *
 * IMPORTANT: this test builds its own SYNTHETIC ELF32/MIPS ET_EXEC
 * image at runtime (below) - it does NOT embed any bytes extracted
 * from the user's real, copyrighted PS2 disc image. The synthetic
 * image's structure (ELF header, two PT_LOAD program headers) follows
 * the same public ELF32 spec citation tier already used by
 * tests/test_iop_elf.c, reproducing the FORMAT, not any real disc's
 * own bytes.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "core/ee_elf_loader.h"
#include "core/ee/ee_core.h"
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

/* Builds a small, synthetic ELF32/MIPS ET_EXEC image into `buf`
 * (caller-provided, must be at least 512 bytes) and returns its total
 * size. Two PT_LOAD segments at different, non-zero p_vaddrs (a real
 * game ELF's .text/.data typically load at 0x00100000+, unlike
 * test_iop_elf.c's IOP module which loads relative to a caller-
 * supplied load_addr) - one with filesz == memsz (no bss), one with
 * filesz < memsz (exercises the bss zero-fill path), to cover both
 * cases this loader (unlike iop_elf.c) does NOT apply any relocation
 * to - real ET_EXEC segments load at their literal p_vaddr. */
static uint32_t build_synthetic_exec(uint8_t *buf)
{
    memset(buf, 0, 512);

    const uint32_t eh_size = 52;
    const uint32_t ph_off = eh_size;
    const uint32_t ph_size = 32;
    const uint32_t ph_count = 2;
    const uint32_t seg0_off = ph_off + ph_count * ph_size; /* 116 */
    const uint32_t seg0_size = 32;
    const uint32_t seg1_off = seg0_off + seg0_size;        /* 148 */
    const uint32_t seg1_filesz = 16;

    const uint32_t seg0_vaddr = 0x00100000u;
    const uint32_t seg1_vaddr = 0x00101000u; /* separate page, no overlap */
    const uint32_t seg1_memsz = seg1_filesz + 24u; /* 24 bytes of bss */

    /* --- ELF header --- */
    buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 1; buf[5] = 1; buf[6] = 1; /* class=32, data=LE, version=1 */
    wle16(buf + 16, 2);       /* e_type = ET_EXEC */
    wle16(buf + 18, 8);       /* e_machine = EM_MIPS */
    wle32(buf + 20, 1);       /* e_version */
    wle32(buf + 24, seg0_vaddr + 0x20u); /* e_entry - somewhere inside segment 0 */
    wle32(buf + 28, ph_off);
    wle32(buf + 32, 0);       /* e_shoff - none, this loader doesn't need sections */
    wle32(buf + 36, 0);       /* e_flags */
    wle16(buf + 40, eh_size);
    wle16(buf + 42, ph_size);
    wle16(buf + 44, (uint16_t)ph_count);
    wle16(buf + 46, 0);       /* e_shentsize */
    wle16(buf + 48, 0);       /* e_shnum */
    wle16(buf + 50, 0);       /* e_shstrndx */

    /* --- program header 0: PT_LOAD, filesz == memsz (no bss) --- */
    wle32(buf + ph_off + 0, 1);            /* p_type = PT_LOAD */
    wle32(buf + ph_off + 4, seg0_off);     /* p_offset */
    wle32(buf + ph_off + 8, seg0_vaddr);   /* p_vaddr */
    wle32(buf + ph_off + 12, seg0_vaddr);  /* p_paddr */
    wle32(buf + ph_off + 16, seg0_size);   /* p_filesz */
    wle32(buf + ph_off + 20, seg0_size);   /* p_memsz */
    wle32(buf + ph_off + 24, 7);           /* p_flags */
    wle32(buf + ph_off + 28, 4);           /* p_align */

    /* --- program header 1: PT_LOAD, filesz < memsz (24 bytes bss) --- */
    wle32(buf + ph_off + ph_size + 0, 1);
    wle32(buf + ph_off + ph_size + 4, seg1_off);
    wle32(buf + ph_off + ph_size + 8, seg1_vaddr);
    wle32(buf + ph_off + ph_size + 12, seg1_vaddr);
    wle32(buf + ph_off + ph_size + 16, seg1_filesz);
    wle32(buf + ph_off + ph_size + 20, seg1_memsz);
    wle32(buf + ph_off + ph_size + 24, 7);
    wle32(buf + ph_off + ph_size + 28, 4);

    /* --- segment 0 contents: a recognizable pattern --- */
    for (uint32_t i = 0; i < seg0_size; i++)
        buf[seg0_off + i] = (uint8_t)(0xA0 + i);

    /* --- segment 1 contents (file part only - bss is implicit) --- */
    for (uint32_t i = 0; i < seg1_filesz; i++)
        buf[seg1_off + i] = (uint8_t)(0xB0 + i);

    return seg1_off + seg1_filesz; /* total image size */
}

int main(void)
{
    uint8_t image[512];
    uint32_t image_size = build_synthetic_exec(image);

    bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    ee_core_init(&bios);
    ee_state_t *st = ee_core_get_state();

    /* Real PS2 game ELFs load at KUSEG addresses (< 0x80000000),
     * which this project's own real, correct R5900 TLB (see
     * ee_core.c's ee_tlb_translate(), ported from PCSX2's own
     * COP0.cpp) requires a genuine TLB entry for - exactly like real
     * hardware, where the kernel's own earlier boot code sets up an
     * identity-mapped TLB entry for user memory before ever running
     * a KUSEG-resident program (EELOAD itself is one). This test
     * installs one such real, standard-format identity-mapped 4KB-
     * page-pair TLB entry (VPN2 covering 0x00100000-0x00101FFF,
     * PFN==VPN identity, V=D=G=1 - same flags-field convention
     * already used by tests/test_ee_cop0_tlb.c) covering both
     * synthetic segments below, matching real kernel practice rather
     * than bypassing this project's TLB model. */
    st->tlb[0].entry_hi  = 0x00100000u; /* VPN2 = 0x00100000 >> 13 */
    st->tlb[0].entry_lo0 = (0x00100000u >> 12 << 6) | 0x7u; /* PFN=0x100 (phys 0x100000), V=D=G=1 */
    st->tlb[0].entry_lo1 = (0x00101000u >> 12 << 6) | 0x7u; /* PFN=0x101 (phys 0x101000), V=D=G=1 */
    st->tlb[0].page_mask = 0;

    ee_elf_load_result_t res;
    const char *err = NULL;
    int rc = ee_elf_load(st, image, image_size, &res, &err);

    CHECK(rc == 0, "ee_elf_load() succeeds on a well-formed synthetic ET_EXEC image");
    CHECK(err == NULL, "no error string set on success");
    CHECK(res.entry == 0x00100020u, "entry point is the literal real e_entry (no relocation for ET_EXEC)");
    CHECK(res.load_start == 0x00100000u, "load_start is the lowest PT_LOAD p_vaddr");
    CHECK(res.load_end == 0x00101000u + 16u + 24u, "load_end accounts for the last segment's filesz+bss");

    /* --- segment 0 contents landed at the literal p_vaddr --- */
    {
        int ok = 1;
        for (uint32_t i = 0; i < 32; i++) {
            if (ee_mem_read8(st, 0x00100000u + i) != (uint8_t)(0xA0 + i)) { ok = 0; break; }
        }
        CHECK(ok, "segment 0's real bytes were written to EE RAM at its literal p_vaddr");
    }

    /* --- segment 1 file contents + bss zero-fill --- */
    {
        int ok = 1;
        for (uint32_t i = 0; i < 16; i++) {
            if (ee_mem_read8(st, 0x00101000u + i) != (uint8_t)(0xB0 + i)) { ok = 0; break; }
        }
        CHECK(ok, "segment 1's real file bytes were written to EE RAM at its literal p_vaddr");
    }
    {
        int ok = 1;
        for (uint32_t i = 16; i < 16 + 24; i++) {
            if (ee_mem_read8(st, 0x00101000u + i) != 0) { ok = 0; break; }
        }
        CHECK(ok, "segment 1's bss tail (memsz beyond filesz) is zero-filled");
    }

    /* --- loader does not touch pc/gpr - caller's responsibility (see header comment) --- */
    CHECK(st->pc == BIOS_RESET_VECTOR, "ee_elf_load() does not modify st->pc - matches real EELOAD (caller jumps)");

    /* --- malformed image is rejected, not guessed at --- */
    {
        uint8_t bad[64];
        memset(bad, 0, sizeof(bad));
        bad[0] = 'X'; /* bad magic */
        ee_elf_load_result_t res2;
        const char *err2 = NULL;
        int rc2 = ee_elf_load(st, bad, sizeof(bad), &res2, &err2);
        CHECK(rc2 == -1 && err2 != NULL, "a bad ELF magic is rejected with a clear error, not silently accepted");
    }

    /* --- a segment that would exceed EE RAM is rejected --- */
    {
        uint8_t huge[256];
        memset(huge, 0, sizeof(huge));
        huge[0] = 0x7F; huge[1] = 'E'; huge[2] = 'L'; huge[3] = 'F';
        huge[4] = 1; huge[5] = 1; huge[6] = 1;
        wle16(huge + 16, 2); wle16(huge + 18, 8); wle32(huge + 20, 1);
        wle32(huge + 24, 0);       /* e_entry */
        wle32(huge + 28, 52);      /* e_phoff */
        wle16(huge + 40, 52); wle16(huge + 42, 32); wle16(huge + 44, 1);
        wle32(huge + 52 + 0, 1);             /* p_type = PT_LOAD */
        wle32(huge + 52 + 4, 84);            /* p_offset */
        wle32(huge + 52 + 8, 0xFFFF0000u);   /* p_vaddr - deliberately near the top of a 32-bit range */
        wle32(huge + 52 + 16, 4);            /* p_filesz */
        wle32(huge + 52 + 20, 0x00200000u);  /* p_memsz - would overflow past EE RAM's real 32MB size */
        ee_elf_load_result_t res3;
        const char *err3 = NULL;
        int rc3 = ee_elf_load(st, huge, sizeof(huge), &res3, &err3);
        CHECK(rc3 == -1 && err3 != NULL, "a PT_LOAD segment that would exceed EE RAM is rejected, not silently truncated");
    }

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
