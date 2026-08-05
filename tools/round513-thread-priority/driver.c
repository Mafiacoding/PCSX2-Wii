/*
 * driver.c - Round 513 survey: for each live IOP TCB, print its
 * status AND priority number (thbase.h convention: lower number is
 * more urgent), plus a listing of every loaded module's name/entry
 * point so a thread's likely owning module can be identified by
 * comparing entry-point ranges against the live PC oscillation
 * Round 512 already captured (0x00100000 / 0x80000080).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/dma.h"
#include "core/hw/iop_dma.h"
#include "core/hw/iop_hle_thread.h"
#include "core/hw/iop_module_loader.h"

static bios_image_t bios;

static const char *status_str(uint32_t s) {
    switch (s) {
        case 1: return "RUN";
        case 2: return "READY";
        case 4: return "WAIT";
        case 8: return "SUSPEND";
        case 0xC: return "WAITSUSPEND";
        case 0x10: return "DORMANT";
        default: return "?";
    }
}

int main(int argc, char **argv)
{
    const char *bios_path = argc > 1 ? argv[1] : "/tmp/round238_diag/bios_fresh.bin";
    uint64_t total_slices = argc > 2 ? strtoull(argv[2], NULL, 10) : 20000000ull;

    bios_load(bios_path, &bios);
    system_init(&bios, &bios);

    system_run_interleaved(total_slices);

    int n = iop_hle_thread_get_thread_count();
    int cur = iop_hle_thread_get_current_thread_id();
    printf("[R513] thread_count=%d current_thread_id=%d\n", n, cur);
    for (int i = 0; i <= n + 2; i++) {
        uint32_t st = iop_hle_thread_get_status(i);
        if (st != 0) {
            uint32_t prio = iop_hle_thread_get_priority(i);
            printf("[R513]   thread[%d] status=0x%x (%s) priority=%u\n", i, st, status_str(st), prio);
        }
    }

    iop_state_t *ist = iop_core_get_state();
    printf("[R513] live IOP pc=0x%08x\n", ist ? ist->pc : 0);

    int mc = iop_module_loader_get_module_count();
    printf("[R513] module_count=%d\n", mc);
    for (int i = 0; i < mc; i++) {
        const char *name = iop_module_loader_get_module_name(i);
        uint32_t entry = iop_module_loader_get_module_entry(i);
        printf("[R513]   module[%d] name='%s' entry=0x%08x\n", i, name ? name : "?", entry);
    }

    printf("[R513] sema_count=%d evf_count=%d alarm_count=%d sif2_xfer=%u\n",
           iop_hle_thread_get_sema_count(), iop_hle_thread_get_evf_count(),
           iop_hle_thread_get_alarm_count(), iop_dma_get_sif2_transfer_count());
    return 0;
}
