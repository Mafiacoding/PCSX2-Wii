/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
*/

#include <kernel.h>
#include <stdio.h>

#define kprintf(args...) // sio_printf(args)

static int InitTLB32MB(void);

struct SyscallData
{
    int syscall;
    void *function;
};

static const struct SyscallData SysEntry[] = {
    {0x5A, &kCopy},
    {0x5B, (void *)0x80075000},
    {0x54, NULL}, // ???
    {0x55, NULL}, // PutTLBEntry
    {0x56, NULL}, // SetTLBEntry
    {0x57, NULL}, // GetTLBEntry
    {0x58, NULL}, // ProbeTLBEntry
    {0x59, NULL}, // ExpandScratchPad
};

extern char **_kExecArg;

extern unsigned char tlbsrc[];
extern unsigned int size_tlbsrc;

void *GetEntryAddress(int syscall);
void setup(int syscall, void *function);

__attribute__((weak))
void InitTLBFunctions(void)
{
    int i;

    setup(SysEntry[0].syscall, SysEntry[0].function);
    Copy((void *)0x80075000, tlbsrc, size_tlbsrc);
    FlushCache(0);
    FlushCache(2);
    setup(SysEntry[1].syscall, SysEntry[1].function);

    for (i = 3; i < 8; i++) {
        setup(SysEntry[i].syscall, GetEntryAddress(SysEntry[i].syscall));
    }

    _kExecArg = GetEntryAddress(3);
}

__attribute__((weak))
void InitTLB(void)
{
    if (GetMemorySize() == 0x2000000) {
        InitTLB32MB();
    } else {
        _InitTLB();
    }
}

/* NOTE: the real file also defines the kernel/default/extended TLB entry
   tables (0x0D + 0x12 + 0x08 = 0x27 entries) used to map the EE's KUSEG/
   KSEG windows onto the 32MB physical map, and InitTLB32MB() which programs
   them via _SetTLBEntry(). Table contents omitted here (pure hardware
   mapping constants, not behaviorally relevant to the boot-stall
   investigation this project is running) - see the live fetch quoted in
   STATUS.md Round 492 if needed again. */
