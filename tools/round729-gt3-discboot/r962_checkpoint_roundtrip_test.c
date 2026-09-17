/* Round 962 direct verification: does the new "IHLI" checkpoint block
 * actually round-trip iop_hle_intr.c's VBLANK/CDVD handler-registration
 * table through a real checkpoint_save()/checkpoint_load() cycle?
 *
 * Method: fresh cold boot far enough for real module code to call
 * RegisterIntrHandler (confirmed by r962_fresh_intr_trace.c to happen
 * by ee_instr~80,000,000 for irq=0/2/11/16/42/43), snapshot the
 * handler table, checkpoint_save() to a scratch file, then
 * checkpoint_load() the SAME file back into a second, freshly
 * system_init()'d process image and compare iop_hle_intr_get_intr_
 * handler(irq) for every irq 0-63 plus iop_hle_intr_get_stats()
 * before vs after. Exits nonzero (with a diagnostic) on any mismatch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/iop_hle_intr.h"
#include "core/hw/iop_cdvd.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios> <disc> <scratch_ckpt_path>\n", argv[0]);
        return 1;
    }
    const char *bios_path = argv[1];
    const char *disc_path = argv[2];
    const char *ckpt_path = argv[3];

    bios_image_t bios;
    if (bios_load(bios_path, &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);

    ee_state_t *ee = ee_core_get_state();
    iop_state_t *iop = iop_core_get_state();

    uint64_t done = 0;
    while (done < 90000000ull && !ee->halted && !iop->halted) {
        system_run_interleaved(10000000ull);
        done += 10000000ull;
    }

    uint32_t before[64];
    int nonzero_before = 0;
    for (int i = 0; i < 64; i++) {
        before[i] = iop_hle_intr_get_intr_handler(i);
        if (before[i] != 0) nonzero_before++;
    }
    const iop_hle_intr_stats_t *st_before = iop_hle_intr_get_stats();
    fprintf(stderr, "[R962RT] pre-save: nonzero_handlers=%d registered=%u dispatches=%u\n",
            nonzero_before, st_before->intr_handlers_registered, st_before->real_handler_dispatches);

    if (nonzero_before == 0) {
        fprintf(stderr, "[R962RT] FAIL: no handlers registered before save - test precondition not met\n");
        return 1;
    }

    if (checkpoint_save(ckpt_path) != 0) {
        fprintf(stderr, "[R962RT] FAIL: checkpoint_save failed\n");
        return 1;
    }
    fprintf(stderr, "[R962RT] checkpoint saved to %s\n", ckpt_path);

    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init (2nd) fail\n"); return 1; }
    if (iop_cdvd_mount_iso(disc_path) != 0) { fprintf(stderr, "disc mount (2nd) fail\n"); return 1; }
    iop_cdvd_set_disc_present(0x12);

    int nonzero_after_reinit = 0;
    for (int i = 0; i < 64; i++) if (iop_hle_intr_get_intr_handler(i) != 0) nonzero_after_reinit++;
    fprintf(stderr, "[R962RT] after fresh re-init (before load): nonzero_handlers=%d (expect 0)\n", nonzero_after_reinit);

    if (checkpoint_load(ckpt_path, &bios, &bios, disc_path) != 0) {
        fprintf(stderr, "[R962RT] FAIL: checkpoint_load failed\n");
        return 1;
    }

    int mismatches = 0;
    for (int i = 0; i < 64; i++) {
        uint32_t after = iop_hle_intr_get_intr_handler(i);
        if (after != before[i]) {
            fprintf(stderr, "[R962RT] MISMATCH irq=%d before=0x%08x after=0x%08x\n", i, before[i], after);
            mismatches++;
        }
    }
    const iop_hle_intr_stats_t *st_after = iop_hle_intr_get_stats();
    fprintf(stderr, "[R962RT] post-load: registered=%u (expect %u) dispatches=%u (expect %u)\n",
            st_after->intr_handlers_registered, st_before->intr_handlers_registered,
            st_after->real_handler_dispatches, st_before->real_handler_dispatches);
    if (st_after->intr_handlers_registered != st_before->intr_handlers_registered) mismatches++;
    if (st_after->real_handler_dispatches != st_before->real_handler_dispatches) mismatches++;

    if (mismatches == 0) {
        fprintf(stderr, "[R962RT] PASS: all %d nonzero handlers + stats survived save/load round-trip intact\n", nonzero_before);
        return 0;
    } else {
        fprintf(stderr, "[R962RT] FAIL: %d mismatch(es)\n", mismatches);
        return 1;
    }
}
