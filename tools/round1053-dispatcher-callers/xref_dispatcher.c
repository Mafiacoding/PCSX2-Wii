/*
 * Round 1053 (task #447/#536/#1051/#1052 continuation): Round 1052
 * proved EXACTLY (per-instruction, non-sampled) that the real Sony
 * enclosing dispatcher function - whose prologue Round 1052's
 * disassembly bounded to a real entry point at IOP 0x00115B90
 * ("addiu $sp,$sp,-48; sw $s1,36($sp); addu $s1,$a0,$zero; ...",
 *  immediately following a sibling debug/assert-print helper's own
 *  "jr $ra; addiu $sp,$sp,32" return at 0x00115B88/8C) - is NEVER
 * fetched across a full 60,000,000-real-IOP-instruction execution
 * window from the Round 1045 steady-state checkpoint.
 *
 * This round asks: what real code, anywhere in IOP RAM, actually
 * calls 0x00115B90 (via a direct `jal`)? If real callers exist but
 * are themselves unreached, that pushes the investigation one level
 * further up the real call chain. If NO real jal caller exists at
 * all, that's a stronger signal the real invocation is indirect
 * (via a function-pointer table, e.g. an interrupt-vector slot) -
 * consistent with this being a scheduler-tick/interrupt-return hook
 * rather than a normally-called subroutine.
 */
#include <stdio.h>
#include <stdint.h>
#include "core/bios_loader.h"
#include "core/system.h"
#include "core/checkpoint.h"
#include "core/iop/iop_core.h"

static uint32_t rd(iop_state_t *iop, uint32_t addr) {
    addr &= 0x1FFFFF;
    if (addr + 4 > iop->ram_size) return 0xFFFFFFFFu;
    return (uint32_t)iop->ram[addr] | ((uint32_t)iop->ram[addr+1] << 8) |
           ((uint32_t)iop->ram[addr+2] << 16) | ((uint32_t)iop->ram[addr+3] << 24);
}

int main(int argc, char **argv)
{
    bios_image_t bios;
    bios_load(argv[1], &bios);
    checkpoint_load(argv[2], &bios, &bios, NULL);
    iop_state_t *iop = iop_core_get_state();

    uint32_t target = 0x00115B90u; /* real dispatcher entry point, Round 1052 */
    int found = 0;
    for (uint32_t a = 0; a + 4 <= iop->ram_size && a < 0x00200000u; a += 4) {
        uint32_t w = rd(iop, a);
        if ((w >> 26) == 0x03) { /* jal */
            uint32_t jt = (a & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
            if (jt == target) {
                printf("[R1053-XREF] jal 0x%08x found at 0x%08x\n", target, a);
                found++;
            }
        }
    }
    if (!found)
        printf("[R1053-XREF] NO direct jal callers of 0x%08x found anywhere in scanned IOP RAM (0x0-0x200000) - suggests indirect (function-pointer/vector-table) invocation.\n", target);
    else
        printf("[R1053-XREF] total direct jal callers of 0x%08x: %d\n", target, found);

    /* Also dump a small disassembly-relevant literal check: does
     * anything load the raw address 0x00115B90 as a 32-bit constant
     * (lui+addiu / or via a jump table slot value) anywhere in RAM,
     * which would indicate its address is stored in a function-
     * pointer table rather than called via direct jal? */
    int litfound = 0;
    for (uint32_t a = 0; a + 4 <= iop->ram_size && a < 0x00200000u; a += 4) {
        uint32_t w = rd(iop, a);
        if (w == target) {
            printf("[R1053-LIT] raw 32-bit literal 0x%08x found stored at IOP addr 0x%08x\n", target, a);
            litfound++;
            if (litfound > 20) break;
        }
    }
    if (!litfound)
        printf("[R1053-LIT] raw literal 0x%08x not found stored anywhere in scanned IOP RAM either.\n", target);

    return 0;
}
