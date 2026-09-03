/*
 * Round 830 (task #811): fresh cold-boot single-slice trace to
 * determine, empirically, whether GT3's real SIF0 DMAC completion
 * handler (0x0101D878, registered via a real AddDmacHandler(5, ...)
 * call - see docs/STATUS.md Round 829/830) ever actually runs, and
 * whether the real DMAC interrupt-pending condition
 * (dma_dmac_interrupt_pending(), which combines D_STAT's channel-5
 * status bit with its channel-5 enable bit, per dma.c/task #176) ever
 * becomes true. Round 829 initially misidentified the real
 * EnableDmac(channel) call site (confused it with a neighboring
 * sceSifSetReg stub); Round 830 corrected that via a clean re-scan
 * and confirmed EnableDmac(5) IS called correctly, matching
 * AddDmacHandler(5, 0x0101D878). This tool exists to settle, with
 * real interleaved EE+IOP execution (system_run_interleaved(1) per
 * slice, single-slice granularity so no transient state is missed),
 * whether the remaining gap is: (a) D_STAT's channel-5 status bit
 * never gets set (sceSifSetDma never truly completes for GT3's own
 * calls), (b) the status bit gets set but Cause.IP3/Status.IM3/IE/EXL
 * never align to actually deliver the interrupt, or (c) everything
 * fires correctly and 0x0101D878 genuinely runs.
 *
 * Fresh "start" cold boot only (no checkpoint dependency) - runs to a
 * budget comfortably past Round 826's established 58,594,303-
 * instruction thread-1-park baseline. Read-only: does not modify
 * ee_core.c/dma.c, only calls their existing public accessors
 * (ee_core_get_state(), dma_get_state(), dma_dmac_interrupt_pending()).
 *
 * Usage: r830_dmac_trace <bios_path> <disc_path> [budget_slices]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/ee/ee_hle_thread.h"
#include "core/hw/iop_cdvd.h"
#include "core/hw/dma.h"

#define EE_CAUSE_IP3 0x00000800u
#define TARGET_PC    0x0101D878u
#define SIF0_STAT_BIT (1u << 5)
#define SIF0_ENABLE_BIT (1u << (16 + 5))
#define GEN_VECTOR   0x80000200u
#define GEN_VECTOR_BEV 0xBFC00400u
#define REG_SITE_PC  0x0101D508u
#define FUNC_LO      0x0101D380u
#define FUNC_HI      0x0101D900u

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <disc_path> [budget_slices]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    uint64_t budget = argc > 3 ? strtoull(argv[3], NULL, 10) : 100000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12 /* CDVD_TYPE_PS2CD */);

    ee_state_t *ee = ee_core_get_state();
    dma_state_t *dma = dma_get_state();

    uint64_t handler_hits = 0;
    uint64_t status_bit_set_transitions = 0;
    uint64_t enable_bit_set_transitions = 0;
    uint64_t ip3_set_transitions = 0;
    uint64_t pending_true_transitions = 0;
    int prev_status_bit = 0, prev_enable_bit = 0, prev_ip3 = 0, prev_pending = 0;
    uint64_t first_status_bit_instr = 0, first_enable_bit_instr = 0;
    uint64_t first_ip3_instr = 0, first_pending_instr = 0, first_handler_instr = 0;
    uint64_t vector_hits = 0, first_vector_instr = 0;
    uint64_t reg_site_hits = 0, first_reg_site_instr = 0;
    uint64_t vector_bev_hits = 0, first_vector_bev_instr = 0;
    uint64_t func_range_hits = 0, first_func_range_instr = 0;
    int prev_at_vector = 0, prev_at_vector_bev = 0, prev_in_func_range = 0;

    /* system_run_interleaved()'s own per-call diagnostic printf (via
     * system_safe_printf(), a raw write(1,...)) fires every single
     * time this driver deliberately requests only 1 slice at a time
     * (its "hit slice cap before both cores halted" warning path) -
     * at real per-instruction granularity that's an enormous, useless
     * I/O flood that dominates wall-clock time. Silence fd 1 for the
     * stepping loop itself (real stdout diagnostic content, not
     * anything checkpoint/BIOS/disc-derived - safe to discard) and
     * restore it before this driver's own final summary printf calls. */
    int saved_stdout = dup(1);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, 1);

    uint64_t slice = 0;
    while (slice < budget && !ee->halted) {
        system_run_interleaved(1);
        slice++;

        if (ee->pc == TARGET_PC) {
            if (handler_hits == 0) first_handler_instr = ee->instructions_executed;
            handler_hits++;
        }

        int status_bit = (dma->d_stat & SIF0_STAT_BIT) ? 1 : 0;
        int enable_bit = (dma->d_stat & SIF0_ENABLE_BIT) ? 1 : 0;
        int ip3 = (ee->cop0[13] & EE_CAUSE_IP3) ? 1 : 0;
        int pending = dma_dmac_interrupt_pending() ? 1 : 0;

        if (status_bit && !prev_status_bit) { status_bit_set_transitions++; if (!first_status_bit_instr) first_status_bit_instr = ee->instructions_executed; }
        if (enable_bit && !prev_enable_bit) { enable_bit_set_transitions++; if (!first_enable_bit_instr) first_enable_bit_instr = ee->instructions_executed; }
        if (ip3 && !prev_ip3) { ip3_set_transitions++; if (!first_ip3_instr) first_ip3_instr = ee->instructions_executed; }
        if (pending && !prev_pending) { pending_true_transitions++; if (!first_pending_instr) first_pending_instr = ee->instructions_executed; }

        prev_status_bit = status_bit;
        prev_enable_bit = enable_bit;
        prev_ip3 = ip3;
        prev_pending = pending;
    }

    if (saved_stdout >= 0) { dup2(saved_stdout, 1); close(saved_stdout); }
    if (devnull >= 0) close(devnull);

    printf("[R830-DMAC] ran %llu slices, total_instr=%llu pc=0x%08x halted=%u tid=%d\n",
           (unsigned long long)slice, (unsigned long long)ee->instructions_executed, ee->pc, ee->halted,
           ee_hle_thread_get_current_thread_id());
    printf("[R830-DMAC] handler(0x0101D878) hits=%llu first_at_instr=%llu\n",
           (unsigned long long)handler_hits, (unsigned long long)first_handler_instr);
    printf("[R830-DMAC] D_STAT ch5 STATUS bit: rising_edges=%llu first_at_instr=%llu final_state=%d (d_stat=0x%08x)\n",
           (unsigned long long)status_bit_set_transitions, (unsigned long long)first_status_bit_instr, prev_status_bit, dma->d_stat);
    printf("[R830-DMAC] D_STAT ch5 ENABLE bit: rising_edges=%llu first_at_instr=%llu final_state=%d\n",
           (unsigned long long)enable_bit_set_transitions, (unsigned long long)first_enable_bit_instr, prev_enable_bit);
    printf("[R830-DMAC] Cause.IP3: rising_edges=%llu first_at_instr=%llu final_state=%d (cop0[13]=0x%08x cop0[12]=0x%08x)\n",
           (unsigned long long)ip3_set_transitions, (unsigned long long)first_ip3_instr, prev_ip3, ee->cop0[13], ee->cop0[12]);
    printf("[R830-DMAC] dma_dmac_interrupt_pending(): rising_edges=%llu first_at_instr=%llu final_state=%d\n",
           (unsigned long long)pending_true_transitions, (unsigned long long)first_pending_instr, prev_pending);
    printf("[R830-DMAC] semaphore-5 signal count=%llu\n", (unsigned long long)ee_hle_thread_get_signal_calls(5));
    printf("[R830-DMAC] general exception vector (0x80000200) entries=%llu first_at_instr=%llu\n",
           (unsigned long long)vector_hits, (unsigned long long)first_vector_instr);
    printf("[R830-DMAC] AddDmacHandler registration site (0x%08x) hits=%llu first_at_instr=%llu\n",
           REG_SITE_PC, (unsigned long long)reg_site_hits, (unsigned long long)first_reg_site_instr);
    printf("[R830-DMAC] BEV interrupt vector (0x%08x) entries=%llu first_at_instr=%llu\n",
           GEN_VECTOR_BEV, (unsigned long long)vector_bev_hits, (unsigned long long)first_vector_bev_instr);
    printf("[R830-DMAC] enclosing func range [0x%08x,0x%08x) entries=%llu first_at_instr=%llu\n",
           FUNC_LO, FUNC_HI, (unsigned long long)func_range_hits, (unsigned long long)first_func_range_instr);

    if (ee->halted) {
        printf("[R830-DMAC] EE halted: %s\n", ee->halt_reason);
    }
    return 0;
}
