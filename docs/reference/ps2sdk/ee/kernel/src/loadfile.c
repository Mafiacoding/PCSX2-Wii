/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# (C)2001, Gustavo Scotti (gustavo@scotti.com)
# (c) 2003 Marcus R. Brown (mrbrown@0xd6.org)
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
*/

/**
 * @file
 * IOP executable file loader API.
 * @defgroup loadfile EE LOADFILE: ELF and IRX loader client library.
 */

#include <tamtypes.h>
#include <ps2lib_err.h>
#include <kernel.h>
#include <sifrpc.h>
#include <string.h>
#include <iopcontrol.h>

#include <loadfile.h>
#include <iopheap.h>
#include <fcntl.h>
#include <unistd.h>

extern SifRpcClientData_t _lf_cd;

int _SifLoadElfPart(const char *path, const char *secname, t_ExecData *data, int fno);
int _SifLoadModuleBuffer(void *ptr, int arg_len, const char *args, int *modres);

#if defined(F_SifLoadFileInit)
SifRpcClientData_t _lf_cd;

int SifLoadFileInit()
{
    int res;

    if (HasIopRebootedSinceLastCall())
        SifLoadFileExit();

    if (_lf_cd.server)
        return 0;

    sceSifInitRpc(0);

    while ((res = sceSifBindRpc(&_lf_cd, 0x80000006, 0)) >= 0 && !_lf_cd.server)
        nopdelay();

    if (res < 0)
        return -E_SIF_RPC_BIND;

    return 0;
}
#endif

#if defined(F_SifLoadFileExit)
void SifLoadFileExit()
{
    memset(&_lf_cd, 0, sizeof _lf_cd);
}
#endif

#ifdef F__SifLoadModule
int _SifLoadModule(const char *path, int arg_len, const char *args, int *modres,
                   int fno, int dontwait)
{
    struct _lf_module_load_arg arg;

    if (SifLoadFileInit() < 0)
        return -SCE_EBINDMISS;

    memset(&arg, 0, sizeof arg);

    strlcpy(arg.path, path, sizeof(arg.path));

    if (args && arg_len) {
        arg.p.arg_len = arg_len > LF_ARG_MAX ? LF_ARG_MAX : arg_len;
        memcpy(arg.args, args, arg.p.arg_len);
    } else {
        arg.p.arg_len = 0;
    }

    if (sceSifCallRpc(&_lf_cd, fno, dontwait, &arg, sizeof arg, &arg, 8, NULL, NULL) < 0)
        return -SCE_ECALLMISS;

    if (modres)
        *modres = arg.modres;

    return arg.p.result;
}
#endif

#if defined(F_SifLoadModule)
int SifLoadModule(const char *path, int arg_len, const char *args)
{
    return _SifLoadModule(path, arg_len, args, NULL, LF_F_MOD_LOAD, 0);
}
#endif

/* NOTE: remaining exported wrappers (SifLoadStartModule, SifLoadModuleEncrypted,
   SifStopModule, SifUnloadModule, SifSearchModuleByName/Address, SifLoadElfPart/
   SifLoadElf/SifLoadElfEncrypted, SifIopSetVal/GetVal, SifLoadModuleBuffer/
   SifLoadStartModuleBuffer, SifExecModuleBuffer, SifExecModuleFile) all follow
   the same pattern: build a fixed-layout arg struct, call SifLoadFileInit(),
   then sceSifCallRpc() against the bound LOADFILE server (sid 0x80000006) with
   a specific LF_F_* function number. SifExecModuleFile in particular does
   open()+lseek() to size the file, SifAllocIopHeap(), SifLoadIopHeap() to DMA
   it into IOP RAM, then _SifLoadModuleBuffer() + SifFreeIopHeap() - this is
   the real client-side implementation this project's own _LoadExecPS2/osdmenu
   trampoline work (Rounds 457-478) was approximating from the EE side. */
