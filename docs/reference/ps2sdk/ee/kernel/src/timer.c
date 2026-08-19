/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright ps2dev - http://www.ps2dev.org
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
*/

/**
 * @file
 * Some routines to do some timer work
 */

#include <kernel.h>
#include <timer.h>
#include <string.h>
#include <mipscopaccess.h>

#define TIMER_MODE_START 0x00000001
#define TIMER_MODE_HANDLER 0x00000002

typedef struct counter_struct_
{
    struct counter_struct_ *timer_next;
    struct counter_struct_ *timer_previous;
    vu32 timer_key;
    u32 timer_mode;
    u64 timer_base_time;
    u64 timer_base_count;
    u64 timer_schedule;
    timer_alarm_handler_t callback_handler;
    void *gp_value;
    void *callback_handler_arg;
    u32 padding[3];
} counter_struct_t __attribute__((aligned(64)));

typedef struct timer_ee_global_struct_
{
    vu64 timer_handled_count;
    s32 intc_handler;
    vu32 timer_counter_total;
    vu32 timer_counter_used;
    counter_struct_t *timer_counter_buf_free;
    counter_struct_t *timer_counter_buf_alarm;
    vs32 current_handling_timer_id;
} timer_ee_global_struct;

#define COUNTER_COUNT 128

#ifdef F_InitTimer
__attribute__((weak)) s32 InitTimer(s32 in_mode)
{
    s32 handler;
    u32 oldintr;
    u32 mode;

    if (g_Timer.intc_handler > 0) {
        return 0x80008001; // EINIT
    }
    g_Timer.timer_handled_count = 0;
    g_Timer.timer_counter_used = 0;
    g_Timer.timer_counter_total = 1;
    g_Timer.current_handling_timer_id = -1;
    memset(g_CounterBuf, 0, sizeof(g_CounterBuf));
    g_Timer.timer_counter_buf_free = &g_CounterBuf[0];
    for (u32 i = 0; i < ((sizeof(g_CounterBuf) / sizeof(g_CounterBuf[0])) - 1); i += 1) {
        g_CounterBuf[i].timer_next = &g_CounterBuf[i + 1];
    }
    g_CounterBuf[(sizeof(g_CounterBuf) / sizeof(g_CounterBuf[0])) - 1].timer_next = NULL;
    g_Timer.timer_counter_buf_alarm = NULL;
    ForTimer_InitAlarm();
    handler = AddIntcHandler2(INTC_TIM2, TimerHandler_callback, 0, NULL);
    if (handler < 0) {
        return 0x80009021; // EINT_HANDLER
    }
    g_Timer.intc_handler = handler;
    oldintr = DIntr();
    mode = ((*T2_MODE) & (~0x3)) | in_mode;
    mode |= (1 << 9) | (1 << 8);
    if ((mode & (1 << 7)) == 0) {
        mode |= (1 << 7);
        mode |= (1 << 11) | (1 << 10);
        SetT2_COUNT(0);
        SetT2_COMP(0xFFFF);
    }
    SetT2_MODE(mode);
    EnableIntc(INTC_TIM2);
    if (oldintr != 0) {
        EIntr();
    }
    return 0;
}
#endif

#ifdef F_iGetTimerSystemTime
u64 iGetTimerSystemTime(void)
{
    u64 timer_handled_count, timer_system_time_now;
    u32 low, mode;

    low = *T2_COUNT;
    mode = *T2_MODE;
    timer_handled_count = g_Timer.timer_handled_count;
    if ((mode & (1 << 11)) != 0) {
        timer_handled_count += 1;
        low = *T2_COUNT;
    }
    timer_system_time_now = (timer_handled_count << 16) | low;
    timer_system_time_now = timer_system_time_now << ((mode & 3) << 2);
    return timer_system_time_now;
}

void _ps2sdk_init_timer_impl(void)
{
    InitTimer(2);
    StartTimerSystemTime();
}

void _ps2sdk_deinit_timer_impl(void)
{
    StopTimerSystemTime();
    EndTimer();
}
#endif

/* NOTE: the real file also defines EndTimer/GetTimerPreScaleFactor/
   StartTimerSystemTime/StopTimerSystemTime/SetNextComp/InsertAlarm_ForTimer/
   UnlinkAlarm_ForTimer/TimerHandler_callback (the real AddIntcHandler2(INTC_TIM2,...)
   ISR that drains the alarm-sorted linked list, invoking each expired
   counter's callback_handler and rescheduling or freeing it) plus
   AllocTimerCounter/FreeTimerCounter/StartTimerCounter/StopTimerCounter/
   SetTimerCount/GetTimerBaseTime/GetTimerCount/SetTimerHandler and the
   TimerBusClock2USec/TimerUSec2BusClock/TimerBusClock2Freq/TimerFreq2BusClock
   BUSCLK<->real-time conversion helpers. This is the real generic multi-
   counter alarm/timer subsystem built on top of the single real T2 hardware
   timer + one INTC_TIM2 handler - relevant ground truth for this project's
   own EE timer/alarm modeling (hw/ee_timers.c) and the Round 386
   CreateSema/WaitSema/DeleteSema-adjacent syscall-stub decoding work. */
