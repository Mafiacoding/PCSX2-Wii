/*
 * test_iop_hle_thread.c - host-native test for the Round 389 real
 * THREADMAN thread scheduler/semaphore HLE - see include/core/hw/
 * iop_hle_thread.h for the full design rationale.
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

static int stats_wait_blocked_at_least_one(void)
{
    return iop_hle_thread_get_stats()->wait_sema_blocked >= 1;
}

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

/* Helper: build a real iop_thread_t param struct (attr@0, option@4,
 * entry@8, stacksize@12, priority@16) at `addr` in guest RAM. */
static void write_thread_param(iop_state_t *st, uint32_t addr, uint32_t entry, uint32_t stacksize, uint32_t priority)
{
    iop_mem_write32(st, addr + 0u, 0);
    iop_mem_write32(st, addr + 4u, 0);
    iop_mem_write32(st, addr + 8u, entry);
    iop_mem_write32(st, addr + 12u, stacksize);
    iop_mem_write32(st, addr + 16u, priority);
}

static void write_sema_param(iop_state_t *st, uint32_t addr, int32_t initial, int32_t max, uint32_t attr)
{
    iop_mem_write32(st, addr + 0u, attr);
    iop_mem_write32(st, addr + 4u, 0);
    iop_mem_write32(st, addr + 8u, (uint32_t)initial);
    iop_mem_write32(st, addr + 12u, (uint32_t)max);
}

int main(void)
{
    /* --- Sentinel matching: real (library, ordinal) pairs --- */
    {
        CHECK(iop_hle_thread_sentinel_for_import("thbase", 4) == IOP_HLE_THREAD_CREATETHREAD, "thbase#4 -> CreateThread");
        CHECK(iop_hle_thread_sentinel_for_import("thbase", 6) == IOP_HLE_THREAD_STARTTHREAD, "thbase#6 -> StartThread");
        CHECK(iop_hle_thread_sentinel_for_import("thbase", 24) == IOP_HLE_THREAD_SLEEPTHREAD, "thbase#24 -> SleepThread");
        CHECK(iop_hle_thread_sentinel_for_import("thbase", 33) == IOP_HLE_THREAD_DELAYTHREAD, "thbase#33 -> DelayThread");
        CHECK(iop_hle_thread_sentinel_for_import("thbase", 35) == IOP_HLE_THREAD_SETALARM, "thbase#35 -> SetAlarm (Round 390: now implemented, no longer out of scope)");
        CHECK(iop_hle_thread_sentinel_for_import("thsemap", 8) == IOP_HLE_THREAD_WAITSEMA, "thsemap#8 -> WaitSema");
        CHECK(iop_hle_thread_sentinel_for_import("thsemap", 10) == 0, "thsemap#10 (real doc gap) -> no sentinel");
        CHECK(iop_hle_thread_sentinel_for_import("intrman", 4) == 0, "unrelated library -> no sentinel");
        CHECK(iop_hle_thread_sentinel_for_import(NULL, 4) == 0, "NULL module name -> no sentinel (no crash)");
    }

    /* --- CreateThread + StartThread + real context switch --- */
    {
        iop_state_t *st = fresh_state();

        /* Root thread (whatever's "already running") does the create/start calls. */
        uint32_t param_addr = 0x00050000u;
        write_thread_param(st, param_addr, 0x00060000u /* entry */, 0x1000u /* stacksize */, 10u /* priority - more urgent than root's default 64 */);

        st->gpr[4] = param_addr;
        st->gpr[31] = 0x00040100u;
        int handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CREATETHREAD);
        CHECK(handled == 1, "CreateThread sentinel recognized");
        int new_thid = (int)st->gpr[2];
        CHECK(new_thid == 2, "CreateThread returns thread id 2 (root thread implicitly created as id 1)");
        CHECK(iop_hle_thread_get_status(new_thid) == IOP_THS_DORMANT, "new thread starts DORMANT");
        CHECK(st->pc == 0x00040100u, "CreateThread returns to caller's ra");

        /* StartThread(new_thid, arg=0x1234) - higher priority (10) than
         * the root thread (64, per iop_hle_thread.c's own documented
         * default) - real semantics: this should IMMEDIATELY pre-empt
         * the root thread via a genuine context switch. */
        int root_thid = iop_hle_thread_get_current_thread_id();
        CHECK(root_thid == 1, "root thread is id 1 before StartThread");
        st->gpr[4] = (uint32_t)new_thid;
        st->gpr[5] = 0x1234u;
        st->gpr[31] = 0x00040200u; /* this ra is saved into the ROOT thread's TCB by the switch */
        handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_STARTTHREAD);
        CHECK(handled == 1, "StartThread sentinel recognized");
        CHECK(iop_hle_thread_get_current_thread_id() == new_thid, "StartThread causes a REAL context switch to the higher-priority new thread");
        CHECK(st->pc == 0x00060000u, "live pc now points at the new thread's real entry point");
        CHECK(st->gpr[4] == 0x1234u, "live $a0 holds the real arg passed to StartThread");
        CHECK(st->gpr[31] == IOP_HLE_THREAD_ENTRY_RETURN_TRAMPOLINE, "live $ra points at the entry-return trampoline");
        const iop_hle_thread_stats_t *stats = iop_hle_thread_get_stats();
        CHECK(stats->context_switches >= 1, "at least one real context switch was recorded");

        /* The new thread now calls ExitThread (falls off the end via
         * the trampoline) - execution must switch BACK to the root
         * thread, resuming exactly where it left off (ra=0x00040200). */
        handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_ENTRY_RETURN_TRAMPOLINE);
        CHECK(handled == 1, "entry-return trampoline recognized");
        CHECK(iop_hle_thread_get_status(new_thid) == IOP_THS_DORMANT, "exited thread is DORMANT");
        CHECK(iop_hle_thread_get_current_thread_id() == root_thid, "context switches back to the root thread after the child exits");
        CHECK(st->pc == 0x00040200u, "root thread resumes exactly at its saved return address");
    }

    /* --- CreateSema/WaitSema/SignalSema: real blocking + wakeup --- */
    {
        iop_state_t *st = fresh_state();
        uint32_t sema_param = 0x00050000u;
        write_sema_param(st, sema_param, 0 /* initial */, 1 /* max */, 0 /* SA_THFIFO */);

        st->gpr[4] = sema_param;
        st->gpr[31] = 0x00040300u;
        int handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CREATESEMA);
        CHECK(handled == 1, "CreateSema sentinel recognized");
        int semid = (int)st->gpr[2];
        CHECK(semid == 1, "CreateSema returns sema id 1");

        /* Create + start a second thread ("waiter") at LOWER urgency
         * (higher priority number) than the root thread, so starting
         * it does NOT pre-empt - both are then "ready"/"running"
         * candidates, root stays live. */
        uint32_t param_addr = 0x00050100u;
        write_thread_param(st, param_addr, 0x00070000u, 0x1000u, 100u);
        st->gpr[4] = param_addr;
        st->gpr[31] = 0x00040310u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CREATETHREAD);
        int waiter = (int)st->gpr[2];
        st->gpr[4] = (uint32_t)waiter;
        st->gpr[5] = 0;
        st->gpr[31] = 0x00040320u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_STARTTHREAD);
        CHECK(iop_hle_thread_get_current_thread_id() == 1, "lower-urgency waiter thread does not pre-empt root");

        /* Manually "switch into" the waiter by directly invoking
         * WaitSema as if the waiter thread's own code called it - to
         * do this validly we drive it through StartThread's already-
         * proven context switch instead: bump the waiter's priority
         * above root's and re-trigger via ChangeThreadPriority, which
         * performs a real reschedule(). */
        st->gpr[4] = (uint32_t)waiter;
        st->gpr[5] = 5u; /* more urgent than root's 64 */
        st->gpr[31] = 0x00040330u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_ICHANGETHREADPRIORITY);
        CHECK(iop_hle_thread_get_current_thread_id() == waiter, "ChangeThreadPriority reschedule switches to the now-more-urgent waiter");

        /* Waiter calls WaitSema(semid) on an empty (count=0) sema - must block. */
        st->gpr[4] = (uint32_t)semid;
        st->gpr[31] = 0x00070100u;
        handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_WAITSEMA);
        CHECK(handled == 1, "WaitSema sentinel recognized");
        CHECK(iop_hle_thread_get_status(waiter) == IOP_THS_WAIT, "waiter thread is now WAIT (blocked on empty sema)");
        CHECK(iop_hle_thread_get_current_thread_id() == 1, "blocking WaitSema switches back to the root thread");
        CHECK(stats_wait_blocked_at_least_one(), "wait_sema_blocked stat incremented");

        /* Root now SignalSema(semid) - must wake the waiter directly
         * (no count increment needed, since it hands off) and, being
         * more urgent than root, immediately pre-empt again. */
        st->gpr[4] = (uint32_t)semid;
        st->gpr[31] = 0x00040400u;
        handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_SIGNALSEMA);
        CHECK(handled == 1, "SignalSema sentinel recognized");
        CHECK(iop_hle_thread_get_status(waiter) == IOP_THS_RUN, "waiter is RUN again after SignalSema wakes it");
        CHECK(iop_hle_thread_get_current_thread_id() == waiter, "SignalSema's wakeup causes a real pre-emptive switch back to the waiter");
        CHECK(st->pc == 0x00070100u, "resumed waiter's pc is exactly its own saved WaitSema return address");
        CHECK(st->gpr[2] == 0, "resumed waiter sees WaitSema's real success return value (0)");
    }

    /* --- SleepThread / WakeupThread pairing, including the
     * pending-wakeup-arrives-before-sleep case --- */
    {
        iop_state_t *st = fresh_state();
        /* Establish the root thread implicitly via any thread call. */
        uint32_t param_addr = 0x00050000u;
        write_thread_param(st, param_addr, 0x00060000u, 0x1000u, 5u /* more urgent than root's default 64, so StartThread below actually switches into it */);
        st->gpr[4] = param_addr; st->gpr[31] = 0x00040500u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CREATETHREAD);
        int thid = (int)st->gpr[2];

        /* WakeupThread(thid) BEFORE the thread ever sleeps -> pending credit. */
        st->gpr[4] = (uint32_t)thid; st->gpr[31] = 0x00040510u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_WAKEUPTHREAD);

        /* Start it, then have it immediately SleepThread() - since a
         * wakeup credit is already pending, real semantics say this
         * must NOT block. */
        st->gpr[4] = (uint32_t)thid; st->gpr[5] = 0; st->gpr[31] = 0x00040520u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_STARTTHREAD);
        CHECK(iop_hle_thread_get_current_thread_id() == thid, "new equal/higher-priority thread is now live");
        int live_before = iop_hle_thread_get_current_thread_id();
        st->gpr[31] = 0x00060100u;
        int handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_SLEEPTHREAD);
        CHECK(handled == 1, "SleepThread sentinel recognized");
        CHECK(iop_hle_thread_get_current_thread_id() == live_before, "pending wakeup credit means SleepThread does NOT block");
        CHECK(st->pc == 0x00060100u, "SleepThread returns immediately to its own ra when a credit was pending");
    }

    /* --- DelayThread + tick-driven wakeup --- */
    {
        iop_state_t *st = fresh_state();
        uint32_t param_addr = 0x00050000u;
        write_thread_param(st, param_addr, 0x00060000u, 0x1000u, 5u /* more urgent than root's default 64 */);
        st->gpr[4] = param_addr; st->gpr[31] = 0x00040600u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CREATETHREAD);
        int thid = (int)st->gpr[2];
        st->gpr[4] = (uint32_t)thid; st->gpr[5] = 0; st->gpr[31] = 0x00040610u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_STARTTHREAD);
        CHECK(iop_hle_thread_get_current_thread_id() == thid, "started thread is live");

        st->gpr[4] = 1000u; /* 1000 usec */
        st->gpr[31] = 0x00060200u;
        int handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_DELAYTHREAD);
        CHECK(handled == 1, "DelayThread sentinel recognized");
        CHECK(iop_hle_thread_get_status(thid) == IOP_THS_WAIT, "delayed thread is WAIT");
        int root_after_delay = iop_hle_thread_get_current_thread_id();

        /* Tick until the deadline passes (33.8688 cycles/usec * 1000
         * usec ~= 33869 cycles - well within a generous loop bound). */
        int woke = 0;
        for (int i = 0; i < 50000; i++) {
            st->instructions_executed++;
            iop_hle_thread_tick(st);
            if (iop_hle_thread_get_status(thid) == IOP_THS_RUN) { woke = 1; break; }
        }
        CHECK(woke == 1, "delayed thread genuinely wakes up once its real deadline (usec * 33.8688MHz) passes");
        (void)root_after_delay;
    }

    /* --- ReferThreadStatus / ReferSemaStatus real field layout --- */
    {
        iop_state_t *st = fresh_state();
        uint32_t param_addr = 0x00050000u;
        write_thread_param(st, param_addr, 0x00060000u, 0x2000u, 42u);
        st->gpr[4] = param_addr; st->gpr[31] = 0x00040700u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CREATETHREAD);
        int thid = (int)st->gpr[2];

        uint32_t info_addr = 0x00051000u;
        st->gpr[4] = (uint32_t)thid;
        st->gpr[5] = info_addr;
        st->gpr[31] = 0x00040710u;
        int handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_REFERTHREADSTATUS);
        CHECK(handled == 1, "ReferThreadStatus sentinel recognized");
        CHECK(iop_mem_read32(st, info_addr + 8u) == IOP_THS_DORMANT, "info.status matches real THS_DORMANT bit");
        CHECK(iop_mem_read32(st, info_addr + 12u) == 0x00060000u, "info.entry matches real entry point");
        CHECK(iop_mem_read32(st, info_addr + 28u) == 42u, "info.initPriority matches real priority");
        CHECK(iop_mem_read32(st, info_addr + 32u) == 42u, "info.currentPriority matches real priority");

        uint32_t sema_param = 0x00052000u;
        write_sema_param(st, sema_param, 3, 5, 1 /* SA_THPRI */);
        st->gpr[4] = sema_param; st->gpr[31] = 0x00040720u;
        iop_hle_thread_try_handle(st, IOP_HLE_THREAD_CREATESEMA);
        int semid = (int)st->gpr[2];
        uint32_t sinfo_addr = 0x00053000u;
        st->gpr[4] = (uint32_t)semid; st->gpr[5] = sinfo_addr; st->gpr[31] = 0x00040730u;
        handled = iop_hle_thread_try_handle(st, IOP_HLE_THREAD_REFERSEMASTATUS);
        CHECK(handled == 1, "ReferSemaStatus sentinel recognized");
        CHECK(iop_mem_read32(st, sinfo_addr + 8u) == 3u, "sema info.initial matches");
        CHECK(iop_mem_read32(st, sinfo_addr + 12u) == 5u, "sema info.max matches");
        CHECK(iop_mem_read32(st, sinfo_addr + 16u) == 3u, "sema info.current matches initial count before any Wait/Signal");
    }

    printf("\n%d failures\n", failures);
    return failures ? 1 : 0;
}
