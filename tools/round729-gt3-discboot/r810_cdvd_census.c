/*
 * Round 810 (task #810 continuation, per user's explicit "trace the
 * producer chain backward from thread 1's blocked operation - especially
 * the CDVD/disc-command path" instruction). Loads a checkpoint and dumps:
 *  - CDVD N/S-command dispatch counters (Round 732, permanent)
 *  - SIF RPC bind count (permanent)
 *  - AddIntcHandler log count (Round 736, permanent)
 *  - full EE HLE thread census (status/wait_type/wait_id/wakeup_count)
 * to check whether ANY disc command was ever dispatched by the time
 * thread 1 (WaitSema id=5) is the only thing left blocked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/hw/iop_cdvd.h"
#include "core/hw/sif.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <ckpt_path>\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(argv[2], &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    ee_state_t *ee = ee_core_get_state();
    printf("total_instr=%llu pc=0x%08x\n", (unsigned long long)ee->instructions_executed, ee->pc);

    printf("--- CDVD dispatch counters (Round 732, permanent) ---\n");
    printf("ncmd_call_count=%llu scmd_call_count=%llu last_ncmd_issued=%u last_scmd_issued=%u\n",
           (unsigned long long)iop_cdvd_get_ncmd_call_count(),
           (unsigned long long)iop_cdvd_get_scmd_call_count(),
           iop_cdvd_get_last_ncmd_issued(), iop_cdvd_get_last_scmd_issued());
    printf("status=0x%02x ready=0x%02x disc_type=0x%02x\n",
           iop_cdvd_get_status(), iop_cdvd_get_ready(), iop_cdvd_get_disc_type());

    printf("--- SIF RPC ---\n");
    printf("rpc_bind_count=%u rpc_bind_cd=0x%08x\n",
           sif_cmd_iop_get_rpc_bind_count(), sif_cmd_iop_get_rpc_bind_cd());

    printf("--- AddIntcHandler log (Round 736, permanent) ---\n");
    uint32_t n = ee_core_get_addintc_log_count();
    printf("entries=%u\n", n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cause, handler_addr, next, call_pc;
        if (ee_core_get_addintc_log_entry(i, &cause, &handler_addr, &next, &call_pc)) {
            printf("  #%u cause=%u handler_addr=0x%08x next=0x%08x call_pc=0x%08x\n",
                   i, cause, handler_addr, next, call_pc);
        }
    }

    printf("--- EE HLE thread census ---\n");
    uint32_t tcount = ee_hle_thread_get_thread_count();
    uint32_t cur = ee_hle_thread_get_current_thread_id();
    printf("thread_count=%u current_thread_id=%u\n", tcount, cur);
    for (uint32_t t = 1; t <= tcount; t++) {
        printf("  T%u status=0x%02x prio=%u wait_type=%u wait_id=%d entry=0x%08x saved_pc=0x%08x\n",
               t, ee_hle_thread_get_status(t), ee_hle_thread_get_priority(t),
               ee_hle_thread_get_wait_type(t), ee_hle_thread_get_wait_id(t),
               ee_hle_thread_get_entry(t), ee_hle_thread_get_saved_pc(t));
        printf("      wakeup_count=%u wakeup_calls=%llu\n", ee_hle_thread_get_wakeup_count(t), (unsigned long long)ee_hle_thread_get_wakeup_calls(t));
    }
    return 0;
}
