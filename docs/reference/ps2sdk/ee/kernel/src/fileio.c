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
 * EE FILE IO handling
 */

#include <tamtypes.h>
#include <ps2lib_err.h>
#include <kernel.h>
#include <sifrpc.h>
#define NEWLIB_PORT_AWARE
#include <fileio.h>
#include <string.h>
#include <iopcontrol.h>
#include <fileio-common.h>

extern SifRpcClientData_t _fio_cd;
extern int _fio_block_mode;
extern int _fio_io_sema;
extern int _fio_completion_sema;
extern int _fio_recv_data[512];
extern int _fio_intr_data[32];

#ifdef F_fio_init
int fioInit(void)
{
    int res;
    ee_sema_t sema;
    if (HasIopRebootedSinceLastCall())
        fioExit();

    if (_fio_cd.server)
        return 0;

    sceSifInitRpc(0);

    while (((res = sceSifBindRpc(&_fio_cd, 0x80000001, 0)) >= 0) &&
           (_fio_cd.server == NULL))
        nopdelay();

    if (res < 0)
        return res;

    sema.init_count      = 1;
    sema.max_count       = 1;
    sema.option          = 0;
    _fio_completion_sema = CreateSema(&sema);
    if (_fio_completion_sema < 0) {
        _fio_completion_sema = 0;
        return -E_LIB_SEMA_CREATE;
    }

    // Unofficial: create a locking semaphore to prevent a thread from overwriting another thread's return status.
    sema.init_count = 1;
    sema.max_count  = 1;
    sema.option     = 0;
    _fio_io_sema    = CreateSema(&sema);
    if (_fio_io_sema < 0) {
        _fio_io_sema = 0;
        return -E_LIB_SEMA_CREATE;
    }

    _fio_block_mode = FIO_WAIT;

    return 0;
}
#endif

#ifdef F_fio_open
int fioOpen(const char *name, int mode)
{
    struct _fio_open_arg arg;
    int res, result;

    if ((res = fioInit()) < 0)
        return res;

    WaitSema(_fio_io_sema);
    WaitSema(_fio_completion_sema);

    arg.mode = mode;
    strncpy(arg.name, name, sizeof(arg.name));
    arg.name[sizeof(arg.name) - 1] = 0;

    if ((res = sceSifCallRpc(&_fio_cd, FIO_F_OPEN, _fio_block_mode, &arg, sizeof arg,
                          _fio_recv_data, 4, (void *)_fio_intr, NULL)) >= 0) {
        result = (_fio_block_mode == FIO_NOWAIT) ? 0 : _fio_recv_data[0];
    } else {
        SignalSema(_fio_completion_sema);
        result = res;
    }

    SignalSema(_fio_io_sema);

    return result;
}
#endif

/* NOTE: the remaining ~20 real functions (fioClose/fioRead/fioWrite/fioLseek/
   fioIoctl/fioRemove/fioMkdir/fioRmdir/fioPutc/fioGetc/fioGets/fioDopen/
   fioDclose/fioDread/fioGetstat/fioChstat/fioFormat/fioSync/fioSetBlockMode/
   fioExit) all follow the exact same pattern seen in fioOpen above: acquire
   _fio_io_sema, acquire _fio_completion_sema (drained by the interrupt-mode
   sceSifCallRpc completion callback _fio_intr / _fio_read_intr), build a
   fixed-layout RPC arg struct, sceSifCallRpc() against the FILEIO server
   (sid 0x80000001) with a FIO_F_* function number, then release both
   semaphores. This is the real client-side counterpart to this project's
   Round 346 "wire rom0: FIO_F_OPEN/READ/CLOSE to real BIOS ROMDIR" work and
   Round 367's cdrom0:/cdrom1: ISO9660 FILEIO wiring - both were reverse-
   engineering the SERVER side of exactly this RPC protocol from BIOS
   disassembly; this file is the official client-side ground truth for the
   same wire format (FIO_F_OPEN/READ/WRITE/CLOSE/LSEEK/IOCTL/REMOVE/MKDIR/
   RMDIR/DOPEN/DCLOSE/DREAD/GETSTAT/CHSTAT/FORMAT function numbers). */
