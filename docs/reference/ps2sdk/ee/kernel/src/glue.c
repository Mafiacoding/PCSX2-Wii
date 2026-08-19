/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# (c) 2003 Marcus R. Brown (mrbrown@0xd6.org)
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
*/

/**
 * @file
 * EE kernel glue and utility routines.
 */

#include "kernel.h"
#include <mipscopaccess.h>

#ifdef F_DIntr
int DIntr()
{
    int eie, res;

    eie = get_mips_cop_reg(0, COP0_REG_Status);
    eie &= 0x10000;
    res = eie != 0;

    if (!eie)
        return 0;

    __asm__ (".p2align 3");
    do {
        __asm__ __volatile__("di");
        EE_SYNCP();
        eie = get_mips_cop_reg(0, COP0_REG_Status);
        eie &= 0x10000;
    } while (eie);

    return res;
}
#endif

#ifdef F_EIntr
int EIntr()
{
    int eie;

    eie = get_mips_cop_reg(0, COP0_REG_Status);
    eie &= 0x10000;
    __asm__ __volatile__("ei");

    return eie != 0;
}
#endif

/* NOTE: full file also defines EnableIntc/DisableIntc/EnableDmac/DisableDmac/
   SetAlarm/ReleaseAlarm/iEnableIntc/iDisableIntc/iEnableDmac/iDisableDmac/
   iSetAlarm/iReleaseAlarm/SyncDCache/iSyncDCache/InvalidDCache/iInvalidDCache -
   each a thin DI()/_underscore-syscall/EI() wrapper. See STATUS.md Round 492
   for the DIntr/EIntr excerpt used to cross-check ee_core.c's DI/EI modeling. */
