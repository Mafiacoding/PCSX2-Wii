/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# (C)2001, Gustavo Scotti (gustavo@scotti.com)
# (c) 2003 Marcus R. Brown <mrbrown@0xd6.org>
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
*/

/**
 * @file
 * EE Kernel prototypes
 */

#ifndef __KERNEL_H__
#define __KERNEL_H__

#include <stddef.h>
#include <stdarg.h>
#include <sifdma.h>
#include <mipscopaccess.h>

#define DI DIntr
#define EI EIntr

// Workaround for EE kernel bug: call this immediately before returning from any interrupt handler.
#define ExitHandler() __asm__ __volatile__("sync\nei\n")

// note: 'sync' is the same as 'sync.l'
#define EE_SYNC()  __asm__ __volatile__("sync")
#define EE_SYNCL() __asm__ __volatile__("sync.l")
#define EE_SYNCP() __asm__ __volatile__("sync.p")

#define UNCACHED_SEG(x) \
    ((void *)(((u32)(x)) | 0x20000000))

#define IS_UNCACHED_SEG(x) \
    (((u32)(x)) & 0x20000000)

#define UCAB_SEG(x) \
    ((void *)(((u32)(x)) | 0x30000000))

#define PUSHDATA(t, x, v, l) \
    *(t *)(x) = (v);         \
    (l)       = sizeof(t)

#define POPDATA(t, x, v, l) \
    (v) = *(t *)(x);        \
    (l) = sizeof(t)

#define ALIGNED(x) __attribute__((aligned((x))))

// GP functions
extern void *ChangeGP(void *gp);
extern void SetGP(void *gp);
extern void *GetGP(void);

extern void *_gp;
#define SetModuleGP() ChangeGP(&_gp)

#define TH_SELF 0

/** Limits */
#define MAX_THREADS    256 // A few will be used for the kernel patches. Thread 0 is always the idle thread.
#define MAX_SEMAPHORES 256 // A few will be used for the kernel patches.
#define MAX_PRIORITY   128
#define MAX_HANDLERS   128
#define MAX_ALARMS     64

/** Modes for FlushCache */
#define WRITEBACK_DCACHE  0
#define INVALIDATE_DCACHE 1
#define INVALIDATE_ICACHE 2
#define INVALIDATE_CACHE  3 // Invalidate both data & instruction caches.

/** EE Interrupt Controller (INTC) interrupt numbers */
enum {
    INTC_GS,
    INTC_SBUS,
    INTC_VBLANK_S,
    INTC_VBLANK_E,
    INTC_VIF0,
    INTC_VIF1,
    INTC_VU0,
    INTC_VU1,
    INTC_IPU,
    INTC_TIM0,
    INTC_TIM1,
    INTC_TIM2,
    INTC_SFIFO = 13,
    INTC_VU0WD
};

#define kINTC_GS           INTC_GS
#define kINTC_SBUS         INTC_SBUS
#define kINTC_VBLANK_START INTC_VBLANK_S
#define kINTC_VBLANK_END   INTC_VBLANK_E
#define kINTC_VIF0         INTC_VIF0
#define kINTC_VIF1         INTC_VIF1
#define kINTC_VU0          INTC_VU0
#define kINTC_VU1          INTC_VU1
#define kINTC_IPU          INTC_IPU
#define kINTC_TIMER0       INTC_TIM0
#define kINTC_TIMER1       INTC_TIM1

/** EE Direct Memory Access Controller (DMAC) interrupt numbers */
enum {
    DMAC_VIF0,
    DMAC_VIF1,
    DMAC_GIF,
    DMAC_FROM_IPU,
    DMAC_TO_IPU,
    DMAC_SIF0,
    DMAC_SIF1,
    DMAC_SIF2,
    DMAC_FROM_SPR,
    DMAC_TO_SPR,
    DMAC_CIS = 13,
    DMAC_MEIS,
    DMAC_BEIS,
};

#define INIT_DMAC 0x01
#define INIT_VU1  0x02
#define INIT_VIF1 0x04
#define INIT_GIF  0x08
#define INIT_VU0  0x10
#define INIT_VIF0 0x20
#define INIT_IPU  0x40

typedef struct t_ee_sema
{
    int count,
        max_count,
        init_count,
        wait_threads;
    u32 attr,
        option;
} ee_sema_t;

typedef struct t_ee_thread
{
    int status;           // 0x00
    void *func;           // 0x04
    void *stack;          // 0x08
    int stack_size;       // 0x0C
    void *gp_reg;         // 0x10
    int initial_priority; // 0x14
    int current_priority; // 0x18
    u32 attr;             // 0x1C
    u32 option;           // 0x20 Do not use - officially documented to not work.
} ee_thread_t;

/** Thread status */
#define THS_RUN         0x01
#define THS_READY       0x02
#define THS_WAIT        0x04
#define THS_SUSPEND     0x08
#define THS_WAITSUSPEND 0x0c
#define THS_DORMANT     0x10

/** Thread WAIT Status */
#define TSW_NONE  0 // Thread is not in WAIT state
#define TSW_SLEEP 1
#define TSW_SEMA  2

// sizeof() == 0x30
typedef struct t_ee_thread_status
{
    int status;           // 0x00
    void *func;           // 0x04
    void *stack;          // 0x08
    int stack_size;       // 0x0C
    void *gp_reg;         // 0x10
    int initial_priority; // 0x14
    int current_priority; // 0x18
    u32 attr;             // 0x1C
    u32 option;           // 0x20
    u32 waitType;         // 0x24
    u32 waitId;           // 0x28
    u32 wakeupCount;      // 0x2C
} ee_thread_status_t;

/* System call prototypes (subset relevant to boot/thread/SIF/alarm/TLB) */
extern void ResetEE(u32 init_bitfield);
extern void SetGsCrt(s16 interlace, s16 pal_ntsc, s16 field);
extern void KExit(s32 exit_code) __attribute__((noreturn));
extern void _LoadExecPS2(const char *filename, s32 num_args, char *args[]) __attribute__((noreturn));
extern s32 _ExecPS2(void *entry, void *gp, int num_args, char *args[]);
extern s32 ExecPS2(void *entry, void *gp, int num_args, char *args[]);
extern void LoadExecPS2(const char *filename, s32 num_args, char *args[]) __attribute__((noreturn));
extern void ExecOSD(int num_args, char *args[]) __attribute__((noreturn));
extern void Exit(s32 exit_code) __attribute__((noreturn));

extern s32 AddIntcHandler(s32 cause, s32 (*handler_func)(s32 cause), s32 next);
extern s32 AddIntcHandler2(s32 cause, s32 (*handler_func)(s32 cause, void *arg, void *addr), s32 next, void *arg);
extern s32 RemoveIntcHandler(s32 cause, s32 handler_id);
extern s32 AddDmacHandler(s32 channel, s32 (*handler)(s32 channel), s32 next);
extern s32 RemoveDmacHandler(s32 channel, s32 handler_id);
extern s32 _EnableIntc(s32 cause);
extern s32 _DisableIntc(s32 cause);

extern s32 CreateThread(ee_thread_t *thread);
extern s32 DeleteThread(s32 thread_id);
extern s32 StartThread(s32 thread_id, void *args);
extern void ExitThread(void);
extern void ExitDeleteThread(void);
extern s32 TerminateThread(s32 thread_id);
extern s32 ChangeThreadPriority(s32 thread_id, s32 priority);
extern s32 GetThreadId(void);
extern s32 ReferThreadStatus(s32 thread_id, ee_thread_status_t *info);
extern s32 SleepThread(void);
extern s32 WakeupThread(s32 thread_id);
extern s32 CancelWakeupThread(s32 thread_id);
extern s32 SuspendThread(s32 thread_id);
extern s32 ResumeThread(s32 thread_id);

extern void *SetupThread(void *gp, void *stack, s32 stack_size, void *args, void *root_func);
extern void SetupHeap(void *heap_start, s32 heap_size);
extern void *EndOfHeap(void);

extern s32 CreateSema(ee_sema_t *sema);
extern s32 DeleteSema(s32 sema_id);
extern s32 SignalSema(s32 sema_id);
extern s32 iSignalSema(s32 sema_id);
extern s32 WaitSema(s32 sema_id);
extern s32 PollSema(s32 sema_id);
extern s32 iPollSema(s32 sema_id);
extern s32 ReferSemaStatus(s32 sema_id, ee_sema_t *sema);

extern void *GetSyscallHandler(int syscall_no);
extern void *GetExceptionHandler(int except_no);
extern void *GetInterruptHandler(int intr_no);

#endif /* __KERNEL_H__ */
