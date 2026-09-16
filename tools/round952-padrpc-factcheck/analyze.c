/* Round 952 (task #944 follow-up): fact-checks the user-relayed
 * "PMODE gated on padman/sio2man VBLANK-burst RPC" proposal against
 * this project's own real evidence, before implementing anything.
 *
 * The proposal claimed:
 *   (a) PADMAN's real SIF RPC server_id is 0x80000005
 *   (b) the EE's VBLANK-burst loop (Round 951) polls a padman RAM
 *       state field and defers SetGsCrt if the IOP pad reply is empty
 *   (c) a fix should intercept server_id==0x80000005 in a new
 *       source/core/sif/sif_rpc.c file
 *
 * This project's own already-shipped, ps2sdk-cited evidence
 * (include/core/hw/sif.h lines ~506-534, sourced directly from real
 * ee/rpc/pad/src/libpad.c) already documents PADMAN's REAL service
 * IDs as SIF_SID_PAD_BIND_ID1_OLD=0x8000010F and
 * SIF_SID_PAD_BIND_ID2_OLD=0x8000011F - NOT 0x80000005, which matches
 * no real PS2 SIF service cited anywhere in this project (LOADFILE=6,
 * IOPHEAP=3, FILEIO=1, MCSERV=0x400, SPU2DRV=0x601, three CDVD
 * services=0x592/0x593/0x595/0x59A - none are 0x80000005 either).
 * Real PAD_BIND dispatch has ALSO already been implemented since
 * Round 663-666 (ee_core.c ~line 5660), wired to the real iop_sio2.c
 * pad-connected state - so even if PADMAN were bound, this project
 * already has a real, evidenced handler for it, not a gap.
 *
 * This tool empirically settles the one remaining open question the
 * static citations can't answer by themselves: during the actual
 * SCPH-50004 diskless boot (Round 951's 1.2B-instruction survey),
 * does PADMAN (or anything) ever actually get RPC-bound at all? If
 * sif_cmd_iop_get_rpc_bind_count()==0 throughout, the entire proposed
 * causal chain (EE waits on an IOP pad-RPC reply) is empirically
 * impossible - there is no outstanding bind for the IOP to reply to
 * in the first place, exactly the same class of disproof used to
 * reject Round 949's MECHACON/SIO2-IP3 proposal.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/gs.h"
#include "core/hw/sif.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <budget>\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    uint64_t budget = strtoull(argv[2], NULL, 10);
    uint64_t chunk = 5000000ull, done = 0;
    ee_state_t *ee_probe = ee_core_get_state();

    uint32_t last_bind_count = 0;
    int saw_growth = 0;

    while (done < budget && !ee_probe->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        uint32_t bc = sif_cmd_iop_get_rpc_bind_count();
        if (bc != last_bind_count) {
            printf("[R952] bind_count changed %u -> %u at ee_instr=%llu (slice done=%llu)\n",
                   last_bind_count, bc, (unsigned long long)ee_probe->instructions_executed,
                   (unsigned long long)done);
            last_bind_count = bc;
            saw_growth = 1;
        }
    }

    ee_state_t  *ee  = ee_core_get_state();
    gs_state_t  *gs  = gs_get_state();
    uint32_t final_bind_count = sif_cmd_iop_get_rpc_bind_count();

    printf("\n[R952] FINAL: ee_instr=%llu ee_pc=0x%08x ee_halted=%d\n",
           (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);
    printf("[R952] FINAL: rpc_bind_count=%u (saw_growth_during_run=%d)\n",
           final_bind_count, saw_growth);
    printf("[R952] FINAL: gs pmode=0x%02x dispfb2=0x%08x display2=0x%016llx\n",
           gs->pmode, gs->dispfb2, (unsigned long long)gs->display2);
    printf("[R952] Real PADMAN sids per this project's own sif.h citation trail: "
           "SIF_SID_PAD_BIND_ID1_OLD=0x%08x SIF_SID_PAD_BIND_ID2_OLD=0x%08x "
           "(user proposal's claimed 0x80000005 matches NEITHER, and matches no "
           "other real cited sid in this project's table either)\n",
           (unsigned)SIF_SID_PAD_BIND_ID1_OLD, (unsigned)SIF_SID_PAD_BIND_ID2_OLD);
    printf("[R952] verdict: %s\n",
           final_bind_count == 0
               ? "rpc_bind_count==0 for the ENTIRE run - NO SIF service of any kind "
                 "(PADMAN or otherwise) is ever bound during this SCPH-50004 diskless "
                 "boot window. The proposal's causal chain (EE blocked waiting on an "
                 "IOP pad-RPC reply) is therefore empirically impossible here: there is "
                 "no outstanding RPC bind for any IOP-side handler to reply to."
               : "rpc_bind_count > 0 - dumping the live bind-sid table below to identify "
                 "which real service(s) were actually bound.");

    if (final_bind_count > 0) {
        uint32_t cds[8], sids[8];
        sif_cmd_iop_dump_bind_table(cds, sids, 8);
        int pad_seen = 0, loadfile_seen = 0, other_seen = 0;
        printf("[R952] live bind-sid table (up to last 8 distinct cd_ptrs seen):\n");
        for (int i = 0; i < 8; i++) {
            if (cds[i] == 0 && sids[i] == 0) continue;
            const char *label = "UNKNOWN/other";
            if (sids[i] == SIF_SID_LOADFILE) { label = "LOADFILE (0x80000006)"; loadfile_seen = 1; }
            else if (sids[i] == SIF_SID_PAD_BIND_ID1_OLD) { label = "PADMAN ID1_OLD (0x8000010F)"; pad_seen = 1; }
            else if (sids[i] == SIF_SID_PAD_BIND_ID2_OLD) { label = "PADMAN ID2_OLD (0x8000011F)"; pad_seen = 1; }
            else if (sids[i] == SIF_SID_MCSERV) { label = "MCSERV (0x80000400)"; other_seen = 1; }
            else if (sids[i] == SIF_SID_IOPHEAP) { label = "IOPHEAP (0x80000003)"; other_seen = 1; }
            else if (sids[i] == SIF_SID_CDVD_INIT) { label = "CDVD_INIT (0x80000592)"; other_seen = 1; }
            else if (sids[i] == SIF_SID_CDVD_NCMD) { label = "CDVD_NCMD (0x80000595)"; other_seen = 1; }
            else other_seen = 1;
            printf("  cd_ptr=0x%08x sid=0x%08x  [%s]\n", cds[i], sids[i], label);
        }
        printf("[R952] pad_bind_seen=%d loadfile_seen=%d other_seen=%d\n",
               pad_seen, loadfile_seen, other_seen);
        printf("[R952] final verdict on proposal: %s\n",
               pad_seen
                   ? "PADMAN WAS bound during this run - worth investigating further "
                     "(but still via the real 0x8000010F/0x8000011F sids, NOT the "
                     "proposal's fabricated 0x80000005)."
                   : "PADMAN was NEVER bound during this run - only other real services "
                     "were. The proposal's entire premise (EE blocked on an IOP pad-RPC "
                     "reply during the VBLANK burst) does not hold: there is no PADMAN "
                     "bind for any pad-RPC reply to even be pending on.");
    }
    return 0;
}
