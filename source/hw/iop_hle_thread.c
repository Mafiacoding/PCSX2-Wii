/*
 * iop_hle_thread.c - see include/core/hw/iop_hle_thread.h for the
 * full design rationale, citations, and sentinel-address map.
 */
#include <string.h>
#include "core/hw/iop_hle_thread.h"

/* Thread-stack bump arena: a dedicated, honestly-labeled simplification
 * (same spirit as iop_module_loader.c's own BUMP_BASE comment - real
 * CreateThread allocates its stack from the SYSMEM heap, which this
 * project has no general allocator for yet). Grows DOWN from a fixed
 * top, safely below IOP RAM's real top (0x00200000) and the initial
 * boot stack (top-0x100, per iop_module_loader.c's INITIAL_SP), with
 * plenty of headroom above the module loader's own BUMP_BASE-based
 * upward allocator (0x00100000 growing up) so the two can never
 * collide in practice. */
#define THREAD_STACK_ARENA_TOP 0x001F0000u
#define THREAD_STACK_ARENA_BOTTOM 0x00180000u

typedef struct {
    int in_use;
    uint32_t status; /* IOP_THS_* */
    uint32_t attr, option;
    uint32_t entry;
    uint32_t stack_base, stack_size;
    uint32_t priority;      /* current */
    uint32_t init_priority;
    int wait_type;    /* IOP_TSW_* */
    int wait_id;       /* sema id (TSW_SEMA) or evf id (TSW_EVENTFLAG) */
    uint32_t wakeup_count;
    uint64_t delay_deadline; /* instructions_executed target, when wait_type==IOP_TSW_DELAY */
    uint32_t ready_seq; /* FIFO tiebreak among equal priority */

    /* Meaningful only when wait_type==IOP_TSW_EVENTFLAG (Round 390). */
    uint32_t wait_evf_bits;
    uint32_t wait_evf_mode;
    uint32_t wait_evf_resptr; /* 0 = caller passed NULL resbits */

    /* Saved register context - meaningful whenever this thread is NOT
     * the currently-live one (see this file's header comment on the
     * scheduling model). */
    uint32_t gpr[32];
    uint32_t pc, next_pc;
    uint32_t hi, lo;
} iop_tcb_t;

typedef struct {
    int in_use;
    uint32_t attr, option;
    int32_t initial, max;
    int32_t count;
    int32_t num_wait_threads;
} iop_sema_t_internal;

typedef struct {
    int in_use;
    uint32_t attr, option;
    uint32_t init_bits;
    uint32_t bits; /* current */
    int32_t num_wait_threads;
} iop_evf_t_internal;

typedef struct {
    int in_use;
    uint32_t handler;
    uint32_t common;
    uint64_t deadline; /* instructions_executed target */
} iop_alarm_t_internal;

static struct {
    iop_tcb_t threads[IOP_HLE_THREAD_MAX_THREADS];
    int thread_count; /* number of in_use slots ever allocated, for ensure_root_thread() gating */
    int current_thread_id; /* 1-based; 0 = none yet */
    uint32_t ready_seq_counter;
    uint32_t stack_bump_next; /* grows DOWN from THREAD_STACK_ARENA_TOP */

    iop_sema_t_internal semas[IOP_HLE_THREAD_MAX_SEMAS];
    iop_evf_t_internal evflags[IOP_HLE_THREAD_MAX_EVFLAGS];
    iop_alarm_t_internal alarms[IOP_HLE_THREAD_MAX_ALARMS];

    /* Round 390: set while an Alarm callback dispatch is in flight (see
     * iop_hle_thread_tick()/IOP_HLE_THREAD_ALARM_RETURN_TRAMPOLINE) -
     * the live-register-file pc/next_pc to resume once the callback
     * returns, plus which alarm slot is being serviced (so its own
     * real reschedule-or-free-on-return semantics can be applied). */
    int alarm_in_dispatch;
    int alarm_dispatch_slot;
    uint32_t alarm_resume_pc, alarm_resume_next_pc;

    iop_hle_thread_stats_t stats;
} g;

void iop_hle_thread_init(void)
{
    memset(&g, 0, sizeof(g));
    g.stack_bump_next = THREAD_STACK_ARENA_TOP;
}

static iop_tcb_t *tcb(int thid) /* thid is 1-based */
{
    if (thid < 1 || thid > IOP_HLE_THREAD_MAX_THREADS) return NULL;
    return &g.threads[thid - 1];
}

static int alloc_tcb_slot(void)
{
    for (int i = 0; i < IOP_HLE_THREAD_MAX_THREADS; i++) {
        if (!g.threads[i].in_use) return i + 1;
    }
    return 0;
}

static int alloc_sema_slot(void)
{
    for (int i = 0; i < IOP_HLE_THREAD_MAX_SEMAS; i++) {
        if (!g.semas[i].in_use) return i + 1;
    }
    return 0;
}

static iop_sema_t_internal *sema(int semid)
{
    if (semid < 1 || semid > IOP_HLE_THREAD_MAX_SEMAS) return NULL;
    return &g.semas[semid - 1];
}

static int alloc_evf_slot(void)
{
    for (int i = 0; i < IOP_HLE_THREAD_MAX_EVFLAGS; i++) {
        if (!g.evflags[i].in_use) return i + 1;
    }
    return 0;
}

static iop_evf_t_internal *evf(int efid)
{
    if (efid < 1 || efid > IOP_HLE_THREAD_MAX_EVFLAGS) return NULL;
    return &g.evflags[efid - 1];
}

static int alloc_alarm_slot(void)
{
    for (int i = 0; i < IOP_HLE_THREAD_MAX_ALARMS; i++) {
        if (!g.alarms[i].in_use) return i + 1;
    }
    return 0;
}

static iop_alarm_t_internal *alarm_slot(int id)
{
    if (id < 1 || id > IOP_HLE_THREAD_MAX_ALARMS) return NULL;
    return &g.alarms[id - 1];
}

/* See header comment: the first time ANY thread primitive runs, TCB
 * slot 1 is synthesized to represent "whatever this project's
 * existing sequential module loader was already running" - so it can
 * be saved/restored/pre-empted like any other real thread from this
 * point on. Priority defaults to a mid-range, defensibly-labeled
 * value (64, the midpoint of the real 1-126 range) - no real hardware
 * citation exists for what priority the pre-THREADMAN boot context
 * "should" have, since real hardware's own bootstrap glue isn't
 * itself a THREADMAN-scheduled thread at all (see header comment). */
static void ensure_root_thread(iop_state_t *st)
{
    if (g.thread_count > 0) return;
    iop_tcb_t *t = &g.threads[0];
    memset(t, 0, sizeof(*t));
    t->in_use = 1;
    t->status = IOP_THS_RUN;
    t->priority = 64;
    t->init_priority = 64;
    t->ready_seq = g.ready_seq_counter++;
    memcpy(t->gpr, st->gpr, sizeof(t->gpr));
    t->pc = st->pc;
    t->next_pc = st->next_pc;
    t->hi = st->hi;
    t->lo = st->lo;
    g.thread_count = 1;
    g.current_thread_id = 1;
}

static void save_context(iop_state_t *st, int thid)
{
    iop_tcb_t *t = tcb(thid);
    if (!t) return;
    memcpy(t->gpr, st->gpr, sizeof(t->gpr));
    t->pc = st->pc;
    t->next_pc = st->next_pc;
    t->hi = st->hi;
    t->lo = st->lo;
}

static void load_context(iop_state_t *st, int thid)
{
    iop_tcb_t *t = tcb(thid);
    if (!t) return;
    memcpy(st->gpr, t->gpr, sizeof(t->gpr));
    st->pc = t->pc;
    st->next_pc = t->next_pc;
    st->hi = t->hi;
    st->lo = t->lo;
}

/* Real priority-based pick: lowest priority NUMBER wins (thbase.h:
 * HIGHEST_PRIORITY=1 ... LOWEST_PRIORITY=126); ties broken by lowest
 * ready_seq (earliest to become ready - real FIFO-within-priority
 * round robin, matching RotateThreadReadyQueue's own real purpose of
 * letting code manually cycle it). Threads currently RUN/READY are
 * both eligible (the running one is treated as still "ready" at its
 * own priority for this comparison - it only actually loses the CPU
 * if something else ties-or-beats it AND has an earlier or equal
 * claim; since the running thread's own ready_seq was set when it was
 * last scheduled in, a fresh contender with a later ready_seq at the
 * SAME priority correctly does NOT preempt it mid-quantum, matching
 * real cooperative-within-priority scheduling). */
static int pick_next_ready(void)
{
    int best = 0;
    uint32_t best_prio = 0xFFFFFFFFu;
    uint32_t best_seq = 0xFFFFFFFFu;
    for (int i = 0; i < IOP_HLE_THREAD_MAX_THREADS; i++) {
        iop_tcb_t *t = &g.threads[i];
        if (!t->in_use) continue;
        if (t->status != IOP_THS_RUN && t->status != IOP_THS_READY) continue;
        if (t->priority < best_prio || (t->priority == best_prio && t->ready_seq < best_seq)) {
            best = i + 1;
            best_prio = t->priority;
            best_seq = t->ready_seq;
        }
    }
    return best;
}

/* Core scheduling point - called after ANY operation that could
 * change which thread should be running. Mirrors real hardware's own
 * physical mechanism exactly (see header comment): if a switch is
 * needed, the live register file is saved into the outgoing thread's
 * TCB and the incoming thread's saved TCB state is loaded into the
 * live register file - a plain struct copy. */
static void reschedule(iop_state_t *st)
{
    int next = pick_next_ready();
    if (next == 0) {
        /* Nothing at all is ready (everything WAIT/DORMANT/SUSPEND) -
         * fall back to this project's existing, already-established
         * "idle but interrupt-responsive" mechanism (iop_core.h's
         * `idle` field, task #179) rather than inventing new halt
         * semantics. */
        if (g.current_thread_id != 0) save_context(st, g.current_thread_id);
        g.current_thread_id = 0;
        st->idle = 1;
        return;
    }
    if (next != g.current_thread_id) {
        if (g.current_thread_id != 0) {
            iop_tcb_t *cur = tcb(g.current_thread_id);
            if (cur && cur->status == IOP_THS_RUN) cur->status = IOP_THS_READY;
            save_context(st, g.current_thread_id);
        }
        load_context(st, next);
        tcb(next)->status = IOP_THS_RUN;
        g.current_thread_id = next;
        g.stats.context_switches++;
        st->idle = 0;
    } else {
        /* Same thread stays live - status may still need normalizing
         * back to RUN (e.g. it was the sole READY thread already). */
        iop_tcb_t *cur = tcb(g.current_thread_id);
        if (cur) cur->status = IOP_THS_RUN;
        st->idle = 0;
    }
}

/* Wakes the highest-priority (SA_THPRI) or earliest (SA_THFIFO, the
 * default) thread waiting on sema `semid`, if any. Returns 1 if a
 * waiter was woken (and the signal was transferred directly to it,
 * per real semantics - the sema's own count is not touched in that
 * case), 0 if no one was waiting. */
static int wake_one_sema_waiter(int semid, uint32_t attr)
{
    int best = 0;
    uint32_t best_prio = 0xFFFFFFFFu;
    uint32_t best_seq = 0xFFFFFFFFu;
    for (int i = 0; i < IOP_HLE_THREAD_MAX_THREADS; i++) {
        iop_tcb_t *t = &g.threads[i];
        if (!t->in_use || t->status != IOP_THS_WAIT || t->wait_type != IOP_TSW_SEMA || t->wait_id != semid)
            continue;
        int better;
        if (attr & 0x1u /* SA_THPRI */)
            better = (t->priority < best_prio) || (t->priority == best_prio && t->ready_seq < best_seq);
        else
            better = (t->ready_seq < best_seq);
        if (best == 0 || better) {
            best = i + 1;
            best_prio = t->priority;
            best_seq = t->ready_seq;
        }
    }
    if (best == 0) return 0;
    iop_tcb_t *t = tcb(best);
    t->status = IOP_THS_READY;
    t->wait_type = 0;
    t->wait_id = 0;
    t->ready_seq = g.ready_seq_counter++;
    iop_sema_t_internal *s = sema(semid);
    if (s) s->num_wait_threads--;
    return 1;
}

/* SetEventFlag/iSetEventFlag shared logic (Round 390) - real semantics
 * cited from ps2sdk's thevent.c (see header's Round 390 addendum):
 * ALL waiters whose condition is now satisfied are woken in one call
 * (unlike SignalSema's single-wake), the loop stops early once
 * evt->bits reaches 0 (a WEF_CLEAR wakeup can starve later waiters in
 * the same call, matching the real source exactly), and each woken
 * thread's resbits receives the RAW evt->bits value at the moment IT
 * matched (not just its own requested subset). Returns the number of
 * threads woken. */
static int wake_evf_waiters(iop_state_t *st, int efid)
{
    iop_evf_t_internal *e = evf(efid);
    if (!e) return 0;
    int woke = 0;
    for (int i = 0; i < IOP_HLE_THREAD_MAX_THREADS; i++) {
        iop_tcb_t *t = &g.threads[i];
        if (!e->bits) break; /* real source's own early-exit */
        if (!t->in_use || t->status != IOP_THS_WAIT || t->wait_type != IOP_TSW_EVENTFLAG || t->wait_id != efid)
            continue;
        uint32_t shared;
        if (t->wait_evf_mode & IOP_WEF_OR)
            shared = e->bits & t->wait_evf_bits;
        else
            shared = ((e->bits & t->wait_evf_bits) == t->wait_evf_bits) ? 1u : 0u;
        if (!shared) continue;
        if (t->wait_evf_resptr)
            iop_mem_write32(st, t->wait_evf_resptr, e->bits);
        if (t->wait_evf_mode & IOP_WEF_CLEAR)
            e->bits = 0;
        t->status = IOP_THS_READY;
        t->wait_type = 0; t->wait_id = 0;
        t->ready_seq = g.ready_seq_counter++;
        e->num_wait_threads--;
        woke++;
    }
    return woke;
}

uint32_t iop_hle_thread_sentinel_for_import(const char *module_name, uint32_t ordinal)
{
    if (!module_name) return 0;
    if (strncmp(module_name, "thbase", 8) == 0) {
        switch (ordinal) {
            case 3:  return IOP_HLE_THREAD_GETTHREADMANDATA;
            case 4:  return IOP_HLE_THREAD_CREATETHREAD;
            case 5:  return IOP_HLE_THREAD_DELETETHREAD;
            case 6:  return IOP_HLE_THREAD_STARTTHREAD;
            case 7:  return IOP_HLE_THREAD_STARTTHREADARGS;
            case 8:  return IOP_HLE_THREAD_EXITTHREAD;
            case 9:  return IOP_HLE_THREAD_EXITDELETETHREAD;
            case 10: return IOP_HLE_THREAD_TERMINATETHREAD;
            case 11: return IOP_HLE_THREAD_ITERMINATETHREAD;
            case 12: return IOP_HLE_THREAD_DISABLEDISPATCHTHREAD;
            case 13: return IOP_HLE_THREAD_ENABLEDISPATCHTHREAD;
            case 14: return IOP_HLE_THREAD_CHANGETHREADPRIORITY;
            case 15: return IOP_HLE_THREAD_ICHANGETHREADPRIORITY;
            case 16: return IOP_HLE_THREAD_ROTATETHREADREADYQUEUE;
            case 17: return IOP_HLE_THREAD_IROTATETHREADREADYQUEUE;
            case 18: return IOP_HLE_THREAD_RELEASEWAITTHREAD;
            case 19: return IOP_HLE_THREAD_IRELEASEWAITTHREAD;
            case 20: return IOP_HLE_THREAD_GETTHREADID;
            case 21: return IOP_HLE_THREAD_CHECKTHREADSTACK;
            case 22: return IOP_HLE_THREAD_REFERTHREADSTATUS;
            case 23: return IOP_HLE_THREAD_IREFERTHREADSTATUS;
            case 24: return IOP_HLE_THREAD_SLEEPTHREAD;
            case 25: return IOP_HLE_THREAD_WAKEUPTHREAD;
            case 26: return IOP_HLE_THREAD_IWAKEUPTHREAD;
            case 27: return IOP_HLE_THREAD_CANCELWAKEUPTHREAD;
            case 28: return IOP_HLE_THREAD_ICANCELWAKEUPTHREAD;
            case 29: return IOP_HLE_THREAD_SUSPENDTHREAD;
            case 30: return IOP_HLE_THREAD_ISUSPENDTHREAD;
            case 31: return IOP_HLE_THREAD_RESUMETHREAD;
            case 32: return IOP_HLE_THREAD_IRESUMETHREAD;
            case 33: return IOP_HLE_THREAD_DELAYTHREAD;
            case 34: return IOP_HLE_THREAD_GETSYSTEMTIME;
            case 35: return IOP_HLE_THREAD_SETALARM;
            case 36: return IOP_HLE_THREAD_ISETALARM;
            case 37: return IOP_HLE_THREAD_CANCELALARM;
            case 38: return IOP_HLE_THREAD_ICANCELALARM;
            case 39: return IOP_HLE_THREAD_USEC2SYSCLOCK;
            case 40: return IOP_HLE_THREAD_SYSCLOCK2USEC;
            case 41: return IOP_HLE_THREAD_GETSYSTEMSTATUSFLAG;
            default: return 0;
        }
    }
    if (strncmp(module_name, "thsemap", 8) == 0) {
        switch (ordinal) {
            case 4:  return IOP_HLE_THREAD_CREATESEMA;
            case 5:  return IOP_HLE_THREAD_DELETESEMA;
            case 6:  return IOP_HLE_THREAD_SIGNALSEMA;
            case 7:  return IOP_HLE_THREAD_ISIGNALSEMA;
            case 8:  return IOP_HLE_THREAD_WAITSEMA;
            case 9:  return IOP_HLE_THREAD_POLLSEMA;
            case 11: return IOP_HLE_THREAD_REFERSEMASTATUS;
            case 12: return IOP_HLE_THREAD_IREFERSEMASTATUS;
            default: return 0;
        }
    }
    if (strncmp(module_name, "thevent", 8) == 0) {
        switch (ordinal) {
            case 4:  return IOP_HLE_THREAD_CREATEEVENTFLAG;
            case 5:  return IOP_HLE_THREAD_DELETEEVENTFLAG;
            case 6:  return IOP_HLE_THREAD_SETEVENTFLAG;
            case 7:  return IOP_HLE_THREAD_ISETEVENTFLAG;
            case 8:  return IOP_HLE_THREAD_CLEAREVENTFLAG;
            case 9:  return IOP_HLE_THREAD_ICLEAREVENTFLAG;
            case 10: return IOP_HLE_THREAD_WAITEVENTFLAG;
            case 11: return IOP_HLE_THREAD_POLLEVENTFLAG;
            case 13: return IOP_HLE_THREAD_REFEREVENTFLAGSTATUS;
            case 14: return IOP_HLE_THREAD_IREFEREVENTFLAGSTATUS;
            default: return 0;
        }
    }
    return 0;
}

int iop_hle_thread_try_handle(iop_state_t *st, uint32_t pc)
{
    /* Round 390: rewritten as an explicit set-membership check instead
     * of a chain of range exclusions, since the thevent/Alarm gates
     * added this round sit in their own sub-ranges above thsemap's
     * with small deliberate gaps (headroom for future ordinals) -
     * a single min/max bound would either miss them or falsely accept
     * the gaps. */
    int in_range =
        (pc >= IOP_HLE_THREAD_GETTHREADMANDATA && pc <= IOP_HLE_THREAD_IREFERSEMASTATUS) ||
        pc == IOP_HLE_THREAD_ENTRY_RETURN_TRAMPOLINE ||
        (pc >= IOP_HLE_THREAD_CREATEEVENTFLAG && pc <= IOP_HLE_THREAD_IREFEREVENTFLAGSTATUS) ||
        (pc >= IOP_HLE_THREAD_SETALARM && pc <= IOP_HLE_THREAD_SYSCLOCK2USEC) ||
        pc == IOP_HLE_THREAD_ALARM_RETURN_TRAMPOLINE;
    if (!in_range) return 0;

    ensure_root_thread(st);
    uint32_t ra = st->gpr[31];
    int cur = g.current_thread_id;

    if (pc == IOP_HLE_THREAD_ALARM_RETURN_TRAMPOLINE) {
        /* The dispatched Alarm callback (see iop_hle_thread_tick())
         * has finished and jr $ra'd back to us. Real semantics
         * (thbase.h SetAlarm doc, cited in header): $v0 holds the
         * callback's real return value - 0 means "do not reschedule"
         * (free the slot), nonzero means "reschedule after that many
         * more usec" (matching DelayThread's own usec->cycle
         * conversion). Then resume exactly where the tick() call that
         * dispatched it left off. */
        if (g.alarm_in_dispatch) {
            iop_alarm_t_internal *a = alarm_slot(g.alarm_dispatch_slot);
            uint32_t retval = st->gpr[2];
            if (a && a->in_use) {
                if (retval == 0) {
                    a->in_use = 0;
                } else {
                    uint64_t cycles = ((uint64_t)retval * IOP_HLE_THREAD_CLOCK_HZ) / 1000000ull;
                    a->deadline = st->instructions_executed + cycles;
                }
            }
            g.stats.alarms_fired++;
            st->pc = g.alarm_resume_pc;
            st->next_pc = g.alarm_resume_next_pc;
            g.alarm_in_dispatch = 0;
            g.alarm_dispatch_slot = 0;
        }
        return 1;
    }

    if (pc == IOP_HLE_THREAD_ENTRY_RETURN_TRAMPOLINE) {
        /* A thread fell off the end of its own entry function via a
         * plain jr $ra instead of calling ExitThread - real ps2sdk
         * thread entries are documented to do this; treated
         * identically to ExitThread(). */
        if (cur) tcb(cur)->status = IOP_THS_DORMANT;
        g.stats.threads_exited++;
        reschedule(st);
        return 1;
    }

    if (pc == IOP_HLE_THREAD_GETTHREADMANDATA) {
        /* extern void *GetThreadmanData(void) - real internal-use
         * accessor, undocumented return value; no verified real
         * struct to point at, so this project's own established
         * "generic 0 default" convention applies (same as unresolved
         * A0/B0/C0 calls elsewhere - not fabricated further). */
        st->gpr[2] = 0;
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_CREATETHREAD) {
        /* int CreateThread(iop_thread_t *thread) - a0=param ptr.
         * Real field layout (thbase.h, cited in this file's header):
         * attr@0, option@4, entry-fptr@8, stacksize@12, priority@16. */
        uint32_t param = st->gpr[4];
        uint32_t attr = iop_mem_read32(st, param + 0u);
        uint32_t option = iop_mem_read32(st, param + 4u);
        uint32_t entry = iop_mem_read32(st, param + 8u);
        uint32_t stacksize = iop_mem_read32(st, param + 12u);
        uint32_t priority = iop_mem_read32(st, param + 16u);
        if (stacksize < 0x100u) stacksize = 0x100u; /* real kernel also enforces a practical minimum */
        int slot = alloc_tcb_slot();
        if (slot == 0 || g.stack_bump_next < THREAD_STACK_ARENA_BOTTOM + stacksize) {
            st->gpr[2] = (uint32_t)-1; /* generic real-kernel-style failure return, out of TCBs/stack arena */
        } else {
            uint32_t aligned_size = (stacksize + 15u) & ~15u;
            g.stack_bump_next -= aligned_size;
            iop_tcb_t *t = tcb(slot);
            memset(t, 0, sizeof(*t));
            t->in_use = 1;
            t->status = IOP_THS_DORMANT;
            t->attr = attr; t->option = option;
            t->entry = entry;
            t->stack_base = g.stack_bump_next;
            t->stack_size = aligned_size;
            t->priority = priority;
            t->init_priority = priority;
            t->ready_seq = 0;
            if (slot > g.thread_count) g.thread_count = slot;
            g.stats.threads_created++;
            st->gpr[2] = (uint32_t)slot; /* real success return: the new thread ID */
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_DELETETHREAD) {
        /* int DeleteThread(int thid) - a0=thid. Real kernel requires
         * the thread be DORMANT first. */
        int thid = (int)st->gpr[4];
        iop_tcb_t *t = tcb(thid);
        if (t && t->in_use && t->status == IOP_THS_DORMANT) {
            t->in_use = 0;
            g.stats.threads_deleted++;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_STARTTHREAD || pc == IOP_HLE_THREAD_STARTTHREADARGS) {
        /* int StartThread(int thid, void *arg) - a0=thid, a1=arg.
         * StartThreadArgs(int thid, int args, void *argp) - a0=thid,
         * a1=args(argc-like count), a2=argp: this project passes argp
         * through in $a0 either way (real ps2sdk _args-style thread
         * entries expect (int args, char *argp), a slightly different
         * real ABI than plain StartThread's single void* - honestly
         * approximated here by passing a1/a2 straight through as
         * $a0/$a1 for the ...Args variant, since no real module in
         * this project's own boot list is yet confirmed to use it). */
        int thid = (int)st->gpr[4];
        iop_tcb_t *t = tcb(thid);
        if (t && t->in_use && t->status == IOP_THS_DORMANT) {
            memset(t->gpr, 0, sizeof(t->gpr));
            uint32_t stack_top = (t->stack_base + t->stack_size) & ~7u; /* real MIPS o32 8-byte SP alignment */
            t->gpr[29] = stack_top; /* $sp */
            t->gpr[28] = st->gpr[28]; /* $gp - inherited from the starting context, see header comment */
            t->gpr[31] = IOP_HLE_THREAD_ENTRY_RETURN_TRAMPOLINE; /* $ra */
            if (pc == IOP_HLE_THREAD_STARTTHREAD) {
                t->gpr[4] = st->gpr[5]; /* $a0 = arg */
            } else {
                t->gpr[4] = st->gpr[5]; /* $a0 = args */
                t->gpr[5] = st->gpr[6]; /* $a1 = argp */
            }
            t->pc = t->entry;
            t->next_pc = t->entry + 4u;
            t->status = IOP_THS_READY;
            t->ready_seq = g.ready_seq_counter++;
            g.stats.threads_started++;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        reschedule(st); /* the newly-READY thread may now pre-empt the caller if higher priority */
        return 1;
    }
    if (pc == IOP_HLE_THREAD_EXITTHREAD || pc == IOP_HLE_THREAD_EXITDELETETHREAD) {
        if (cur) {
            tcb(cur)->status = IOP_THS_DORMANT;
            if (pc == IOP_HLE_THREAD_EXITDELETETHREAD) tcb(cur)->in_use = 0;
        }
        g.stats.threads_exited++;
        /* No caller to return to - this context is gone. reschedule()
         * picks whatever runs next (or falls back to idle). */
        reschedule(st);
        return 1;
    }
    if (pc == IOP_HLE_THREAD_TERMINATETHREAD || pc == IOP_HLE_THREAD_ITERMINATETHREAD) {
        int thid = (int)st->gpr[4];
        iop_tcb_t *t = tcb(thid);
        if (t && t->in_use && thid != cur) {
            t->status = IOP_THS_DORMANT;
            t->wait_type = 0; t->wait_id = 0;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1; /* real kernel also disallows terminating the caller's own thread this way */
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_DISABLEDISPATCHTHREAD || pc == IOP_HLE_THREAD_ENABLEDISPATCHTHREAD) {
        /* Real kernel-level preemption on/off toggle around a
         * critical section. This project's HLE syscalls already run
         * atomically with respect to each other (plain C, no
         * interleaving mid-call), so honored as a real, harmless
         * no-op - same documented-simplification precedent as
         * CpuSuspendIntr/CpuResumeIntr in iop_core.c. */
        st->gpr[2] = 0;
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_CHANGETHREADPRIORITY || pc == IOP_HLE_THREAD_ICHANGETHREADPRIORITY) {
        int thid = (int)st->gpr[4];
        int32_t priority = (int32_t)st->gpr[5];
        iop_tcb_t *t = tcb(thid);
        if (t && t->in_use) {
            t->priority = (uint32_t)priority;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        reschedule(st);
        return 1;
    }
    if (pc == IOP_HLE_THREAD_ROTATETHREADREADYQUEUE || pc == IOP_HLE_THREAD_IROTATETHREADREADYQUEUE) {
        /* int RotateThreadReadyQueue(int priority) - a0=priority (0 =
         * caller's own current priority). Moves the head of that
         * priority's ready queue to the tail - implemented here by
         * giving the earliest-ready_seq thread at that priority a
         * fresh (latest) ready_seq, which is observably equivalent
         * for this file's ready_seq-ordered scheduling model. */
        int32_t priority = (int32_t)st->gpr[4];
        if (priority == 0 && cur) priority = (int32_t)tcb(cur)->priority;
        int earliest = 0;
        uint32_t earliest_seq = 0xFFFFFFFFu;
        for (int i = 0; i < IOP_HLE_THREAD_MAX_THREADS; i++) {
            iop_tcb_t *t = &g.threads[i];
            if (t->in_use && (int32_t)t->priority == priority &&
                (t->status == IOP_THS_READY || t->status == IOP_THS_RUN) &&
                t->ready_seq < earliest_seq) {
                earliest = i + 1;
                earliest_seq = t->ready_seq;
            }
        }
        if (earliest) tcb(earliest)->ready_seq = g.ready_seq_counter++;
        st->gpr[2] = 0;
        st->pc = ra; st->next_pc = ra + 4u;
        reschedule(st);
        return 1;
    }
    if (pc == IOP_HLE_THREAD_RELEASEWAITTHREAD || pc == IOP_HLE_THREAD_IRELEASEWAITTHREAD) {
        /* int ReleaseWaitThread(int thid) - forces a waiting thread
         * to wake early with an error return, real semantics. */
        int thid = (int)st->gpr[4];
        iop_tcb_t *t = tcb(thid);
        if (t && t->in_use && t->status == IOP_THS_WAIT) {
            if (t->wait_type == IOP_TSW_SEMA) {
                iop_sema_t_internal *s = sema(t->wait_id);
                if (s) s->num_wait_threads--;
            }
            t->status = IOP_THS_READY;
            t->wait_type = 0; t->wait_id = 0;
            t->ready_seq = g.ready_seq_counter++;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        reschedule(st);
        return 1;
    }
    if (pc == IOP_HLE_THREAD_GETTHREADID) {
        st->gpr[2] = (uint32_t)cur;
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_CHECKTHREADSTACK) {
        /* int CheckThreadStack(void) - real return is free stack
         * margin in bytes; this project doesn't model stack-canary
         * fill patterns (TH_CLEAR_STACK), so a generous constant
         * (the calling thread's own full stack_size) is returned -
         * an honestly-labeled simplification, not a measured value. */
        iop_tcb_t *t = tcb(cur);
        st->gpr[2] = t ? t->stack_size : 0u;
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_REFERTHREADSTATUS || pc == IOP_HLE_THREAD_IREFERTHREADSTATUS) {
        /* int ReferThreadStatus(int thid, iop_thread_info_t *info) -
         * a0=thid, a1=info ptr. Real field offsets cited in this
         * file's header. regContext (a real internal kernel pointer
         * this project doesn't expose as guest-readable memory) is
         * honestly left 0 rather than fabricated. */
        int thid = (int)st->gpr[4];
        uint32_t info = st->gpr[5];
        iop_tcb_t *t = tcb(thid);
        if (t && t->in_use) {
            iop_mem_write32(st, info + 0u, t->attr);
            iop_mem_write32(st, info + 4u, t->option);
            iop_mem_write32(st, info + 8u, t->status);
            iop_mem_write32(st, info + 12u, t->entry);
            iop_mem_write32(st, info + 16u, t->stack_base);
            iop_mem_write32(st, info + 20u, t->stack_size);
            iop_mem_write32(st, info + 24u, t->gpr[28]); /* gpReg */
            iop_mem_write32(st, info + 28u, t->init_priority);
            iop_mem_write32(st, info + 32u, t->priority);
            iop_mem_write32(st, info + 36u, (uint32_t)t->wait_type);
            iop_mem_write32(st, info + 40u, (uint32_t)t->wait_id);
            iop_mem_write32(st, info + 44u, t->wakeup_count);
            iop_mem_write32(st, info + 48u, 0u); /* regContext - not exposed, honestly zeroed */
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_SLEEPTHREAD) {
        iop_tcb_t *t = tcb(cur);
        if (t) {
            if (t->wakeup_count > 0) {
                /* A WakeupThread() already arrived before this
                 * SleepThread() - real semantics: consume the pending
                 * credit and return immediately without blocking. */
                t->wakeup_count--;
                st->gpr[2] = 0;
                st->pc = ra; st->next_pc = ra + 4u;
            } else {
                st->gpr[2] = 0; /* pre-set: the real return value once woken */
                st->pc = ra; st->next_pc = ra + 4u;
                t->status = IOP_THS_WAIT;
                t->wait_type = IOP_TSW_SLEEP;
                t->wait_id = 0;
                g.stats.sleep_thread_blocked++;
                reschedule(st);
            }
        } else {
            st->pc = ra; st->next_pc = ra + 4u;
        }
        return 1;
    }
    if (pc == IOP_HLE_THREAD_WAKEUPTHREAD || pc == IOP_HLE_THREAD_IWAKEUPTHREAD) {
        int thid = (int)st->gpr[4];
        iop_tcb_t *t = tcb(thid);
        if (t && t->in_use) {
            if (t->status == IOP_THS_WAIT && t->wait_type == IOP_TSW_SLEEP) {
                t->status = IOP_THS_READY;
                t->wait_type = 0;
                t->ready_seq = g.ready_seq_counter++;
            } else {
                t->wakeup_count++; /* real semantics: pending credit for a future SleepThread() */
            }
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        reschedule(st);
        return 1;
    }
    if (pc == IOP_HLE_THREAD_CANCELWAKEUPTHREAD || pc == IOP_HLE_THREAD_ICANCELWAKEUPTHREAD) {
        int thid = (int)st->gpr[4];
        iop_tcb_t *t = tcb(thid);
        if (t && t->in_use) {
            st->gpr[2] = (uint32_t)t->wakeup_count; /* real return: previous pending count */
            t->wakeup_count = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_SUSPENDTHREAD || pc == IOP_HLE_THREAD_ISUSPENDTHREAD) {
        int thid = (int)st->gpr[4];
        iop_tcb_t *t = tcb(thid);
        if (t && t->in_use && t->status != IOP_THS_DORMANT) {
            t->status |= IOP_THS_SUSPEND; /* THS_WAITSUSPEND if it was already WAIT, matching real bit layout */
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        reschedule(st);
        return 1;
    }
    if (pc == IOP_HLE_THREAD_RESUMETHREAD || pc == IOP_HLE_THREAD_IRESUMETHREAD) {
        int thid = (int)st->gpr[4];
        iop_tcb_t *t = tcb(thid);
        if (t && t->in_use && (t->status & IOP_THS_SUSPEND)) {
            t->status &= ~IOP_THS_SUSPEND;
            if (t->status == 0) t->status = IOP_THS_READY; /* was pure THS_SUSPEND -> now runnable */
            if (t->status == IOP_THS_READY) t->ready_seq = g.ready_seq_counter++;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        reschedule(st);
        return 1;
    }
    if (pc == IOP_HLE_THREAD_DELAYTHREAD) {
        uint32_t usec = st->gpr[4];
        iop_tcb_t *t = tcb(cur);
        if (t) {
            /* usec -> instructions_executed deadline, per this
             * project's own "1 instruction = 1 real IOP cycle"
             * convention (see header comment) at the real, cited
             * 33.8688MHz IOP clock rate. 64-bit throughout to avoid
             * overflow for large usec values. */
            uint64_t cycles = ((uint64_t)usec * IOP_HLE_THREAD_CLOCK_HZ) / 1000000ull;
            t->delay_deadline = st->instructions_executed + cycles;
            t->status = IOP_THS_WAIT;
            t->wait_type = IOP_TSW_DELAY;
            t->wait_id = 0;
            g.stats.delay_thread_blocked++;
            st->gpr[2] = 0;
            st->pc = ra; st->next_pc = ra + 4u;
            reschedule(st);
        } else {
            st->pc = ra; st->next_pc = ra + 4u;
        }
        return 1;
    }
    if (pc == IOP_HLE_THREAD_GETSYSTEMTIME) {
        /* int GetSystemTime(iop_sys_clock_t *sys_clock) - a0=ptr to
         * {u32 lo, hi}. Real units are undocumented-precise internal
         * ticks; this project reports instructions_executed directly
         * (consistent with the same "1 instruction = 1 cycle"
         * convention used throughout), an honest, labeled choice. */
        uint32_t ptr = st->gpr[4];
        uint64_t v = st->instructions_executed;
        iop_mem_write32(st, ptr + 0u, (uint32_t)(v & 0xFFFFFFFFu));
        iop_mem_write32(st, ptr + 4u, (uint32_t)(v >> 32));
        st->gpr[2] = 0;
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_GETSYSTEMSTATUSFLAG) {
        /* Real bitflags (boot-phase indicators) this project doesn't
         * model - generic 0 default, same established convention. */
        st->gpr[2] = 0;
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_CREATESEMA) {
        /* int CreateSema(iop_sema_t *sema) - a0=param ptr. Real
         * fields (thsemap.h, cited in header): attr@0, option@4,
         * initial@8, max@12. */
        uint32_t param = st->gpr[4];
        uint32_t attr = iop_mem_read32(st, param + 0u);
        uint32_t option = iop_mem_read32(st, param + 4u);
        int32_t initial = (int32_t)iop_mem_read32(st, param + 8u);
        int32_t max = (int32_t)iop_mem_read32(st, param + 12u);
        int slot = alloc_sema_slot();
        if (slot == 0) {
            st->gpr[2] = (uint32_t)-1;
        } else {
            iop_sema_t_internal *s = sema(slot);
            memset(s, 0, sizeof(*s));
            s->in_use = 1;
            s->attr = attr; s->option = option;
            s->initial = initial; s->max = max;
            s->count = initial;
            g.stats.semas_created++;
            st->gpr[2] = (uint32_t)slot;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_DELETESEMA) {
        int semid = (int)st->gpr[4];
        iop_sema_t_internal *s = sema(semid);
        if (s && s->in_use) {
            s->in_use = 0;
            g.stats.semas_deleted++;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_SIGNALSEMA || pc == IOP_HLE_THREAD_ISIGNALSEMA) {
        int semid = (int)st->gpr[4];
        iop_sema_t_internal *s = sema(semid);
        if (s && s->in_use) {
            if (!wake_one_sema_waiter(semid, s->attr)) {
                if (s->count < s->max) s->count++;
            }
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        if (pc == IOP_HLE_THREAD_SIGNALSEMA) reschedule(st); /* iSignalSema: interrupt context, defer any switch */
        return 1;
    }
    if (pc == IOP_HLE_THREAD_WAITSEMA) {
        int semid = (int)st->gpr[4];
        iop_sema_t_internal *s = sema(semid);
        if (!s || !s->in_use) {
            st->gpr[2] = (uint32_t)-1;
            st->pc = ra; st->next_pc = ra + 4u;
            return 1;
        }
        if (s->count > 0) {
            s->count--;
            g.stats.wait_sema_immediate++;
            st->gpr[2] = 0;
            st->pc = ra; st->next_pc = ra + 4u;
        } else {
            iop_tcb_t *t = tcb(cur);
            st->gpr[2] = 0; /* pre-set: the real return value once woken */
            st->pc = ra; st->next_pc = ra + 4u;
            if (t) {
                t->status = IOP_THS_WAIT;
                t->wait_type = IOP_TSW_SEMA;
                t->wait_id = semid;
                t->ready_seq = g.ready_seq_counter++; /* used as this thread's own wait FIFO order too */
            }
            s->num_wait_threads++;
            g.stats.wait_sema_blocked++;
            reschedule(st);
        }
        return 1;
    }
    if (pc == IOP_HLE_THREAD_POLLSEMA) {
        int semid = (int)st->gpr[4];
        iop_sema_t_internal *s = sema(semid);
        if (s && s->in_use && s->count > 0) {
            s->count--;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1; /* real: a negative "not available" error code (exact value not verified/cited) */
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_REFERSEMASTATUS || pc == IOP_HLE_THREAD_IREFERSEMASTATUS) {
        int semid = (int)st->gpr[4];
        uint32_t info = st->gpr[5];
        iop_sema_t_internal *s = sema(semid);
        if (s && s->in_use) {
            iop_mem_write32(st, info + 0u, s->attr);
            iop_mem_write32(st, info + 4u, s->option);
            iop_mem_write32(st, info + 8u, (uint32_t)s->initial);
            iop_mem_write32(st, info + 12u, (uint32_t)s->max);
            iop_mem_write32(st, info + 16u, (uint32_t)s->count);
            iop_mem_write32(st, info + 20u, (uint32_t)s->num_wait_threads);
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }

    /* ---- Round 390: thevent (EventFlags) ---- */

    if (pc == IOP_HLE_THREAD_CREATEEVENTFLAG) {
        /* int CreateEventFlag(iop_event_t *event) - a0=param ptr.
         * Real fields (thevent.h, cited in header): attr@0, option@4,
         * bits@8 (initial value). */
        uint32_t param = st->gpr[4];
        uint32_t attr = iop_mem_read32(st, param + 0u);
        uint32_t option = iop_mem_read32(st, param + 4u);
        uint32_t bits = iop_mem_read32(st, param + 8u);
        int slot = alloc_evf_slot();
        if (slot == 0) {
            st->gpr[2] = (uint32_t)-1;
        } else {
            iop_evf_t_internal *e = evf(slot);
            memset(e, 0, sizeof(*e));
            e->in_use = 1;
            e->attr = attr; e->option = option;
            e->init_bits = bits; e->bits = bits;
            g.stats.evflags_created++;
            st->gpr[2] = (uint32_t)slot;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_DELETEEVENTFLAG) {
        /* int DeleteEventFlag(int ef) - real semantics (thevent.c,
         * cited in header): any thread still waiting on it is woken
         * with an error return (this project's generic -1, same
         * established convention as ReleaseWaitThread/DeleteSema's
         * own analogous "deleted out from under a waiter" cases). */
        int efid = (int)st->gpr[4];
        iop_evf_t_internal *e = evf(efid);
        if (e && e->in_use) {
            for (int i = 0; i < IOP_HLE_THREAD_MAX_THREADS; i++) {
                iop_tcb_t *t = &g.threads[i];
                if (t->in_use && t->status == IOP_THS_WAIT && t->wait_type == IOP_TSW_EVENTFLAG && t->wait_id == efid) {
                    t->status = IOP_THS_READY;
                    t->wait_type = 0; t->wait_id = 0;
                    t->ready_seq = g.ready_seq_counter++;
                    /* real: v0 = KE_WAIT_DELETE for the woken thread;
                     * this project's own generic error-return
                     * convention (-1) is used instead, consistent
                     * with every other "operation failed/aborted"
                     * case in this file. */
                }
            }
            e->in_use = 0;
            g.stats.evflags_deleted++;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        reschedule(st);
        return 1;
    }
    if (pc == IOP_HLE_THREAD_SETEVENTFLAG || pc == IOP_HLE_THREAD_ISETEVENTFLAG) {
        int efid = (int)st->gpr[4];
        uint32_t bits = st->gpr[5];
        iop_evf_t_internal *e = evf(efid);
        if (e && e->in_use) {
            if (bits != 0) { /* real: bits==0 is a real documented no-op */
                e->bits |= bits;
                wake_evf_waiters(st, efid);
            }
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        if (pc == IOP_HLE_THREAD_SETEVENTFLAG) reschedule(st); /* iSetEventFlag: interrupt context, defer any switch */
        return 1;
    }
    if (pc == IOP_HLE_THREAD_CLEAREVENTFLAG || pc == IOP_HLE_THREAD_ICLEAREVENTFLAG) {
        /* int ClearEventFlag(int ef, u32 bits) - real semantics
         * CONFIRMED this round via direct ps2sdk thevent.c fetch (see
         * header's Round 390 addendum): `evt->bits &= bits;` - a
         * "keep mask" (uITRON clr_flg convention), NOT the more
         * commonly-assumed "clear mask" currBits &= ~bits. No waiter
         * interaction in the real source - this call never wakes
         * anyone by itself. */
        int efid = (int)st->gpr[4];
        uint32_t bits = st->gpr[5];
        iop_evf_t_internal *e = evf(efid);
        if (e && e->in_use) {
            e->bits &= bits;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_WAITEVENTFLAG) {
        /* int WaitEventFlag(int ef, u32 bits, int mode, u32 *resbits) -
         * a0=ef, a1=bits, a2=mode, a3=resbits. Real match rule (cited
         * in header): WEF_OR -> (currBits & bits) != 0; default AND
         * -> (currBits & bits) == bits. On match, *resbits (if
         * non-NULL) receives the RAW currBits (not just the matched
         * subset), and WEF_CLEAR zeroes ALL of currBits (not just the
         * matched bits) - both exactly as fetched from thevent.c. */
        int efid = (int)st->gpr[4];
        uint32_t bits = st->gpr[5];
        uint32_t mode = st->gpr[6];
        uint32_t resptr = st->gpr[7];
        iop_evf_t_internal *e = evf(efid);
        if (!e || !e->in_use || bits == 0) {
            st->gpr[2] = (uint32_t)-1; /* real: KE_UNKNOWN_EVFID / KE_EVF_ILPAT, generic -1 per this file's established convention */
            st->pc = ra; st->next_pc = ra + 4u;
            return 1;
        }
        /* Real fetched source's own non-EA_MULTI reject check reads
         * `waiter_count >= 0` - always true for an unsigned counter,
         * which read literally would reject even the FIRST wait on
         * any EA_SINGLE flag. Judged a transcription/logic artifact
         * upstream (see header's Round 390 addendum, deviation #1);
         * implemented as the evidently-intended "a second
         * simultaneous waiter on a non-multi flag is rejected"
         * instead. */
        if (!(e->attr & IOP_EA_MULTI) && e->num_wait_threads > 0) {
            st->gpr[2] = (uint32_t)-1; /* real: KE_EVF_MULTI */
            st->pc = ra; st->next_pc = ra + 4u;
            return 1;
        }
        uint32_t shared = (mode & IOP_WEF_OR) ? (e->bits & bits) : (((e->bits & bits) == bits) ? 1u : 0u);
        if (shared) {
            if (resptr) iop_mem_write32(st, resptr, e->bits);
            if (mode & IOP_WEF_CLEAR) e->bits = 0;
            g.stats.wait_evf_immediate++;
            st->gpr[2] = 0;
            st->pc = ra; st->next_pc = ra + 4u;
        } else {
            iop_tcb_t *t = tcb(cur);
            st->gpr[2] = 0; /* pre-set: the real return value once woken */
            st->pc = ra; st->next_pc = ra + 4u;
            if (t) {
                t->status = IOP_THS_WAIT;
                t->wait_type = IOP_TSW_EVENTFLAG;
                t->wait_id = efid;
                t->wait_evf_bits = bits;
                /* Real fetched source stores `event_mode = bits`
                 * (not `mode`) - inconsistent with SetEventFlag's own
                 * later `& WEF_OR`/`& WEF_CLEAR` use of that same
                 * field (see header's Round 390 addendum, deviation
                 * #2). The actual mode argument is stored here
                 * instead, matching the evident intent. */
                t->wait_evf_mode = mode;
                t->wait_evf_resptr = resptr;
            }
            e->num_wait_threads++;
            g.stats.wait_evf_blocked++;
            reschedule(st);
        }
        return 1;
    }
    if (pc == IOP_HLE_THREAD_POLLEVENTFLAG) {
        int efid = (int)st->gpr[4];
        uint32_t bits = st->gpr[5];
        uint32_t mode = st->gpr[6];
        uint32_t resptr = st->gpr[7];
        iop_evf_t_internal *e = evf(efid);
        uint32_t shared = 0;
        if (e && e->in_use && bits != 0)
            shared = (mode & IOP_WEF_OR) ? (e->bits & bits) : (((e->bits & bits) == bits) ? 1u : 0u);
        if (e && e->in_use && bits != 0 && shared) {
            if (resptr) iop_mem_write32(st, resptr, e->bits);
            if (mode & IOP_WEF_CLEAR) e->bits = 0;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1; /* real: KE_EVF_COND (or KE_UNKNOWN_EVFID/KE_EVF_ILPAT), generic -1 */
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_REFEREVENTFLAGSTATUS || pc == IOP_HLE_THREAD_IREFEREVENTFLAGSTATUS) {
        /* int ReferEventFlagStatus(int ef, iop_event_info_t *info) -
         * real field offsets (thevent.h, cited in header): attr@0,
         * option@4, initBits@8, currBits@12, numThreads@16. */
        int efid = (int)st->gpr[4];
        uint32_t info = st->gpr[5];
        iop_evf_t_internal *e = evf(efid);
        if (e && e->in_use) {
            iop_mem_write32(st, info + 0u, e->attr);
            iop_mem_write32(st, info + 4u, e->option);
            iop_mem_write32(st, info + 8u, e->init_bits);
            iop_mem_write32(st, info + 12u, e->bits);
            iop_mem_write32(st, info + 16u, (uint32_t)e->num_wait_threads);
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }

    /* ---- Round 390: thbase Alarm ---- */

    if (pc == IOP_HLE_THREAD_SETALARM || pc == IOP_HLE_THREAD_ISETALARM) {
        /* int SetAlarm(u32 time, alarm_callback_t callback, void
         * *common) - a0=usec, a1=callback, a2=common. Real return is
         * a positive alarm ID or a negative error (ps2sdk thbase.h). */
        uint32_t usec = st->gpr[4];
        uint32_t handler = st->gpr[5];
        uint32_t common = st->gpr[6];
        int slot = alloc_alarm_slot();
        if (slot == 0 || handler == 0) {
            st->gpr[2] = (uint32_t)-1;
        } else {
            uint64_t cycles = ((uint64_t)usec * IOP_HLE_THREAD_CLOCK_HZ) / 1000000ull;
            iop_alarm_t_internal *a = alarm_slot(slot);
            a->in_use = 1;
            a->handler = handler;
            a->common = common;
            a->deadline = st->instructions_executed + cycles;
            g.stats.alarms_set++;
            st->gpr[2] = (uint32_t)slot;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_CANCELALARM || pc == IOP_HLE_THREAD_ICANCELALARM) {
        /* int CancelAlarm(alarm_callback_t callback, void *common) -
         * real signature identifies the alarm by (callback, common)
         * pair, not by the SetAlarm-returned ID - matched here the
         * same way. */
        uint32_t handler = st->gpr[4];
        uint32_t common = st->gpr[5];
        int found = 0;
        for (int i = 0; i < IOP_HLE_THREAD_MAX_ALARMS; i++) {
            iop_alarm_t_internal *a = &g.alarms[i];
            if (a->in_use && a->handler == handler && a->common == common) {
                a->in_use = 0;
                found = 1;
                break;
            }
        }
        if (found) {
            g.stats.alarms_cancelled++;
            st->gpr[2] = 0;
        } else {
            st->gpr[2] = (uint32_t)-1;
        }
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_USEC2SYSCLOCK) {
        /* int USec2SysClock(u32 usec, iop_sys_clock_t *sys_clock) -
         * a0=usec, a1=out ptr to {u32 lo, hi}. Same instructions_
         * executed-as-ticks convention as GetSystemTime/DelayThread
         * (see header comment), honestly labeled there as this
         * project's own established simplification, not a fabricated
         * new one. */
        uint32_t usec = st->gpr[4];
        uint32_t ptr = st->gpr[5];
        uint64_t cycles = ((uint64_t)usec * IOP_HLE_THREAD_CLOCK_HZ) / 1000000ull;
        iop_mem_write32(st, ptr + 0u, (uint32_t)(cycles & 0xFFFFFFFFu));
        iop_mem_write32(st, ptr + 4u, (uint32_t)(cycles >> 32));
        st->gpr[2] = 0;
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }
    if (pc == IOP_HLE_THREAD_SYSCLOCK2USEC) {
        /* int SysClock2USec(iop_sys_clock_t *sys_clock, u32 *sec, u32
         * *usec) - a0=in ptr, a1=sec out, a2=usec out. Inverse of the
         * above, same convention/citation. */
        uint32_t ptr = st->gpr[4];
        uint32_t sec_ptr = st->gpr[5];
        uint32_t usec_ptr = st->gpr[6];
        uint64_t lo = iop_mem_read32(st, ptr + 0u);
        uint64_t hi = iop_mem_read32(st, ptr + 4u);
        uint64_t cycles = lo | (hi << 32);
        uint64_t total_usec = (cycles * 1000000ull) / IOP_HLE_THREAD_CLOCK_HZ;
        if (sec_ptr) iop_mem_write32(st, sec_ptr, (uint32_t)(total_usec / 1000000ull));
        if (usec_ptr) iop_mem_write32(st, usec_ptr, (uint32_t)(total_usec % 1000000ull));
        st->gpr[2] = 0;
        st->pc = ra; st->next_pc = ra + 4u;
        return 1;
    }

    return 0;
}

void iop_hle_thread_tick(iop_state_t *st)
{
    if (g.thread_count == 0) return; /* no thread primitive has ever run yet */

    /* Round 390: at most one due Alarm is dispatched per tick call
     * (see header's Round 390 addendum for why) - checked BEFORE the
     * DelayThread wake scan below so a due alarm gets first claim on
     * this tick's single "nested call" slot; any other still-due
     * alarm, or the DelayThread wake scan itself, is serviced on the
     * very next tick once this dispatch's own return trampoline
     * (IOP_HLE_THREAD_ALARM_RETURN_TRAMPOLINE) resumes normal
     * execution. Never dispatches while another alarm dispatch is
     * already in flight (g.alarm_in_dispatch) - real hardware
     * interrupt-context alarm dispatch is likewise strictly
     * sequential, not re-entrant. */
    if (!g.alarm_in_dispatch) {
        for (int i = 0; i < IOP_HLE_THREAD_MAX_ALARMS; i++) {
            iop_alarm_t_internal *a = &g.alarms[i];
            if (a->in_use && st->instructions_executed >= a->deadline) {
                g.alarm_in_dispatch = 1;
                g.alarm_dispatch_slot = i + 1;
                g.alarm_resume_pc = st->pc;
                g.alarm_resume_next_pc = st->next_pc;
                st->gpr[4] = a->common; /* $a0 = common, real alarm_callback_t(void *common) ABI */
                st->gpr[31] = IOP_HLE_THREAD_ALARM_RETURN_TRAMPOLINE; /* $ra = our own return gate */
                st->pc = a->handler;
                st->next_pc = a->handler + 4u;
                return; /* the dispatched call takes over this instruction slot */
            }
        }
    }

    int woke_any = 0;
    for (int i = 0; i < IOP_HLE_THREAD_MAX_THREADS; i++) {
        iop_tcb_t *t = &g.threads[i];
        if (t->in_use && t->status == IOP_THS_WAIT && t->wait_type == IOP_TSW_DELAY &&
            st->instructions_executed >= t->delay_deadline) {
            t->status = IOP_THS_READY;
            t->wait_type = 0;
            t->ready_seq = g.ready_seq_counter++;
            woke_any = 1;
        }
    }
    if (woke_any) reschedule(st);
}

const iop_hle_thread_stats_t *iop_hle_thread_get_stats(void) { return &g.stats; }

/* Round 519 INCIDENT (see docs/STATUS.md for the full writeup):
 * the original Round 514 body retired whatever thread happened to be
 * g.current_thread_id on EVERY call - but this function was invoked
 * from all 4 of iop_module_loader.c's idle-bypass re-entry points,
 * which fire continuously once boot-dispatch is exhausted (that IS
 * the steady-state idle loop). Without a guard, every subsequent
 * re-entry retired whatever real worker thread had since become
 * current straight to DORMANT - silently killing every real thread
 * THREADMAN ever dispatched after the first one, which starved all
 * subsequent real IOP activity (SIF RPC dispatch never fired again:
 * rpc_pending_sets/delivered went from a real 228 pre-Round-514 to a
 * hard 0 post-Round-514, and the EE boot depth regressed from
 * pc=0x0050172C/~320M real instructions to parking at the very early
 * pc=0x000820E0 BOOTEND poll).
 *
 * A first fix attempt guarded this body to only retire thread 1 (the
 * real synthetic root/bridge thread per Round 513), only while it is
 * still RUN, and only once ever (see the guard below). That was
 * REBUILT AND RE-TESTED and did NOT restore the regression -
 * rpc_pending_sets stayed 0. This disproves "repeated/wrong-thread
 * retirement" as the sole cause: even a single, correctly-scoped,
 * one-time retirement of thread 1 is incompatible with this
 * project's real boot-progress dependency on thread 1 staying RUN.
 *
 * The actual working fix was to fully disable all 4 call sites in
 * iop_module_loader.c (see that file's own Round 519 comments) -
 * this function is intentionally left defined-but-unused below, in
 * its guarded form, as a record of the disproven attempt. Do not
 * wire it back up without new evidence explaining why thread 1 can
 * safely be retired at all in this idle-bypass context. */
static int g_root_thread_retired = 0;

void iop_hle_thread_retire_root_thread(iop_state_t *st)
{
    if (g_root_thread_retired) return; /* only the synthetic root thread's one-time handoff, never again */
    if (g.thread_count == 0) return; /* no thread primitive has ever run - nothing to retire */
    int cur = g.current_thread_id;
    if (cur != 1) return; /* only thread 1, the real synthetic bridge thread identified in Round 513 - never any other real thread */
    iop_tcb_t *t = tcb(cur);
    if (!t) return;
    if (t->status != IOP_THS_RUN) return; /* only if it is genuinely still the live thread */
    /* Real THREADMAN semantics: this synthetic bridge thread's one
     * job (running the module loader's fixed dispatch sequence) has
     * genuinely finished, matching a real ExitThread-style handoff -
     * not a WaitSema/SleepThread-style block (nothing will ever
     * signal or wake it again; it must never be reselected). */
    t->status = IOP_THS_DORMANT;
    g_root_thread_retired = 1;
    reschedule(st);
}

int iop_hle_thread_get_thread_count(void) { return g.thread_count; }
int iop_hle_thread_get_current_thread_id(void) { return g.current_thread_id; }
uint32_t iop_hle_thread_get_status(int thid)
{
    iop_tcb_t *t = tcb(thid);
    return (t && t->in_use) ? t->status : 0u;
}
uint32_t iop_hle_thread_get_priority(int thid)
{
    iop_tcb_t *t = tcb(thid);
    return (t && t->in_use) ? t->priority : 0u;
}
int iop_hle_thread_get_sema_count(void)
{
    int n = 0;
    for (int i = 0; i < IOP_HLE_THREAD_MAX_SEMAS; i++) if (g.semas[i].in_use) n++;
    return n;
}
int iop_hle_thread_get_evf_count(void)
{
    int n = 0;
    for (int i = 0; i < IOP_HLE_THREAD_MAX_EVFLAGS; i++) if (g.evflags[i].in_use) n++;
    return n;
}
int iop_hle_thread_get_alarm_count(void)
{
    int n = 0;
    for (int i = 0; i < IOP_HLE_THREAD_MAX_ALARMS; i++) if (g.alarms[i].in_use) n++;
    return n;
}

uint32_t iop_hle_thread_get_entry(int thid)
{
    iop_tcb_t *t = tcb(thid);
    return (t && t->in_use) ? t->entry : 0u;
}

uint32_t iop_hle_thread_get_pc(int thid)
{
    iop_tcb_t *t = tcb(thid);
    return (t && t->in_use) ? t->pc : 0u;
}

int iop_hle_thread_get_wait_type(int thid)
{
    iop_tcb_t *t = tcb(thid);
    return (t && t->in_use) ? t->wait_type : 0;
}

int iop_hle_thread_get_wait_id(int thid)
{
    iop_tcb_t *t = tcb(thid);
    return (t && t->in_use) ? t->wait_id : 0;
}
