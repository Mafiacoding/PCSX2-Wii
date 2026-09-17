/* Round 962 (task #887/937/938, user-directed follow-up to Round 961's
 * "why does the VBLANK wake cycle do nothing" finding): does ANY real
 * GT3-loaded IOP module ever call RegisterIntrHandler for irq=0
 * (VBLANK_START) or irq=11 (VBLANK_END) - or any irq at all - during a
 * FRESH cold boot (system_init(), no checkpoint involved at all)?
 *
 * This is a necessary control test before trusting Round 961's
 * checkpoint-based finding (which showed intr_handler_addr[]-backed
 * dispatch never finding a real handler): source/core/checkpoint.c's
 * own block list (checked this round) saves/restores every other
 * hw-state struct (DMA0/EINT/ESIO/ETMR/GIF0/GS00/GSM0/IDMA/IEXC/IBIO/
 * IMOD/IINT/ITMR/ITHR/ICDV/IMLD/MCH0/SIF0/SIFX/VIF0/VIF1/VU10/EETH/
 * IHP1) but has NO block at all for iop_hle_intr.c's own static
 * registration table (include/core/hw/iop_hle_intr.h's
 * iop_hle_intr_get_intr_handler()/get_stats() accessors exist, but
 * nothing in checkpoint.c ever calls them to save/restore that
 * state). If a real module DID register a VBLANK handler during the
 * original cold boot, checkpoint_load() would silently wipe it before
 * Round 961 ever got to observe it - a checkpoint bug that would look
 * identical to "no module ever registers one", but with a completely
 * different (and fixable) root cause.
 *
 * Method: fresh system_init() (NOT checkpoint_load()), then poll
 * iop_hle_intr_get_stats() and iop_hle_intr_get_intr_handler(irq) for
 * every irq 0-63 at coarse chunk boundaries (10,000,000 slices, same
 * granularity chain_driver.c uses - acceptable here since we only need
 * to know WHETHER a registration ever happened and for which irq, not
 * catch a transient, so no fine-grain stepping needed for this
 * specific question), printing a diff whenever calls_seen or any
 * intr_handler_addr[irq] changes. Runs to the same ~1,440,000,000 EE-
 * instruction depth as Round 960's persisted checkpoint for a fair
 * comparison. Read-only, no tracked-source changes yet - this
 * determines whether one is warranted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_hle_intr.h"
#include "core/hw/iop_cdvd.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios> <disc> [budget_slices]\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    uint64_t budget = (argc >= 4) ? strtoull(argv[3], NULL, 10) : 180000000ull;

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);

    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    uint32_t last_addr[64];
    for (int i = 0; i < 64; i++) last_addr[i] = iop_hle_intr_get_intr_handler(i);
    uint32_t last_calls_seen = iop_hle_intr_get_stats()->calls_seen;
    uint32_t last_registered = iop_hle_intr_get_stats()->intr_handlers_registered;
    uint32_t last_released = iop_hle_intr_get_stats()->intr_handlers_released;
    uint32_t last_dispatched = iop_hle_intr_get_stats()->real_handler_dispatches;

    /* report any nonzero handler already present at t=0 (shouldn't be,
     * fresh boot, but check anyway) */
    for (int i = 0; i < 64; i++) {
        if (last_addr[i] != 0)
            fprintf(stderr, "[R962] t=0 irq=%d already has handler=0x%08x (unexpected)\n", i, last_addr[i]);
    }

    uint64_t done = 0;
    const uint64_t CHUNK = 10000000ull;
    while (done < budget && !ee->halted && !iop->halted) {
        system_run_interleaved(CHUNK);
        done += CHUNK;

        const iop_hle_intr_stats_t *st = iop_hle_intr_get_stats();
        if (st->calls_seen != last_calls_seen || st->intr_handlers_registered != last_registered ||
            st->intr_handlers_released != last_released || st->real_handler_dispatches != last_dispatched) {
            fprintf(stderr, "[R962STAT] slices=%llu ee_instr=%llu calls_seen=%u->%u registered=%u->%u "
                    "released=%u->%u dispatched=%u->%u\n",
                    (unsigned long long)done, (unsigned long long)ee->instructions_executed,
                    last_calls_seen, st->calls_seen, last_registered, st->intr_handlers_registered,
                    last_released, st->intr_handlers_released, last_dispatched, st->real_handler_dispatches);
            last_calls_seen = st->calls_seen;
            last_registered = st->intr_handlers_registered;
            last_released = st->intr_handlers_released;
            last_dispatched = st->real_handler_dispatches;
        }
        for (int i = 0; i < 64; i++) {
            uint32_t cur = iop_hle_intr_get_intr_handler(i);
            if (cur != last_addr[i]) {
                fprintf(stderr, "[R962IRQ] slices=%llu ee_instr=%llu irq=%d handler 0x%08x->0x%08x\n",
                        (unsigned long long)done, (unsigned long long)ee->instructions_executed,
                        i, last_addr[i], cur);
                last_addr[i] = cur;
            }
        }
    }

    fprintf(stderr, "\n[R962] FINAL slices=%llu ee_instr=%llu ee_halted=%d iop_halted=%d\n",
            (unsigned long long)done, (unsigned long long)ee->instructions_executed, ee->halted, iop->halted);
    const iop_hle_intr_stats_t *st = iop_hle_intr_get_stats();
    fprintf(stderr, "[R962] FINAL stats: calls_seen=%u intr_handlers_registered=%u intr_handlers_released=%u "
            "exc_handlers_registered=%u exc_handlers_released=%u default_exc_handlers_registered=%u "
            "real_handler_dispatches=%u\n",
            st->calls_seen, st->intr_handlers_registered, st->intr_handlers_released,
            st->exc_handlers_registered, st->exc_handlers_released,
            st->default_exc_handlers_registered, st->real_handler_dispatches);
    fprintf(stderr, "[R962] FINAL irq=0(VBLANK_START) handler=0x%08x irq=11(VBLANK_END) handler=0x%08x\n",
            iop_hle_intr_get_intr_handler(0), iop_hle_intr_get_intr_handler(11));
    fprintf(stderr, "[R962] FINAL nonzero handler table entries:\n");
    for (int i = 0; i < 64; i++) {
        uint32_t h = iop_hle_intr_get_intr_handler(i);
        if (h != 0) fprintf(stderr, "[R962]   irq=%d handler=0x%08x\n", i, h);
    }
    return 0;
}
