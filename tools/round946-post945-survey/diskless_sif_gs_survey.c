/* Round 946 (task #447/#536, follow-up to Round 945's delay-slot fix):
 * diskless-boot checkpoint-chain survey instrumented to answer the
 * user's two direct questions:
 *  (1) does the EE send new real SIF commands (SIF_MSCOM writes /
 *      RPC-bind / init-cmd traffic) to the IOP while the IOP holds
 *      pc=0x00155910 (Round 943's documented idle point)?
 *  (2) does the GS PMODE/DISPFB2/DISPLAY2/SMODE2 state ever change
 *      organically (i.e. before the Round 940/941 instr=25,000,000
 *      synthetic force-override fires) during the long EE run?
 * Reuses only already-existing, already-shipped read-only accessors
 * (sif_get_state(), sif_cmd_iop_get_init_cmd_count(),
 * sif_cmd_iop_get_rpc_bind_count(), gs_get_state()) - no tracked
 * source file is modified by this driver.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/gs.h"
#include "core/hw/sif.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios_path> <ckpt_path> <start|continue> [budget]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *ckpt_path = argv[2];
    const char *mode = argv[3];
    uint64_t budget = argc > 4 ? strtoull(argv[4], NULL, 10) : 200000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }

    if (strcmp(mode, "start") == 0) {
        if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    } else {
        if (checkpoint_load(ckpt_path, &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }
    }

    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();
    gs_state_t *gs = gs_get_state();
    sif_state_t *sif = sif_get_state();

    uint64_t chunk = 5000000ull, done = 0;
    uint32_t last_mscom = sif->mscom, last_smcom = sif->smcom;
    uint32_t last_msflag = sif->msflag, last_smflag = sif->smflag;
    uint64_t last_init_cmd = sif_cmd_iop_get_init_cmd_count();
    uint64_t last_bind = sif_cmd_iop_get_rpc_bind_count();
    uint64_t last_pmode = gs->pmode, last_dispfb2 = gs->dispfb2, last_display2 = gs->display2, last_smode2 = gs->smode2;
    uint32_t last_iop_pc = iop->pc;
    unsigned mscom_changes = 0, smcom_changes = 0, msflag_changes = 0, smflag_changes = 0;
    unsigned gs_changes_pre25M = 0, gs_changes_post25M = 0;
    unsigned iop_pc_excursions = 0;

    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;

        int mscom_ch = (sif->mscom != last_mscom);
        int smcom_ch = (sif->smcom != last_smcom);
        int msflag_ch = (sif->msflag != last_msflag);
        int smflag_ch = (sif->smflag != last_smflag);
        uint64_t init_cmd = sif_cmd_iop_get_init_cmd_count();
        uint64_t bind = sif_cmd_iop_get_rpc_bind_count();
        int gs_ch = (gs->pmode != last_pmode || gs->dispfb2 != last_dispfb2 ||
                     gs->display2 != last_display2 || gs->smode2 != last_smode2);
        int iop_moved = (iop->pc != last_iop_pc);

        if (mscom_ch) mscom_changes++;
        if (smcom_ch) smcom_changes++;
        if (msflag_ch) msflag_changes++;
        if (smflag_ch) smflag_changes++;
        if (gs_ch) { if (done <= 25000000ull) gs_changes_pre25M++; else gs_changes_post25M++; }
        if (iop_moved) iop_pc_excursions++;

        fprintf(stderr, "[R946] cum=%llu ee_pc=0x%08x ee_instr=%llu iop_pc=0x%08x iop_moved=%d "
                "mscom=0x%08x%s smcom=0x%08x%s msflag=0x%08x%s smflag=0x%08x%s init_cmd=%llu bind=%llu "
                "pmode=0x%02x dispfb2=0x%08x display2=0x%016llx smode2=0x%02x gs_ch=%d\n",
                (unsigned long long)done, ee->pc, (unsigned long long)ee->instructions_executed,
                iop->pc, iop_moved,
                sif->mscom, mscom_ch ? "*" : "", sif->smcom, smcom_ch ? "*" : "",
                sif->msflag, msflag_ch ? "*" : "", sif->smflag, smflag_ch ? "*" : "",
                (unsigned long long)init_cmd, (unsigned long long)bind,
                (unsigned)gs->pmode, (unsigned)gs->dispfb2, (unsigned long long)gs->display2,
                (unsigned)gs->smode2, gs_ch);

        last_mscom = sif->mscom; last_smcom = sif->smcom;
        last_msflag = sif->msflag; last_smflag = sif->smflag;
        last_init_cmd = init_cmd; last_bind = bind;
        last_pmode = gs->pmode; last_dispfb2 = gs->dispfb2; last_display2 = gs->display2; last_smode2 = gs->smode2;
        last_iop_pc = iop->pc;
    }

    printf("[R946-SUMMARY] total_instr=%llu ee_pc=0x%08x halted=%u iop_pc=0x%08x\n"
           "  mscom_changes=%u smcom_changes=%u msflag_changes=%u smflag_changes=%u\n"
           "  final init_cmd_count=%llu rpc_bind_count=%llu\n"
           "  gs_changes_before_25M(organic)=%u gs_changes_after_25M(incl.forced)=%u\n"
           "  iop_pc_excursions_from_sampled_value=%u\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted, iop->pc,
           mscom_changes, smcom_changes, msflag_changes, smflag_changes,
           (unsigned long long)last_init_cmd, (unsigned long long)last_bind,
           gs_changes_pre25M, gs_changes_post25M, iop_pc_excursions);

    if (ee->halted) {
        printf("[R946] EE halted: %s\n", ee->halt_reason);
        return 0;
    }
    if (checkpoint_save(ckpt_path) != 0) { fprintf(stderr, "checkpoint_save fail\n"); return 1; }
    printf("[R946] checkpoint saved to %s\n", ckpt_path);
    return 0;
}
