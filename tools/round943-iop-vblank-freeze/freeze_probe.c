/* Round 943 (task #447/#536 continuation): investigate the IOP freeze at
 * pc=0x00155910 observed on both the diskless BIOS boot and GT3 disc-boot
 * paths after Round 942's EE-side idle/EXL fix (IOP side was NOT touched
 * by that fix and has stayed frozen throughout all of Round 942/942b's
 * multi-billion-instruction EE surveys). This driver loads an existing
 * checkpoint at that frozen state and:
 *   1) dumps raw words around g_iop.pc (for manual R3000A/MIPS-I decode -
 *      no dedicated IOP disassembler exists in tools/ yet)
 *   2) prints g_iop.idle / exception_pending / cop0 Status+Cause + all
 *      GPRs at the frozen point
 *   3) single-steps the IOP core alone 200 times, logging pc/idle/$v0/$a0/
 *      $ra/cop0 Status+Cause every step, to see whether it's a static
 *      freeze (nothing changes - same class of bug as the old EE one) or
 *      a genuine polling loop (registers/pc cycle but the polled
 *      condition never resolves).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/ee/ee_core.h"
#include "core/iop/iop_core.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <bios_path> <ckpt_path>\n", argv[0]); return 1; }
    bios_image_t bios;
    if (bios_load(argv[1], &bios) != 0) { fprintf(stderr, "bios load fail\n"); return 1; }
    if (checkpoint_load(argv[2], &bios, &bios, NULL) != 0) { fprintf(stderr, "checkpoint_load fail\n"); return 1; }

    iop_state_t *iop = iop_core_get_state();
    ee_state_t  *ee  = ee_core_get_state();

    printf("[R943] initial: iop_pc=0x%08x idle=%d exception_pending=%d halted=%d\n",
           iop->pc, iop->idle, iop->exception_pending, iop->halted);
    printf("[R943] cop0: Status(12)=0x%08x Cause(13)=0x%08x EPC(14)=0x%08x PRId(15)=0x%08x\n",
           iop->cop0[12], iop->cop0[13], iop->cop0[14], iop->cop0[15]);
    printf("[R943] ee: pc=0x%08x idle=%d instructions_executed=%llu\n",
           ee->pc, ee->idle, (unsigned long long)ee->instructions_executed);

    printf("[R943] raw words around pc (pc-40..pc+40):\n");
    for (int32_t off = -40; off <= 40; off += 4) {
        uint32_t addr = (uint32_t)((int64_t)iop->pc + off);
        uint32_t w = iop_mem_read32(iop, addr);
        printf("  [%s] 0x%08x: 0x%08x\n", off == 0 ? "PC" : "  ", addr, w);
    }

    printf("[R943] GPRs at frozen point:\n");
    static const char *names[32] = {
        "zero","at","v0","v1","a0","a1","a2","a3",
        "t0","t1","t2","t3","t4","t5","t6","t7",
        "s0","s1","s2","s3","s4","s5","s6","s7",
        "t8","t9","k0","k1","gp","sp","fp","ra"
    };
    for (int i = 0; i < 32; i++) {
        printf("  $%-4s=0x%08x%s", names[i], iop->gpr[i], (i % 4 == 3) ? "\n" : "  ");
    }

    printf("[R943] single-stepping IOP core alone 200 times:\n");
    for (int i = 0; i < 200; i++) {
        uint32_t pc_before = iop->pc;
        iop_core_step();
        if (i < 40 || i % 20 == 0 || iop->pc != pc_before) {
            printf("[R943STEP] step=%d pc=0x%08x idle=%d exc_pend=%d v0=0x%08x a0=0x%08x a1=0x%08x ra=0x%08x status=0x%08x cause=0x%08x\n",
                   i, iop->pc, iop->idle, iop->exception_pending,
                   iop->gpr[2], iop->gpr[4], iop->gpr[5], iop->gpr[31],
                   iop->cop0[12], iop->cop0[13]);
        }
        if (iop->halted) { printf("[R943] IOP halted: %s\n", iop->halt_reason); break; }
    }

    printf("[R943] final: pc=0x%08x idle=%d\n", iop->pc, iop->idle);
    return 0;
}
