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
    uint64_t total_slices = argc > 2 ? strtoull(argv[2], NULL, 10) : 60000000ull;

    bios_load(bios_path, &bios);
    system_init(&bios, &bios);

    system_run_interleaved(total_slices);

    int n = iop_hle_thread_get_thread_count();
    int cur = iop_hle_thread_get_current_thread_id();
    printf("[R512] thread_count=%d current_thread_id=%d\n", n, cur);
    for (int i = 0; i <= n + 2; i++) {
        uint32_t st = iop_hle_thread_get_status(i);
        if (st != 0)
            printf("[R512]   thread[%d] status=0x%x (%s)\n", i, st, status_str(st));
    }
    printf("[R512] sema_count=%d evf_count=%d alarm_count=%d sif2_xfer=%u\n",
           iop_hle_thread_get_sema_count(), iop_hle_thread_get_evf_count(),
           iop_hle_thread_get_alarm_count(), iop_dma_get_sif2_transfer_count());
    return 0;
}
