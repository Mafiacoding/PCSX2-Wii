/*
 * Round 509: user uploaded uLaunchELF's real BOOT.ELF and asked
 * whether this project's OWN emulator core can launch it - not real
 * PCSX2 this time. Direct continuation of the syscall-7 trampoline
 * methodology (Rounds 457-469, re-run in Round 502), but pointed at
 * a real, substantial third-party homebrew ELF (uLaunchELF v4.43a's
 * BOOT.ELF, 497108 bytes, real MIPS32 ET_EXEC, e_entry=0x01D0001C)
 * instead of a game disc's boot ELF or our own tiny diagnostic ELF
 * (Round 507/478's osdmenu.elf precedent).
 *
 * Loads the ELF directly via ee_elf_load() (no ISO/disc involved -
 * matches Round 478's osdmenu.elf approach) after the same organic
 * BIOS warm-up + VBLANK-END-mask trampoline recipe already evidenced
 * to work for real game code (Round 466-469, re-confirmed Round 502).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_cdvd.h"
#include "core/hw/iop_sio2.h"
#include "core/hw/ee_intc.h"
#include "core/hw/gs.h"
#include "core/ee_elf_loader.h"

static bios_image_t bios;
static uint8_t elf_buf[2 * 1024 * 1024];

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    uint64_t warmup_slices = (argc > 1) ? strtoull(argv[1], NULL, 10) : 40000000ull;
    uint64_t post_slices   = (argc > 2) ? strtoull(argv[2], NULL, 10) : 60000000ull;

    memset(&bios, 0, sizeof(bios));
    if (bios_load("/tmp/round238_diag/bios_fresh.bin", &bios) != 0) {
        fprintf(stderr, "[R509] bios load FAILED\n"); return 1;
    }
    if (system_init(&bios, &bios) != 0) {
        fprintf(stderr, "[R509] system_init FAILED\n"); return 1;
    }
    iop_sio2_pad_connect();
    iop_sio2_pad_press(IOP_PAD_BTN_CROSS);

    ee_state_t *ee = ee_core_get_state();

    fprintf(stderr, "[R509] warmup: running %llu slices to reach OSDSYS steady state\n",
        (unsigned long long)warmup_slices);
    system_run_interleaved((int)warmup_slices);
    fprintf(stderr, "[R509] warmup done: ee_pc=0x%08X instr=%llu\n",
        ee->pc, (unsigned long long)ee->instructions_executed);
    fprintf(stderr, "[R510] pad_cmd_count after warmup: %u\n", iop_sio2_get_pad_command_count());

    /* --- load uLaunchELF's real BOOT.ELF directly (no disc/ISO involved) --- */
    FILE *f = fopen("/tmp/round238_diag/ulaunchelf_boot.elf", "rb");
    if (!f) { fprintf(stderr, "[R509] fopen FAILED\n"); return 1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || (size_t)fsize > sizeof(elf_buf)) {
        fprintf(stderr, "[R509] bad file size %ld\n", fsize); fclose(f); return 1;
    }
    size_t nread = fread(elf_buf, 1, (size_t)fsize, f);
    fclose(f);
    if (nread != (size_t)fsize) {
        fprintf(stderr, "[R509] short read %zu != %ld\n", nread, fsize); return 1;
    }
    fprintf(stderr, "[R509] uLaunchELF BOOT.ELF read: %zu bytes\n", nread);

    {
        gs_state_t *gs0 = gs_get_state();
        fprintf(stderr, "[R509] GS state BEFORE trampoline: PMODE=0x%016llX DISPFB1=0x%016llX DISPLAY1=0x%016llX DISPFB2=0x%016llX DISPLAY2=0x%016llX\n",
            (unsigned long long)gs0->pmode, (unsigned long long)gs0->dispfb1, (unsigned long long)gs0->display1,
            (unsigned long long)gs0->dispfb2, (unsigned long long)gs0->display2);
    }

    ee_elf_load_result_t elf_res;
    const char *elf_err = NULL;
    if (ee_elf_load(ee, elf_buf, (uint32_t)nread, &elf_res, &elf_err) != 0) {
        fprintf(stderr, "[R509] ee_elf_load FAILED: %s\n", elf_err ? elf_err : "?");
        return 1;
    }
    fprintf(stderr, "[R509] ELF loaded: entry=0x%08X load_start=0x%08X load_end=0x%08X\n",
        elf_res.entry, elf_res.load_start, elf_res.load_end);

    /* Round 469's evidenced fix: mask VBLANK-END (bit 3) before firing the trampoline */
    ee_intc_state_t *intc = ee_intc_get_state();
    uint32_t mask_before = intc->mask;
    intc->mask &= ~(1u << 3);
    fprintf(stderr, "[R509] INTC_MASK before=0x%08X after=0x%08X (VBLANK-END bit 3 cleared)\n",
        mask_before, intc->mask);

    /* install trampoline: syscall instruction in scratchpad, real ExecPS2 calling convention */
    const uint32_t SCRATCH_BASE = 0x70000000u;
    const uint32_t SYSCALL_WORD = 0x0000000Cu;
    ee_mem_write32(ee, SCRATCH_BASE, SYSCALL_WORD);

    const char *fname = "mc0:/BOOT.ELF";
    uint32_t str_addr = SCRATCH_BASE + 0x10;
    uint32_t argv_addr = SCRATCH_BASE + 0x40;
    for (uint32_t i = 0; fname[i]; i++) {
        ee_mem_write8(ee, str_addr + i, (uint8_t)fname[i]);
    }
    ee_mem_write8(ee, str_addr + (uint32_t)strlen(fname), 0);
    ee_mem_write32(ee, argv_addr, str_addr);

    ee->gpr[3].ud0 = 7;              /* $v1 = 7 (_ExecPS2) */
    ee->gpr[4].ud0 = elf_res.entry;  /* $a0 = entry */
    ee->gpr[5].ud0 = 0;              /* $a1 = gp */
    ee->gpr[6].ud0 = 1;              /* $a2 = argc */
    ee->gpr[7].ud0 = argv_addr;      /* $a3 = argv pointer */
    ee->pc = SCRATCH_BASE;
    ee->next_pc = SCRATCH_BASE + 4;

    fprintf(stderr, "[R509] trampoline installed: v1=7 a0=0x%08X a1=0 a2=1 a3=0x%08X pc=0x%08X\n",
        elf_res.entry, argv_addr, ee->pc);

    ee_core_step();
    fprintf(stderr, "[R509] post-syscall-step: ee_pc=0x%08X\n", ee->pc);

    fprintf(stderr, "[R509] running %llu more slices in 10 sampled chunks\n", (unsigned long long)post_slices);
    uint64_t chunk = post_slices / 10;
    uint32_t gs_writes_before_total = 0;
    for (int i = 0; i < 10; i++) {
        system_run_interleaved((int)chunk);
        gs_state_t *gsx = gs_get_state();
        fprintf(stderr, "[R510]   chunk %d: ee_pc=0x%08X halted=%d instr=%llu PMODE=0x%llX DISPFB2=0x%llX DISPLAY2=0x%llX pad_cmd_count=%u\n",
            i, ee->pc, ee->halted, (unsigned long long)ee->instructions_executed,
            (unsigned long long)gsx->pmode, (unsigned long long)gsx->dispfb2, (unsigned long long)gsx->display2,
            iop_sio2_get_pad_command_count());
        if (ee->halted) break;
    }
    (void)gs_writes_before_total;

    fprintf(stderr, "\n========== ROUND 509 ULAUNCHELF TRAMPOLINE RESULT ==========\n");
    fprintf(stderr, "EE: halted=%d pc=0x%08X instr=%llu\n",
        ee->halted, ee->pc, (unsigned long long)ee->instructions_executed);
    if (ee->halted) {
        fprintf(stderr, "EE halt_reason: %s\n", ee->halt_reason);
    }
    gs_state_t *gs = gs_get_state();
    fprintf(stderr, "GS: PMODE=0x%016llX DISPFB1=0x%016llX DISPLAY1=0x%016llX DISPFB2=0x%016llX DISPLAY2=0x%016llX\n",
        (unsigned long long)gs->pmode, (unsigned long long)gs->dispfb1, (unsigned long long)gs->display1,
        (unsigned long long)gs->dispfb2, (unsigned long long)gs->display2);
    fprintf(stderr, "===================================================\n");

    return 0;
}
