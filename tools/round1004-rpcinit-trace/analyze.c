/* Round 1004 (task #982, continuing Round 1003): Round 1003 found that
 * sif_cmd_iop_get_ee_recvbuf() already resolves to exactly 0x0040DA80 -
 * our long-hunted mailbox address - at the Round 990 resting point, with
 * init_cmd_count=2. Reading sif_cmd_iop_send_rpcinit_ready()'s real body
 * (ee_core.c:1967) shows it's a fully-implemented, unconditional 24-byte
 * synthetic SIF_CMD_SET_SREG(RPCINIT,1) packet writer - NOT a stub. Per
 * ee_core.c:5422's own corrected comment, the arm condition is actually
 * `init_cmd_count()==1` (the FIRST send), not the second as an older,
 * now-stale doc comment near the function itself still claims (this
 * project's own internal comment drift, not a functional bug - the
 * code and its adjacent, later comment agree with each other).
 *
 * On paper this entire chain should already have fired by the Round 990
 * resting point: arm on first INIT_CMD send, 200-instruction delay,
 * then an unconditional write of psize=24 into ee_recvbuf+0 (nonzero
 * byte 0, exactly what the dispatcher's `lbu v0,0(a3)` flag check
 * wants), cid=SIF_CMD_SET_SREG=0x80000001 (masked&0x7FFFFFFF=1 - the
 * exact index Round 995-999 found callback B registered at), sreg=
 * SIF_SREG_RPCINIT=0, val=1 - which callback B would write as
 * table[0]=1, unblocking the Round 990 poll loop. Yet Round 997 found
 * table[0..31] all zero at that same resting point. This tool
 * instruments (via a SCRATCH COPY of ee_core.c, per this project's
 * backup-before-experimenting rule - tracked source is untouched) the
 * five real call/decision sites in this chain to see, with ground
 * truth, whether it ever actually fires, and if so what happens next.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"
#include "core/hw/gs.h"

#define R990_TARGET_PC 0x0026fe9cu

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios_path> <budget>\n", argv[0]);
        return 1;
    }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (system_init(&bios, &bios) != 0) { fprintf(stderr, "system_init fail\n"); return 1; }
    uint64_t budget = strtoull(argv[2], NULL, 10);
    uint64_t chunk = 1000000ull, done = 0;
    ee_state_t *ee_probe = ee_core_get_state();
    int reached_target = 0;
    while (done < budget && !ee_probe->halted) {
        system_run_interleaved(chunk);
        done += chunk;
        if (ee_probe->pc == R990_TARGET_PC) { reached_target = 1; break; }
    }

    ee_state_t *ee = ee_core_get_state();
    printf("\n[R1004] reached_target_pc=%d ee_instr=%llu ee_pc=0x%08x halted=%d\n",
           reached_target, (unsigned long long)ee->instructions_executed, ee->pc, ee->halted);

    printf("\n[R1004] Final 32-slot table dump 0x0040dc80-0x0040dcfc:\n");
    for (uint32_t a = 0x0040dc80u; a < 0x0040dd00u; a += 4) {
        uint32_t v = ee_mem_read32(ee, a);
        printf("  table[%2u] (0x%08x) = 0x%08x%s\n", (a-0x0040dc80u)/4, a, v, v ? "  <-- NONZERO" : "");
    }

    printf("\n[R1004] Mailbox 0x0040DA80 struct raw bytes (first 24):\n");
    for (uint32_t a = 0x0040da80u; a < 0x0040da80u+24u; a += 4) {
        printf("  [0x%08x] = 0x%08x\n", a, ee_mem_read32(ee, a));
    }
    return 0;
}
