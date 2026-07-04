/*
 * iop_hle_bios.c - IOP BIOS syscall trap. See iop_hle_bios.h for the
 * full scope explanation (this is a generic PS1-style A0/B0/C0
 * dispatch mechanism, NOT a port of PCSX2's real, much more involved
 * IopBios.cpp - and specific function-number semantics are
 * deliberately not guessed at).
 */
#include "core/hw/iop_hle_bios.h"
#include <string.h>
#include <stdio.h>

static iop_hle_bios_state_t g_hle;

void iop_hle_bios_init(void)
{
    memset(&g_hle, 0, sizeof(g_hle));
}

iop_hle_bios_state_t *iop_hle_bios_get_state(void) { return &g_hle; }

static const char *table_name(uint32_t pc)
{
    switch (pc) {
        case IOP_HLE_TABLE_A0: return "A0";
        case IOP_HLE_TABLE_B0: return "B0";
        case IOP_HLE_TABLE_C0: return "C0";
        default: return "?";
    }
}

int iop_hle_bios_try_handle(iop_state_t *st, uint32_t pc)
{
    if (pc != IOP_HLE_TABLE_A0 && pc != IOP_HLE_TABLE_B0 && pc != IOP_HLE_TABLE_C0)
        return 0;

    uint32_t function = st->gpr[9];  /* $t1 - the real PS1/PS2 BIOS call convention register */
    uint32_t ra        = st->gpr[31]; /* $ra - where to return to */

    g_hle.calls_seen++;
    g_hle.last_table    = pc;
    g_hle.last_function = function;
    snprintf(g_hle.last_call_desc, sizeof(g_hle.last_call_desc),
             "%s table, function 0x%02X", table_name(pc), (unsigned int)function);

    /* No specific function-number behavior is implemented (see the
     * header comment for why) - every call gets a generic default
     * return value of 0 in $v0 (r2), matching real MIPS calling
     * convention for a single-word return value, and control returns
     * to the caller exactly as if a real `JR $ra` had executed. */
    st->gpr[2] = 0; /* $v0 = 0 */

    st->pc      = ra;
    st->next_pc = ra + 4;

    return 1;
}
