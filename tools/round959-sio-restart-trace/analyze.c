/* Round 959 (task #952, SCPH-50004): reproduces the exact discovery
 * this round made - once ee_mem_read8()/ee_mem_write8()'s new SIO
 * MMIO-dispatch fix (ee_core.c) is in place, this project's own
 * Round-392 debug-console capture (ee_sio_get_console_text()) becomes
 * reachable by real BIOS code for the first time in this project's
 * history. Runs a diskless SCPH-50004 boot survey, prints the exact
 * ee_instr/sio_bytes_written/sio_console_len at every real LOADFILE
 * reply (ee_core_get_loadfile_reply_count() transition, Round 957),
 * then dumps the captured console text at the end.
 *
 * Expected real output (verified this round): a repeating
 * "# Restart Without Memory Clear." / "# Initialize GS ..." / ...
 * / "# Restart Without Memory Clear Done." block, alternating with an
 * occasional full "# Restart." / "# Initialize User Memory ..." /
 * "# Restart Done." cycle - the real PS2 BIOS boot ROM's own textual,
 * first-party account of a genuine, intentional, repeated internal
 * warm-reset sequence. This is the positive explanation for Rounds
 * 955-957's "13-16x LOADFILE reload" pattern: each BIOS Restart cycle
 * naturally ends with OSDSYS being reloaded fresh via LF_F_ELF_LOAD. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/ee/ee_core.h"
#include "core/hw/ee_sio.h"

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
    ee_state_t *ee = ee_core_get_state();
    uint64_t done = 0, chunk = 2000, last_rc = 0;

    while (done < budget && !ee->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        uint64_t rc = ee_core_get_loadfile_reply_count();
        if (rc != last_rc) {
            ee_sio_state_t *sio = ee_sio_get_state();
            printf("[R959] ee_instr=%llu loadfile_reply %llu->%llu  sio_bytes_written=%u sio_console_len=%u\n",
                   (unsigned long long)ee->instructions_executed,
                   (unsigned long long)last_rc, (unsigned long long)rc,
                   sio->bytes_written, sio->console_len);
            last_rc = rc;
        }
    }

    ee_sio_state_t *sio = ee_sio_get_state();
    printf("\n[R959] FINAL sio_bytes_written=%u sio_console_len=%u\n",
           sio->bytes_written, sio->console_len);
    printf("[R959] captured console text (last 2000 chars shown):\n");
    const char *txt = ee_sio_get_console_text();
    size_t len = txt ? strlen(txt) : 0;
    size_t start = len > 2000 ? len - 2000 : 0;
    printf("%s\n", txt ? txt + start : "(null)");
    return 0;
}
