/* Round 961 (task #887, user-directed continuation of Round 960's GT3
 * finding + explicit "vergiss nicht es ist disp2" reminder): dump the
 * IOP RAM window around pc=0x00155b40 - the address Round 960 found
 * the IOP core frozen at, unchanged across two independent 720,000,000-
 * instruction continuations of the fresh gt3_round960_freshboot.ckpt
 * checkpoint (itself a from-instruction-0 cold boot against the
 * current, Round-959-fixed tree, correctly superseding the stale
 * pre-Round-942-fix gt3_round861_fresh_chain.ckpt).
 *
 * Purpose: get real IOP pc/gpr/ra/sp state plus a RAM window wide
 * enough to disassemble the resting loop and its immediate callers,
 * so tools/round655-ee-disasm/disasm.c (a superset R5900 decoder -
 * the plain-MIPS-I opcode paths it shares with the IOP's R3000A are
 * exactly what this code needs, no EE-specific MMI/COP2/128-bit
 * opcodes expected in real IOP kernel code) can turn it into real
 * mnemonics instead of guessing from raw hex.
 *
 * The user's reminder that "it's disp2" is a scope note for the
 * eventual fix, not a new hypothesis about this dump: GS circuit
 * writes are EE-side (gs_mmio_write64, source/hw/gs.c) and the IOP
 * has no direct GS access on real hardware either - so this dump
 * stays IOP-RAM-only. The connection to DISP2 is that whatever real
 * condition the IOP is stuck spinning on is blocking the EE-side
 * restart cycle from ever reaching the point where it would write
 * the real organic DISPFB2/DISPLAY2 values (Round 942's documented
 * pmode=0x66 milestone) instead of staying pinned at Round 940/941's
 * synthetic forced override.
 *
 * Never touches tracked source; read-only diagnostic driver, matches
 * the r815_iop_halt_finegrain.c / r819_ckpt_disasm.c precedent.
 *
 * Usage: r961_iop_freeze_dump <bios> <disc> <ckpt_path> <center_addr_hex> <halfwidth_bytes> <out_file>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_intc.h"

int main(int argc, char **argv)
{
    if (argc < 7) {
        fprintf(stderr, "usage: %s <bios> <disc> <ckpt_path> <center_addr_hex> <halfwidth_bytes> <out_file>\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];
    uint32_t center = (uint32_t)strtoul(argv[4], NULL, 16);
    uint32_t halfwidth = (uint32_t)strtoul(argv[5], NULL, 10);
    const char *out_file = argv[6];

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);
    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) {
        fprintf(stderr, "checkpoint_load FAILED for %s\n", ckpt_path);
        return 1;
    }

    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    fprintf(stderr, "[R961] loaded %s: total_ee_instr=%llu ee_pc=0x%08x ee_halted=%d\n",
            ckpt_path, (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);
    fprintf(stderr, "[R961] iop: pc=0x%08x halted=%d halt_reason=\"%s\"\n",
            iop->pc, iop->halted, iop->halt_reason ? iop->halt_reason : "(none)");
    for (int r = 0; r < 32; r++) {
        fprintf(stderr, "[R961] iop.gpr[%d]=0x%08x", r, iop->gpr[r]);
        if ((r % 4) == 3) fprintf(stderr, "\n"); else fprintf(stderr, "  ");
    }
    fprintf(stderr, "[R961] iop.next_pc=0x%08x\n", iop->next_pc);

    /* Round 943 (task #931) already established that a frozen IOP pc in
     * this project means idle=1 (fetch/decode/execute deliberately
     * skipped, pc simply holds its last value) rather than a real
     * polling loop - and that round's own sched_ticks fix was meant to
     * let VBLANK wake the IOP again every ~1 NTSC frame. Checking idle/
     * sched_ticks/INTC state directly here answers the obvious follow-
     * up Round 943 never got to test at this depth: did that fix
     * actually keep working 1.44 BILLION EE instructions into a real
     * GT3 boot, or is idle stuck again for some other reason? */
    fprintf(stderr, "[R961] iop.idle=%u iop.sched_ticks=%llu\n",
            iop->idle, (unsigned long long)iop->sched_ticks);
    iop_intc_state_t *intc = iop_intc_get_state();
    fprintf(stderr, "[R961] intc: istat=0x%08x imask=0x%08x ictrl=0x%08x istat_hi=0x%08x imask_hi=0x%08x\n",
            intc->istat, intc->imask, intc->ictrl, intc->istat_hi, intc->imask_hi);
    fprintf(stderr, "[R961] iop.cop0[12](Status)=0x%08x cop0[13](Cause)=0x%08x cop0[14](EPC)=0x%08x "
            "exception_pending=%u\n",
            iop->cop0[12], iop->cop0[13], iop->cop0[14], iop->exception_pending);

    uint32_t start = center - halfwidth;
    uint32_t len = halfwidth * 2;
    FILE *f = fopen(out_file, "wb");
    if (!f) { fprintf(stderr, "open fail\n"); return 1; }
    for (uint32_t a = start; a < start + len; a += 4) {
        uint32_t w = iop_mem_read32(iop, a);
        fwrite(&w, 4, 1, f);
    }
    fclose(f);
    fprintf(stderr, "[R961] dumped IOP RAM 0x%08x..0x%08x (%u bytes) to %s\n",
            start, start + len, len, out_file);
    return 0;
}
