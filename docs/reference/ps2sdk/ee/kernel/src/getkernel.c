/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
*/

/**
 * @file
 * Routines for accessing the EE's on-chip serial port.
 */

#include <kernel.h>

#ifdef F_GetSyscallHandler
static u32 *g_pSyscallTable = NULL;

static void InitSyscallTable(void)
{
    u32 oldintr, oldop;
    u32 *pAddr;

    oldintr         = DIntr();
    oldop           = ee_set_opmode(0);
    pAddr           = (u32 *)0x800002f0;
    g_pSyscallTable = (u32 *)((pAddr[0] << 16) | (pAddr[2] & 0xFFFF));
    ee_set_opmode(oldop);
    if (oldintr) {
        EIntr();
    }
}

void *GetSyscallHandler(int syscall_no)
{
    u32 oldintr, oldop;
    u32 addr = 0;

    if (g_pSyscallTable == NULL) {
        InitSyscallTable();
    }

    if (g_pSyscallTable != NULL) {
        oldintr = DIntr();
        oldop   = ee_set_opmode(0);
        addr    = g_pSyscallTable[syscall_no];
        ee_set_opmode(oldop);
        if (oldintr) {
            EIntr();
        }
    }

    return (void *)addr;
}
#endif

#ifdef F_GetExceptionHandler
extern void *GetSyscallHandler(int syscall_no);

void *GetExceptionHandler(int ex_cause_no)
{
    u32 oldintr, oldop;
    u32 addr, table_addr;
    u16 lo16, hi16;

    if ((ex_cause_no < 1) || (ex_cause_no > 15)) {
        return (NULL);
    }

    addr = (u32)GetSyscallHandler(13);

    oldintr = DIntr();
    oldop   = ee_set_opmode(0);

    lo16       = ((vu32 *)addr)[0x20 / 4];
    hi16       = ((vu32 *)addr)[0x14 / 4];
    table_addr = ((u32)(hi16 << 16) | lo16);

    addr = ((u32 *)table_addr)[ex_cause_no];

    ee_set_opmode(oldop);
    if (oldintr) {
        EIntr();
    }

    return ((void *)addr);
}
#endif

#ifdef F_GetInterruptHandler
extern void *GetSyscallHandler(int syscall_no);

void *GetInterruptHandler(int intr_cause_no)
{
    u32 oldintr, oldop;
    u32 addr;
    u16 lo16, hi16;

    if ((intr_cause_no < 0) || (intr_cause_no > 7)) {
        return (NULL);
    }

    addr = (u32)GetSyscallHandler(15);

    oldintr = DIntr();
    oldop   = ee_set_opmode(0);

    hi16 = ((vu32 *)addr)[0x10 / 4];
    lo16 = ((vu32 *)addr)[0x1C / 4];

    addr = ((u32 *)((u32)(hi16 << 16) | lo16))[intr_cause_no];

    ee_set_opmode(oldop);
    if (oldintr) {
        EIntr();
    }

    return (void *)addr;
}
#endif
