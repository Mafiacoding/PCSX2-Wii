/*
 * test_iop_hle_event_flags_alarm.c - host-native test for the Round
 * 390 thevent (EventFlags) and thbase Alarm HLE additions to
 * iop_hle_thread.c - see include/core/hw/iop_hle_thread.h's Round 390
 * addendum for the full design rationale and citations.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include "core/iop/iop_core.c"
#include "core/hw/iop_hle_thread.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static iop_state_t *fresh_state(void)
{
    static bios_image_t bios;
    memset(&bios, 0, sizeof(bios));
    bios.data = memalign(32, BIOS_MAX_SIZE);
    memset(bios.data, 0, BIOS_MAX_SIZE);
    bios.size = BIOS_MAX_SIZE;
    bios.loaded = 1;

    iop_core_init(&bios);
    return iop_core_get_state();
}

static void write_thread_param(iop_state_t *st, uint32_t addr, uint32_t entry, uint32_t stacksize, uint32_t priority)
{
    iop_mem_write32(st, addr + 0u, 0);
    iop_mem_write32(st, addr + 4u, 0);
    iop_mem_write32(st, addr + 8u, entry);
    iop_mem_write32(st, addr + 12u, stacksize);
    iop_mem_write32(st, addr + 16u, priority);
}

static void write_evf_param(iop_state_t *st, uint32_t addr, uint32_t attr, uint32_t bits)
{
    iop_mem_write32(st, addr + 0u, attr);
    iop_mem_write32(st, addr + 4u, 0);
    iop_mem_write32(st, addr + 8u, bits);
}

int main(void)
{
    /* --- Sentinel matching: real thevent (library, ordinal) pairs --- */
    {
        CHECK(iop_hle_thread_sentinel_for_import("thevent", 4) == IOP_HLE_THREAD_CREATEEVENTFLAG, "thevent#4 -> CreateEventFlag");
        CHECK(iop_hle_thread_sentinel_for_import("thevent", 8) == IOP_HLE_THREAD_CLEAREVENTFLAG, "thevent#8 -> ClearEventFlag");
        CHECK(iop_hle_thread_sentinel_for_import("thevent", 10) == IOP_HLE_THREAD_WAITEVENTFLAG, "thevent#10 -> WaitEventFlag");
        CHECK(iop_hle_thread_sentinel_for_import("thevent", 12) == 0, "thevent#12 real documented gap -> not resolved");
        CHECK(iop_hle_thread_sentinel_for_import("thevent", 14) == IOP_HLE_THREAD_IREFEREVENTFLAGSTATUS, "thevent#14 -> iReferEventFlagStatus");
        CHECK(iop_hle_thread_sentinel_for_import("thbase", 35) == IOP_HLE_THREAD_SETALARM, "thbase#35 -> SetAlarm");
        CHECK(iop_hle_thread_sentinel_for_import("thbase", 37) == IOP_HLE_THREAD_CANCELALARM, "thbase#37 -> CancelAlarm");
        CHECK(iop_hle_thread_sentinel_for_import("thbase", 39) == IOP_HLE_THREAD_USEC2SYSCLOCK, "thbase#39 -> USec2SysClock");
    }

    /* --- CreateEventFlag / ClearEventFlag real "keep mask" semantics --- */
    {
        iop_state_t *st = fresh_state();
        uint32_t param = 0x00050000u;
        write_evf_param(st, param, IOP_EA_MULTI, 0x0Fu /* initial bits 0b1111 */);
        st->gpr[4] = param; st->gpr[31] = 0x00040000u;
        int handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CREATEEVENTFLAG);
        CHECK(handled == 1, "CreateEventFlag sentinel recognized");
        int efid = (int)st->gpr[2];
        CHECK(efid == 1, "CreateEventFlag returns handle 1");
        CHECK(iop_hle_thread_get_evf_count() == 1, "one event flag now tracked");

        /* ClearEventFlag(ef, 0b0101) on currBits=0b1111 must KEEP only
         * the bits also set in the mask -> 0b0101 (uITRON clr_flg
         * "keep mask", confirmed via real ps2sdk thevent.c fetch this
         * round - NOT currBits &= ~mask, which would give 0b1010). */
        st->gpr[4] = (uint32_t)efid; st->gpr[5] = 0x05u; st->gpr[31] = 0x00040010u;
        handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CLEAREVENTFLAG);
        CHECK(handled == 1, "ClearEventFlag sentinel recognized");
        uint32_t info_addr = 0x00051000u;
        st->gpr[4] = (uint32_t)efid; st->gpr[5] = info_addr; st->gpr[31] = 0x00040020u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_REFEREVENTFLAGSTATUS);
        CHECK(iop_mem_read32(st, info_addr + 12u) == 0x05u, "ClearEventFlag real keep-mask semantics: currBits &= bits (0xF & 0x5 = 0x5)");
        CHECK(iop_mem_read32(st, info_addr + 8u) == 0x0Fu, "info.initBits unaffected by ClearEventFlag, still the original create-time value");
    }

    /* --- SetEventFlag / WaitEventFlag: real blocking + AND-mode wakeup --- */
    {
        iop_state_t *st = fresh_state();
        uint32_t param = 0x00050000u;
        write_evf_param(st, param, IOP_EA_MULTI, 0 /* initial bits 0 */);
        st->gpr[4] = param; st->gpr[31] = 0x00040000u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CREATEEVENTFLAG);
        int efid = (int)st->gpr[2];

        /* Establish root thread + a more-urgent waiter thread, same
         * proven pattern as test_iop_hle_thread.c's WaitSema block. */
        uint32_t tparam = 0x00050100u;
        write_thread_param(st, tparam, 0x00070000u, 0x1000u, 5u /* more urgent than root's default 64 */);
        st->gpr[4] = tparam; st->gpr[31] = 0x00040100u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CREATETHREAD);
        int waiter = (int)st->gpr[2];
        st->gpr[4] = (uint32_t)waiter; st->gpr[5] = 0; st->gpr[31] = 0x00040110u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_STARTTHREAD);
        CHECK(iop_hle_thread_get_current_thread_id() == waiter, "more-urgent waiter thread is live after StartThread");

        /* Waiter blocks on WaitEventFlag(ef, 0b011, WEF_AND|WEF_CLEAR, &resbits). */
        uint32_t resbits_addr = 0x00052000u;
        st->gpr[4] = (uint32_t)efid;
        st->gpr[5] = 0x03u;
        st->gpr[6] = IOP_WEF_AND | IOP_WEF_CLEAR;
        st->gpr[7] = resbits_addr;
        st->gpr[31] = 0x00070100u;
        int handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_WAITEVENTFLAG);
        CHECK(handled == 1, "WaitEventFlag sentinel recognized");
        CHECK(iop_hle_thread_get_status(waiter) == IOP_THS_WAIT, "waiter thread is WAIT (bits not satisfied yet)");
        CHECK(iop_hle_thread_get_current_thread_id() == 1, "blocking WaitEventFlag switches back to root");

        /* Root SetEventFlag(ef, 0b001) alone does not satisfy AND(0b011). */
        st->gpr[4] = (uint32_t)efid; st->gpr[5] = 0x01u; st->gpr[31] = 0x00040200u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_SETEVENTFLAG);
        CHECK(iop_hle_thread_get_status(waiter) == IOP_THS_WAIT, "single bit insufficient for AND-mode wait, waiter still blocked");

        /* Root SetEventFlag(ef, 0b010) completes the AND(0b011) pattern -> real wakeup + WEF_CLEAR zeroes ALL currBits. */
        st->gpr[4] = (uint32_t)efid; st->gpr[5] = 0x02u; st->gpr[31] = 0x00040210u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_SETEVENTFLAG);
        CHECK(iop_hle_thread_get_status(waiter) == IOP_THS_RUN, "AND(0b011) now satisfied - waiter woken and pre-empts (more urgent)");
        CHECK(iop_hle_thread_get_current_thread_id() == waiter, "SetEventFlag's wakeup causes a real pre-emptive switch back to the waiter");
        CHECK(st->pc == 0x00070100u, "resumed waiter's pc is exactly its own saved WaitEventFlag return address");
        CHECK(st->gpr[2] == 0, "resumed waiter sees WaitEventFlag's real success return value (0)");
        CHECK(iop_mem_read32(st, resbits_addr) == 0x03u, "resbits received the RAW currBits at match time (0b011), not just the requested subset");

        uint32_t info_addr = 0x00053000u;
        st->gpr[4] = (uint32_t)efid; st->gpr[5] = info_addr; st->gpr[31] = 0x00040220u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_REFEREVENTFLAGSTATUS);
        CHECK(iop_mem_read32(st, info_addr + 12u) == 0u, "WEF_CLEAR zeroed ALL of currBits on match, real thevent.c semantics (not just the matched bits)");
    }

    /* --- PollEventFlag: non-blocking, real KE_EVF_COND-style failure --- */
    {
        iop_state_t *st = fresh_state();
        uint32_t param = 0x00050000u;
        write_evf_param(st, param, IOP_EA_SINGLE, 0x01u);
        st->gpr[4] = param; st->gpr[31] = 0x00040000u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CREATEEVENTFLAG);
        int efid = (int)st->gpr[2];

        /* OR-mode poll for 0b10 against currBits 0b01 - no match. */
        st->gpr[4] = (uint32_t)efid; st->gpr[5] = 0x02u; st->gpr[6] = IOP_WEF_OR; st->gpr[7] = 0; st->gpr[31] = 0x00040100u;
        int handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_POLLEVENTFLAG);
        CHECK(handled == 1, "PollEventFlag sentinel recognized");
        CHECK((int32_t)st->gpr[2] == -1, "PollEventFlag returns real failure when condition unmet, no blocking occurs");

        /* OR-mode poll for 0b01 against currBits 0b01 - matches immediately. */
        st->gpr[4] = (uint32_t)efid; st->gpr[5] = 0x01u; st->gpr[6] = IOP_WEF_OR; st->gpr[7] = 0; st->gpr[31] = 0x00040110u;
        handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_POLLEVENTFLAG);
        CHECK(handled == 1, "PollEventFlag sentinel recognized (match case)");
        CHECK(st->gpr[2] == 0, "PollEventFlag returns real success (0) when OR condition is already met");
    }

    /* --- SetAlarm: real nested-call dispatch via tick(), $ra-rigged
     * return trampoline (same convention as iop_hle_intr.c's own
     * already-proven interrupt-handler dispatch) --- */
    {
        iop_state_t *st = fresh_state();
        /* Establish the root thread implicitly (any thread call). */
        uint32_t tparam = 0x00050000u;
        write_thread_param(st, tparam, 0x00060000u, 0x1000u, 64u);
        st->gpr[4] = tparam; st->gpr[31] = 0x00040000u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CREATETHREAD);

        uint32_t handler_addr = 0x00080000u;
        uint32_t common_val = 0xCAFEBABEu;
        st->gpr[4] = 1000u; /* 1000 usec */
        st->gpr[5] = handler_addr;
        st->gpr[6] = common_val;
        st->gpr[31] = 0x00040300u;
        int handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_SETALARM);
        CHECK(handled == 1, "SetAlarm sentinel recognized");
        int alarm_id = (int)st->gpr[2];
        CHECK(alarm_id == 1, "SetAlarm returns alarm id 1");
        CHECK(iop_hle_thread_get_alarm_count() == 1, "one alarm now tracked");

        /* Simulate normal instruction execution up to (and past) the
         * real 1000usec*33.8688MHz deadline; iop_hle_thread_tick()
         * (called once per real emulated IOP instruction, same site
         * as iop_timers_tick()) must genuinely redirect the live
         * register file into the handler exactly once. */
        uint32_t pre_dispatch_pc = 0x12345678u;
        st->pc = pre_dispatch_pc;
        st->next_pc = pre_dispatch_pc + 4u;
        int dispatched = 0;
        for (int i = 0; i < 50000; i++) {
            st->instructions_executed++;
            iop_hle_thread_tick(st);
            if (st->pc == handler_addr) { dispatched = 1; break; }
        }
        CHECK(dispatched == 1, "Alarm handler genuinely dispatched once its real deadline (usec * 33.8688MHz) passes");
        CHECK(st->gpr[31] == IOP_HLE_THREAD_ALARM_RETURN_TRAMPOLINE, "$ra rigged to the private Alarm return trampoline, real jal-equivalent convention");
        CHECK(st->gpr[4] == common_val, "$a0 = common, real alarm_callback_t(void *common) ABI");

        /* Handler runs (nothing to simulate - a real handler is just
         * guest code), then falls off via jr $ra into our trampoline
         * with $v0=0 ("do not reschedule", real SetAlarm return
         * convention). */
        st->gpr[2] = 0;
        handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_ALARM_RETURN_TRAMPOLINE);
        CHECK(handled == 1, "Alarm return trampoline recognized");
        CHECK(st->pc == pre_dispatch_pc, "execution resumes exactly where tick() originally interrupted it");
        CHECK(iop_hle_thread_get_alarm_count() == 0, "$v0=0 return frees the one-shot alarm slot, real semantics");
    }

    /* --- CancelAlarm: matched by (callback, common) pair, real signature --- */
    {
        iop_state_t *st = fresh_state();
        uint32_t tparam = 0x00050000u;
        write_thread_param(st, tparam, 0x00060000u, 0x1000u, 64u);
        st->gpr[4] = tparam; st->gpr[31] = 0x00040000u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CREATETHREAD);

        st->gpr[4] = 5000u; st->gpr[5] = 0x00090000u; st->gpr[6] = 0x11111111u; st->gpr[31] = 0x00040300u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_SETALARM);
        CHECK(iop_hle_thread_get_alarm_count() == 1, "alarm registered before cancel test");

        /* Wrong (callback, common) pair must NOT cancel it. */
        st->gpr[4] = 0x00090000u; st->gpr[5] = 0x22222222u; st->gpr[31] = 0x00040310u;
        int handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CANCELALARM);
        CHECK(handled == 1, "CancelAlarm sentinel recognized");
        CHECK((int32_t)st->gpr[2] == -1, "CancelAlarm with wrong common value real-fails, no match");
        CHECK(iop_hle_thread_get_alarm_count() == 1, "non-matching CancelAlarm leaves the alarm intact");

        /* Correct pair cancels it. */
        st->gpr[4] = 0x00090000u; st->gpr[5] = 0x11111111u; st->gpr[31] = 0x00040320u;
        handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CANCELALARM);
        CHECK(handled == 1, "CancelAlarm sentinel recognized (matching case)");
        CHECK(st->gpr[2] == 0, "CancelAlarm with the real matching (callback, common) pair succeeds");
        CHECK(iop_hle_thread_get_alarm_count() == 0, "cancelled alarm slot freed");
    }

    /* --- USec2SysClock / SysClock2USec: real round-trip conversion --- */
    {
        iop_state_t *st = fresh_state();
        uint32_t sysclock_addr = 0x00054000u;
        st->gpr[4] = 2000000u; /* 2,000,000 usec = 2 sec */
        st->gpr[5] = sysclock_addr;
        st->gpr[31] = 0x00040000u;
        int handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_USEC2SYSCLOCK);
        CHECK(handled == 1, "USec2SysClock sentinel recognized");

        uint32_t sec_addr = 0x00054100u, usec_addr = 0x00054104u;
        st->gpr[4] = sysclock_addr; st->gpr[5] = sec_addr; st->gpr[6] = usec_addr; st->gpr[31] = 0x00040010u;
        handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_SYSCLOCK2USEC);
        CHECK(handled == 1, "SysClock2USec sentinel recognized");
        CHECK(iop_mem_read32(st, sec_addr) == 2u, "round-trip usec->sysclock->usec correctly recovers 2 whole seconds");
        CHECK(iop_mem_read32(st, usec_addr) == 0u, "round-trip recovers exactly 0 leftover usec (no drift for a whole-second input)");
    }

    printf("\n%d failures\n", failures);
    return failures ? 1 : 0;
}
