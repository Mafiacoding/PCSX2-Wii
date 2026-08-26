/*
 * ee_hle_thread.c - see include/core/ee/ee_hle_thread.h for the full
 * design rationale, citations, and the Round 569 real-vectoring
 * experiment negative result that motivated this file.
 */
#include <string.h>
#include "core/ee/ee_hle_thread.h"

#define EE_HLE_THREAD_MAX_THREADS 32
#define EE_HLE_THREAD_MAX_SEMAS   64

/* Real status bits (ee/kernel/include/kernel.h). */
#define EE_THS_RUN         0x01u
#define EE_THS_READY       0x02u
#define EE_THS_WAIT        0x04u
#define EE_THS_SUSPEND     0x08u
#define EE_THS_WAITSUSPEND 0x0Cu
#define EE_THS_DORMANT     0x10u

/* Real wait-type values (ee/kernel/include/kernel.h - DIFFERENT
 * numbering from the IOP side's thbase.h, see header comment). */
#define EE_TSW_NONE 0
#define EE_TSW_SLEEP 1
#define EE_TSW_SEMA  2

/* A fixed, honestly-labeled simplification (same spirit as the IOP
 * side's own THREAD_STACK_ARENA): real CreateThread takes the stack
 * pointer/size directly from the caller-supplied ee_thread_t struct
 * (the game itself owns/allocates the stack memory, unlike the IOP's
 * SYSMEM-backed allocation) - so no bump arena is needed here at all,
 * simplifying this file relative to its IOP counterpart. */

typedef struct {
    int in_use;
    uint32_t status;
    uint32_t attr, option;
    uint32_t entry;      /* func */
    uint32_t stack_base, stack_size;
    uint32_t gp_reg;
    uint32_t priority;      /* current */
    uint32_t init_priority;
    int wait_type;    /* EE_TSW_* */
    int wait_id;       /* sema id when wait_type==EE_TSW_SEMA */
    uint32_t wakeup_count;
    uint32_t ready_seq;

    /* Saved register context - meaningful whenever this thread is NOT
     * the currently-live one (see header's scheduling-model comment). */
    ee_reg128_t gpr[32];
    uint32_t pc, next_pc;
    ee_reg128_t hi, lo;
    uint32_t sa_reg;
} ee_tcb_t;

typedef struct {
    int in_use;
    uint32_t attr, option;
    int32_t max_count;
    int32_t count;
    int32_t wait_threads;
} ee_sema_internal_t;

static struct {
    ee_tcb_t threads[EE_HLE_THREAD_MAX_THREADS];
    int thread_count;
    int current_thread_id; /* 1-based; 0 = none yet */
    uint32_t ready_seq_counter;

    ee_sema_internal_t semas[EE_HLE_THREAD_MAX_SEMAS];
} g;

/* Round 733 (task #447, GT3-in-game-code stall investigation, user:
 * "use all sources available to track down this issue and fix them
 * once and for all"): live per-target call counters for WakeupThread/
 * _iWakeupThread and SignalSema/iSignalSema - diagnostic-only, same
 * "project-internal accessor" convention as Round 732's CDVD dispatch
 * counters (iop_cdvd.c). Deliberately NOT part of the checkpointed `g`
 * blob (mirrors Round 732's choice not to checkpoint its counters
 * either) - these answer a live, per-process-run empirical question
 * ("does ANY thread ever call WakeupThread(3) or WakeupThread(5) - GT3's
 * own two real, currently-sleeping worker threads found parked with
 * wait_type=TSW_SLEEP/wakeup_count=0 - or SignalSema on whatever they
 * might really be gated behind"), not something that needs to survive
 * a save/resume cycle. */
static uint64_t g_wakeup_call_count[EE_HLE_THREAD_MAX_THREADS + 1];
static uint64_t g_signal_call_count[EE_HLE_THREAD_MAX_SEMAS + 1];

/* Round 733 continuation (task #447): the force-wake diagnostic proved
 * threads 3/5 stay READY-but-never-scheduled forever even once woken,
 * because pick_next_ready()'s FIFO tiebreak always favors whichever
 * same-priority thread has the OLDEST ready_seq - and the currently-
 * RUNNING thread never loses that advantage unless something calls
 * RotateThreadReadyQueue() (sysnum 43/-44) on its own priority level
 * (the real PS2 kernel's documented fairness mechanism - real games
 * that run multiple same-priority worker threads are expected to call
 * this periodically, typically from their main-loop/VSync handler).
 * This counter answers the empirical question "does GT3's thread 4
 * ever actually call it" - decisive for classifying whether this is a
 * real emulator dispatch gap or genuinely-not-yet-reached game code. */
static uint64_t g_rotate_call_count;
uint64_t ee_hle_thread_get_rotate_calls(void) { return g_rotate_call_count; }

void ee_hle_thread_init(void)
{
    memset(&g, 0, sizeof(g));
    memset(g_wakeup_call_count, 0, sizeof(g_wakeup_call_count));
    memset(g_signal_call_count, 0, sizeof(g_signal_call_count));
    g_rotate_call_count = 0;
}

uint64_t ee_hle_thread_get_wakeup_calls(int thid)
{
    if (thid < 0 || thid > EE_HLE_THREAD_MAX_THREADS) return 0;
    return g_wakeup_call_count[thid];
}

uint64_t ee_hle_thread_get_signal_calls(int semid)
{
    if (semid < 0 || semid > EE_HLE_THREAD_MAX_SEMAS) return 0;
    return g_signal_call_count[semid];
}

void ee_hle_thread_get_checkpoint_blob(void **ptr, uint32_t *size)
{
    *ptr = &g;
    *size = (uint32_t)sizeof(g);
}

static ee_tcb_t *tcb(int thid) /* thid is 1-based */
{
    if (thid < 1 || thid > EE_HLE_THREAD_MAX_THREADS) return NULL;
    return &g.threads[thid - 1];
}

/* Round 733 diagnostic-only (task #447, GT3 stall investigation): forces
 * a WAIT/SLEEP thread to READY, using the EXACT same real-hardware
 * transition as the sysnum==51 WakeupThread handler below (mirrors it
 * rather than calling it, since that handler is inlined in
 * ee_hle_thread_try_handle() and not separately callable). This is NOT
 * wired into any EE syscall path and never fires during organic
 * emulation - it exists purely so a scratch driver can test the
 * hypothesis "would GT3's threads 3/5 go on to do real work (e.g. issue
 * a CDVD read) if something had woken them", without first having to
 * locate why the real wake call never happens. Same diagnostic-injection
 * precedent as Round 568's synthetic WaitSema(0) signal test. */
void ee_hle_thread_debug_force_wakeup(int thid)
{
    ee_tcb_t *t = tcb(thid);
    if (!t || !t->in_use) return;
    if (t->status == EE_THS_WAIT && t->wait_type == EE_TSW_SLEEP) {
        t->status = EE_THS_READY;
        t->wait_type = EE_TSW_NONE;
        t->ready_seq = g.ready_seq_counter++;
    } else {
        t->wakeup_count++;
    }
}

static int alloc_tcb_slot(void)
{
    for (int i = 0; i < EE_HLE_THREAD_MAX_THREADS; i++) {
        if (!g.threads[i].in_use) return i + 1;
    }
    return 0;
}

static int alloc_sema_slot(void)
{
    /* Round 569 fix: real hardware (and this project's own prior,
     * proven-working g_ee_sema[] table) assigns 0-based semaphore
     * IDs - the first CreateSema() call returns id=0. This matters
     * because real BIOS/game code sometimes hardcodes low semaphore
     * IDs (e.g. the semid=0 WaitSema park traced in Round 567/568)
     * rather than always threading through CreateSema's return
     * value. Returning 1-based IDs here (the earlier, buggy version
     * of this function) silently shifted every real semaphore ID by
     * one and broke that hardcoded-ID assumption, which is what
     * regressed the diskless BIOS boot baseline (pmode stuck 0x0)
     * even in an otherwise-correct, non-blocking CreateSema call.
     * -1 (not 0) is the "table full" sentinel now, since 0 is a
     * legitimate id. */
    for (int i = 0; i < EE_HLE_THREAD_MAX_SEMAS; i++) {
        if (!g.semas[i].in_use) return i;
    }
    return -1;
}

static ee_sema_internal_t *sema(int semid)
{
    if (semid < 0 || semid >= EE_HLE_THREAD_MAX_SEMAS) return NULL;
    return &g.semas[semid];
}

static void ensure_root_thread(ee_state_t *st)
{
    if (g.thread_count > 0) return;
    ee_tcb_t *t = &g.threads[0];
    memset(t, 0, sizeof(*t));
    t->in_use = 1;
    t->status = EE_THS_RUN;
    t->priority = 64;
    t->init_priority = 64;
    t->ready_seq = g.ready_seq_counter++;
    memcpy(t->gpr, st->gpr, sizeof(t->gpr));
    t->pc = st->pc;
    t->next_pc = st->next_pc;
    t->hi = st->hi;
    t->lo = st->lo;
    t->sa_reg = st->sa_reg;
    g.thread_count = 1;
    g.current_thread_id = 1;
}

static void save_context(ee_state_t *st, int thid)
{
    ee_tcb_t *t = tcb(thid);
    if (!t) return;
    memcpy(t->gpr, st->gpr, sizeof(t->gpr));
    t->pc = st->pc;
    t->next_pc = st->next_pc;
    t->hi = st->hi;
    t->lo = st->lo;
    t->sa_reg = st->sa_reg;
}

static void load_context(ee_state_t *st, int thid)
{
    ee_tcb_t *t = tcb(thid);
    if (!t) return;
    memcpy(st->gpr, t->gpr, sizeof(t->gpr));
    st->pc = t->pc;
    st->next_pc = t->next_pc;
    st->hi = t->hi;
    st->lo = t->lo;
    st->sa_reg = t->sa_reg;
}

/* Real priority-based pick, identical algorithm to the IOP side's own
 * pick_next_ready() (see that file's comment for the full real-
 * semantics rationale): lowest priority NUMBER wins, ties broken by
 * earliest ready_seq. */
static int pick_next_ready(void)
{
    int best = 0;
    uint32_t best_prio = 0xFFFFFFFFu;
    uint32_t best_seq = 0xFFFFFFFFu;
    for (int i = 0; i < EE_HLE_THREAD_MAX_THREADS; i++) {
        ee_tcb_t *t = &g.threads[i];
        if (!t->in_use) continue;
        if (t->status != EE_THS_RUN && t->status != EE_THS_READY) continue;
        if (t->priority < best_prio || (t->priority == best_prio && t->ready_seq < best_seq)) {
            best = i + 1;
            best_prio = t->priority;
            best_seq = t->ready_seq;
        }
    }
    return best;
}

/* Core scheduling point - called after any operation that could
 * change which thread should be running. See header comment: a plain
 * struct copy in/out of the single live register file, matching real
 * hardware's own physical context-switch mechanism. */
static void reschedule(ee_state_t *st)
{
    int next = pick_next_ready();
    if (next == 0) {
        /* Nothing at all is ready - nothing meaningful to fall back
         * to on the EE side (unlike the IOP, this project has no
         * existing "idle" flag on ee_state_t) - simply leave the live
         * context exactly as-is (whatever the caller already set
         * st->pc/next_pc to, e.g. WaitSema's own park-by-not-
         * advancing-pc convention still applies as the honest last
         * resort when literally nothing is ready). */
        if (g.current_thread_id != 0) save_context(st, g.current_thread_id);
        return;
    }
    if (next != g.current_thread_id) {
        if (g.current_thread_id != 0) {
            ee_tcb_t *cur = tcb(g.current_thread_id);
            if (cur && cur->status == EE_THS_RUN) cur->status = EE_THS_READY;
            save_context(st, g.current_thread_id);
        }
        load_context(st, next);
        tcb(next)->status = EE_THS_RUN;
        g.current_thread_id = next;
    } else {
        ee_tcb_t *cur = tcb(g.current_thread_id);
        if (cur) cur->status = EE_THS_RUN;
    }
}

/* Round 734 diagnostic-only (task #447, GT3 stall investigation
 * continuation, user: "maybe its time we write our self some code
 * RotateThreadReadyQueue" - see this round's clarification: NOT a
 * proposal to make our scheduler auto-rotate on its own, since Round
 * 712's own cited research already established real EE kernel hardware
 * has no automatic timeslicing among equal-priority threads - that
 * would be fabricating non-real behavior (the exact Round 549 mistake).
 * Instead, this is a scratch-driver-callable injection point, applying
 * the EXACT same real transition as the sysnum==43/-44
 * RotateThreadReadyQueue handler below (mirrored, not called - that
 * handler is inlined in ee_hle_thread_try_handle()), immediately
 * followed by a real reschedule() so a context switch can actually
 * happen. This exists purely to test what threads 3/5 DO once they
 * finally get real CPU time - the Round 733 force_wakeup experiment
 * flipped their status to READY but never called reschedule()
 * afterward, so pick_next_ready()'s FIFO tiebreak (thread 4's
 * long-standing, older ready_seq always wins) meant nothing ever
 * visibly changed. This function is NOT wired into any EE syscall path
 * and never fires during organic emulation. */
void ee_hle_thread_debug_force_rotate(ee_state_t *st, int priority)
{
    int earliest = 0;
    uint32_t earliest_seq = 0xFFFFFFFFu;
    for (int i = 0; i < EE_HLE_THREAD_MAX_THREADS; i++) {
        ee_tcb_t *t = &g.threads[i];
        if (t->in_use && (int32_t)t->priority == priority &&
            (t->status == EE_THS_READY || t->status == EE_THS_RUN) &&
            t->ready_seq < earliest_seq) {
            earliest = i + 1;
            earliest_seq = t->ready_seq;
        }
    }
    if (earliest) tcb(earliest)->ready_seq = g.ready_seq_counter++;
    reschedule(st);
}

/* Wakes the earliest (FIFO, real default SA_THFIFO-equivalent - this
 * project's ee_sema_t doesn't expose a real SA_THPRI attribute bit in
 * its own already-established field layout, so FIFO-only is the
 * correct, honest default here) thread waiting on sema `semid`, if
 * any. Returns 1 if a waiter was woken (signal transferred directly,
 * count untouched, matching real semantics), 0 if none waiting. */
static int wake_one_sema_waiter(int semid)
{
    int best = 0;
    uint32_t best_seq = 0xFFFFFFFFu;
    for (int i = 0; i < EE_HLE_THREAD_MAX_THREADS; i++) {
        ee_tcb_t *t = &g.threads[i];
        if (!t->in_use || t->status != EE_THS_WAIT || t->wait_type != EE_TSW_SEMA || t->wait_id != semid)
            continue;
        if (best == 0 || t->ready_seq < best_seq) {
            best = i + 1;
            best_seq = t->ready_seq;
        }
    }
    if (best == 0) return 0;
    ee_tcb_t *t = tcb(best);
    t->status = EE_THS_READY;
    t->wait_type = EE_TSW_NONE;
    t->wait_id = 0;
    t->ready_seq = g.ready_seq_counter++;
    ee_sema_internal_t *s = sema(semid);
    if (s) s->wait_threads--;
    return 1;
}

int ee_hle_thread_try_handle(ee_state_t *st, int32_t sysnum, uint32_t this_pc, int in_delay_slot)
{
    (void)in_delay_slot;
    static const int32_t handled[] = {
        32, 33, 34, 35, 36, 37, -38, 39, 40, 41, -42, 43, -44,
        47, -47, 48, -49, 50, 51, -52, 53, -54,
        64, 65, 66, -67, 68, 69
    };
    int recognized = 0;
    for (size_t i = 0; i < sizeof(handled) / sizeof(handled[0]); i++) {
        if (handled[i] == sysnum) { recognized = 1; break; }
    }
    if (!recognized) return 0;

    ensure_root_thread(st);
    int cur = g.current_thread_id;
    uint32_t ra = (uint32_t)st->gpr[31].ud0;
#define EE_RET(v) do { st->gpr[2].ud0 = (uint64_t)(int64_t)(int32_t)(v); } while (0)
    /* Round 569 fix: every syscall completion (blocking or not) must
     * advance PC to the instruction AFTER the syscall, exactly like
     * this project's original, proven g_ee_sema[] handlers did
     * (st->pc = this_pc + 4u; st->next_pc = this_pc + 8u;). The
     * earlier version of this file instead jumped straight to $ra
     * ("return to caller") for every completion - which is wrong for
     * MIPS `syscall` semantics: $ra is NOT a call-return address for
     * this instruction (unlike `jal`), it's whatever the calling
     * ps2sdk stub function last set it to, and that same stub
     * function typically has its OWN code between the `syscall`
     * instruction and its eventual `jr $ra` (saving the return value,
     * restoring saved registers, etc). Jumping straight to $ra
     * silently skipped all of that every single time, which is what
     * actually regressed the diskless BIOS boot baseline (pmode
     * stuck at 0x0) - not any of the semaphore-specific bugs fixed
     * above, though those were real bugs too. (void)ra suppresses
     * the now-unused-variable warning if no branch below still needs
     * it. */
    (void)ra;
#define EE_ADVANCE() do { st->pc = this_pc + 4u; st->next_pc = this_pc + 8u; } while (0)

    if (sysnum == 32) {
        /* CreateThread(ee_thread_t *thread) - a0=param ptr. Real
         * field layout cited in header: func@4, stack@8,
         * stack_size@0xC, gp_reg@0x10, initial_priority@0x14. */
        uint32_t param = (uint32_t)st->gpr[4].ud0;
        uint32_t func = ee_mem_read32(st, param + 4u);
        uint32_t stack = ee_mem_read32(st, param + 8u);
        uint32_t stack_size = ee_mem_read32(st, param + 0xCu);
        uint32_t gp_reg = ee_mem_read32(st, param + 0x10u);
        int32_t priority = (int32_t)ee_mem_read32(st, param + 0x14u);
        uint32_t attr = ee_mem_read32(st, param + 0x1Cu);
        uint32_t option = ee_mem_read32(st, param + 0x20u);
        int slot = alloc_tcb_slot();
        if (slot == 0) {
            EE_RET(-1);
        } else {
            ee_tcb_t *t = tcb(slot);
            memset(t, 0, sizeof(*t));
            t->in_use = 1;
            t->status = EE_THS_DORMANT;
            t->attr = attr; t->option = option;
            t->entry = func;
            t->stack_base = stack;
            t->stack_size = stack_size;
            t->gp_reg = gp_reg;
            t->priority = (uint32_t)priority;
            t->init_priority = (uint32_t)priority;
            if (slot > g.thread_count) g.thread_count = slot;
            EE_RET(slot);
        }
        EE_ADVANCE();
        return 1;
    }
    if (sysnum == 33) {
        /* DeleteThread(int thid) */
        int thid = (int)(int32_t)st->gpr[4].ud0;
        ee_tcb_t *t = tcb(thid);
        if (t && t->in_use && t->status == EE_THS_DORMANT) {
            t->in_use = 0;
            EE_RET(0);
        } else {
            EE_RET(-1);
        }
        EE_ADVANCE();
        return 1;
    }
    if (sysnum == 34) {
        /* StartThread(int thid, void *arg) - a0=thid, a1=arg. Real EE
         * crt0 thread entries take (void *arg) per ee_thread_t.func's
         * documented signature. */
        int thid = (int)(int32_t)st->gpr[4].ud0;
        uint32_t arg = (uint32_t)st->gpr[5].ud0;
        ee_tcb_t *t = tcb(thid);
        if (t && t->in_use && t->status == EE_THS_DORMANT) {
            memset(t->gpr, 0, sizeof(t->gpr));
            uint32_t stack_top = (t->stack_base + t->stack_size) & ~0xFu; /* real EE o32 16-byte SP alignment */
            t->gpr[29].ud0 = stack_top; /* $sp */
            t->gpr[28].ud0 = t->gp_reg; /* $gp - real, caller-supplied per-thread value (unlike IOP's inherited-gp simplification) */
            t->gpr[31].ud0 = 0u; /* $ra - real threads never return; treated as ExitThread-equivalent dead end if they do (matches real ps2sdk documented convention: entry functions call ExitThread themselves) */
            t->gpr[4].ud0 = (uint64_t)arg; /* $a0 */
            t->pc = t->entry;
            t->next_pc = t->entry + 4u;
            t->status = EE_THS_READY;
            t->ready_seq = g.ready_seq_counter++;
            EE_RET(0);
        } else {
            EE_RET(-1);
        }
        EE_ADVANCE();
        reschedule(st); /* the newly-READY thread may now pre-empt the caller if higher priority */
        return 1;
    }
    if (sysnum == 35 || sysnum == 36) {
        /* ExitThread() / ExitDeleteThread() - no args, no return. */
        if (cur) {
            tcb(cur)->status = EE_THS_DORMANT;
            if (sysnum == 36) tcb(cur)->in_use = 0;
        }
        reschedule(st);
        return 1;
    }
    if (sysnum == 37 || sysnum == -38) {
        /* TerminateThread(int thid) / iTerminateThread(int thid) */
        int thid = (int)(int32_t)st->gpr[4].ud0;
        ee_tcb_t *t = tcb(thid);
        if (t && t->in_use && thid != cur) {
            t->status = EE_THS_DORMANT;
            t->wait_type = EE_TSW_NONE; t->wait_id = 0;
            EE_RET(0);
        } else {
            EE_RET(-1);
        }
        EE_ADVANCE();
        return 1;
    }
    if (sysnum == 39 || sysnum == 40) {
        /* DisableDispatchThread/EnableDispatchThread - real kernel-
         * level preemption toggle around a critical section. This
         * project's HLE syscalls already run atomically with respect
         * to each other, so honored as a real, harmless no-op (same
         * established precedent as the IOP side's own identical
         * pair). */
        EE_RET(0);
        EE_ADVANCE();
        return 1;
    }
    if (sysnum == 41 || sysnum == -42) {
        /* ChangeThreadPriority(int thid, int priority) */
        int thid = (int)(int32_t)st->gpr[4].ud0;
        int32_t priority = (int32_t)st->gpr[5].ud0;
        ee_tcb_t *t = tcb(thid);
        if (t && t->in_use) {
            t->priority = (uint32_t)priority;
            EE_RET(0);
        } else {
            EE_RET(-1);
        }
        EE_ADVANCE();
        reschedule(st);
        return 1;
    }
    if (sysnum == 43 || sysnum == -44) {
        g_rotate_call_count++; /* Round 733 - see field comment */
        /* RotateThreadReadyQueue(int priority) - 0 = caller's own
         * current priority. Same real-equivalent implementation as
         * the IOP side: give the earliest-ready_seq thread at that
         * priority a fresh (latest) ready_seq. */
        int32_t priority = (int32_t)st->gpr[4].ud0;
        if (priority == 0 && cur) priority = (int32_t)tcb(cur)->priority;
        int earliest = 0;
        uint32_t earliest_seq = 0xFFFFFFFFu;
        for (int i = 0; i < EE_HLE_THREAD_MAX_THREADS; i++) {
            ee_tcb_t *t = &g.threads[i];
            if (t->in_use && (int32_t)t->priority == priority &&
                (t->status == EE_THS_READY || t->status == EE_THS_RUN) &&
                t->ready_seq < earliest_seq) {
                earliest = i + 1;
                earliest_seq = t->ready_seq;
            }
        }
        if (earliest) tcb(earliest)->ready_seq = g.ready_seq_counter++;
        EE_RET(0);
        EE_ADVANCE();
        reschedule(st);
        return 1;
    }
    if (sysnum == 47 || sysnum == -47) {
        /* GetThreadId() - no args. */
        EE_RET(cur);
        EE_ADVANCE();
        return 1;
    }
    if (sysnum == 48 || sysnum == -49) {
        /* ReferThreadStatus(int thid, ee_thread_status_t *info) */
        int thid = (int)(int32_t)st->gpr[4].ud0;
        uint32_t info = (uint32_t)st->gpr[5].ud0;
        ee_tcb_t *t = tcb(thid);
        if (t && t->in_use) {
            ee_mem_write32(st, info + 0x00u, t->status);
            ee_mem_write32(st, info + 0x04u, t->entry);
            ee_mem_write32(st, info + 0x08u, t->stack_base);
            ee_mem_write32(st, info + 0x0Cu, t->stack_size);
            ee_mem_write32(st, info + 0x10u, t->gp_reg);
            ee_mem_write32(st, info + 0x14u, t->init_priority);
            ee_mem_write32(st, info + 0x18u, t->priority);
            ee_mem_write32(st, info + 0x1Cu, t->attr);
            ee_mem_write32(st, info + 0x20u, t->option);
            ee_mem_write32(st, info + 0x24u, (uint32_t)t->wait_type);
            ee_mem_write32(st, info + 0x28u, (uint32_t)t->wait_id);
            EE_RET(0);
        } else {
            EE_RET(-1);
        }
        EE_ADVANCE();
        return 1;
    }
    if (sysnum == 50) {
        /* SleepThread() - no args. */
        ee_tcb_t *t = tcb(cur);
        if (t) {
            if (t->wakeup_count > 0) {
                t->wakeup_count--;
                EE_RET(0);
                EE_ADVANCE();
            } else {
                EE_RET(0); /* pre-set: the real return value once woken */
                EE_ADVANCE();
                t->status = EE_THS_WAIT;
                t->wait_type = EE_TSW_SLEEP;
                t->wait_id = 0;
                reschedule(st);
            }
        } else {
            EE_ADVANCE();
        }
        return 1;
    }
    if (sysnum == 51 || sysnum == -52) {
        /* WakeupThread(int thid) / _iWakeupThread(int thid) */
        int thid = (int)(int32_t)st->gpr[4].ud0;
        if (thid >= 0 && thid <= EE_HLE_THREAD_MAX_THREADS) g_wakeup_call_count[thid]++; /* Round 733 - see field comment */
        ee_tcb_t *t = tcb(thid);
        if (t && t->in_use) {
            if (t->status == EE_THS_WAIT && t->wait_type == EE_TSW_SLEEP) {
                t->status = EE_THS_READY;
                t->wait_type = EE_TSW_NONE;
                t->ready_seq = g.ready_seq_counter++;
            } else {
                t->wakeup_count++;
            }
            EE_RET(0);
        } else {
            EE_RET(-1);
        }
        EE_ADVANCE();
        reschedule(st);
        return 1;
    }
    if (sysnum == 53 || sysnum == -54) {
        /* CancelWakeupThread(int thid) / iCancelWakeupThread(int thid) */
        int thid = (int)(int32_t)st->gpr[4].ud0;
        ee_tcb_t *t = tcb(thid);
        if (t && t->in_use) {
            EE_RET((int32_t)t->wakeup_count);
            t->wakeup_count = 0;
        } else {
            EE_RET(-1);
        }
        EE_ADVANCE();
        return 1;
    }
    if (sysnum == 64) {
        /* CreateSema(ee_sema_t *sema) - same field offsets as this
         * project's pre-existing g_ee_sema-based implementation
         * (task #188), for consistency: max_count@4, init_count@8,
         * attr@0x10, option@0x14. */
        uint32_t param = (uint32_t)st->gpr[4].ud0;
        int32_t max_count = (int32_t)ee_mem_read32(st, param + 4u);
        int32_t init_count = (int32_t)ee_mem_read32(st, param + 8u);
        uint32_t attr = ee_mem_read32(st, param + 0x10u);
        uint32_t option = ee_mem_read32(st, param + 0x14u);
        int slot = alloc_sema_slot();
        if (slot < 0) {
            EE_RET(-1);
        } else {
            ee_sema_internal_t *s = sema(slot);
            memset(s, 0, sizeof(*s));
            s->in_use = 1;
            s->attr = attr; s->option = option;
            s->max_count = max_count;
            s->count = init_count;
            EE_RET(slot);
        }
        EE_ADVANCE();
        return 1;
    }
    if (sysnum == 65) {
        /* DeleteSema(int semid). Round 569 fix: match the original
         * handler's real-error precedent - refuse to delete (real
         * E_KERNEL_SEMA_STAT-style error) while threads are still
         * waiting, rather than silently deleting out from under a
         * waiter. */
        int semid = (int)(int32_t)st->gpr[4].ud0;
        ee_sema_internal_t *s = sema(semid);
        if (s && s->in_use) {
            if (s->wait_threads > 0) {
                EE_RET(-419); /* real error: threads still waiting */
            } else {
                s->in_use = 0;
                EE_RET(0);
            }
        } else {
            EE_RET(-1);
        }
        EE_ADVANCE();
        return 1;
    }
    if (sysnum == 66 || sysnum == -67) {
        /* SignalSema(int semid) / iSignalSema(int semid).
         *
         * Round 569 fix: this project's WaitSema busy-park idiom
         * (see the sysnum==68 handler above) resumes a parked thread
         * by simply re-executing the SAME WaitSema syscall and
         * re-checking s->count - it has no other channel for
         * "you were specifically signaled". The earlier version of
         * this function called wake_one_sema_waiter() FIRST and only
         * incremented count if no tracked waiter was found - a
         * "direct transfer" optimization that looks correct for a
         * real preemptive scheduler, but is fatal here: it marks the
         * waiter READY without ever bumping count, so that thread's
         * next WaitSema recheck still sees count==0 and re-parks
         * forever. This exact mismatch is what kept the diskless BIOS
         * boot baseline stuck at pmode=0x0 for the module's entire
         * test run. Fix: match the original, proven g_ee_sema[]
         * handler's semantics exactly - SignalSema ALWAYS increments
         * count (bounded by max_count); wake_one_sema_waiter() is
         * still called to eagerly flip a tracked waiter's status to
         * READY (harmless bookkeeping/an optional latency
         * optimization for reschedule()), but it no longer gates
         * whether count is incremented. */
        int semid = (int)(int32_t)st->gpr[4].ud0;
        if (semid >= 0 && semid <= EE_HLE_THREAD_MAX_SEMAS) g_signal_call_count[semid]++; /* Round 733 - see field comment */
        ee_sema_internal_t *s = sema(semid);
        if (s && s->in_use) {
            if (s->count < s->max_count) {
                s->count++;
                wake_one_sema_waiter(semid); /* bookkeeping only - does not gate the increment above */
                EE_RET(0);
            } else {
                EE_RET(-419); /* real E_KERNEL_SEMA_OVF-style error, matches original - EE_RET already sign-extends */
            }
        } else {
            EE_RET(-1);
        }
        EE_ADVANCE();
        if (sysnum == 66) reschedule(st); /* iSignalSema: interrupt context, defer any switch */
        return 1;
    }
    if (sysnum == 68) {
        /* WaitSema(int semid) - the exact real primitive Round 567/
         * 568 identified as this project's central remaining EE
         * architectural gap. */
        int semid = (int)(int32_t)st->gpr[4].ud0;
        ee_sema_internal_t *s = sema(semid);
        if (!s || !s->in_use) {
            EE_RET(-1);
            EE_ADVANCE();
            return 1;
        }
        if (s->count > 0) {
            s->count--;
            EE_RET(0);
            EE_ADVANCE();
        } else {
            /* Round 569 fix: must NOT call EE_ADVANCE() here.
             * This project's established park idiom (see ee_core.c's
             * original sysnum==68 handler, ~line 3179) is to leave
             * pc AT this_pc (re-execute the same syscall instruction
             * next step) rather than advancing to $ra - otherwise the
             * blocked thread's saved context resumes as if WaitSema
             * had already returned successfully, without the
             * semaphore ever actually being decremented. That earlier
             * (buggy) version of this function regressed the diskless
             * BIOS boot baseline (pmode stuck at 0x0) - this is the
             * fix, verified against that same baseline below. */
            ee_tcb_t *t = tcb(cur);
            EE_RET(0); /* pre-set: the real return value once actually woken and re-dispatched */
            st->pc = this_pc;
            st->next_pc = this_pc + 4u;
            if (t) {
                t->status = EE_THS_WAIT;
                t->wait_type = EE_TSW_SEMA;
                t->wait_id = semid;
                t->ready_seq = g.ready_seq_counter++;
            }
            s->wait_threads++;
            reschedule(st);
        }
        return 1;
    }
    if (sysnum == 69) {
        /* PollSema(int semid) - non-blocking WaitSema.
         *
         * Round 569 fix: this project's own Round 301 live-hardware
         * finding (see ee_core.c's original sysnum==69 handler,
         * ~line 6296) proved real PollSema's success path returns the
         * SEMAPHORE ID ITSELF (v0=semid), not a flat 0 - real OSDSYS
         * code (the 0x0020D478/0x0020E830/0x002034D0 device-comm
         * helper family, which this exact module's diskless-boot test
         * run got stuck cycling through) does an equality check
         * against that specific value. The earlier version of this
         * function returned a flat 0 on success (copying WaitSema's
         * own, separately-verified convention), silently
         * reintroducing the exact bug Round 301 already fixed once in
         * the original g_ee_sema[]-based handler. */
        int semid = (int)(int32_t)st->gpr[4].ud0;
        ee_sema_internal_t *s = sema(semid);
        if (s && s->in_use && s->count > 0) {
            s->count--;
            EE_RET(semid); /* real, live-traced: success returns the semaphore ID itself, not 0 */
        } else {
            EE_RET(-1);
        }
        EE_ADVANCE();
        return 1;
    }

    /* Recognized in the membership table above but not yet given a
     * concrete body (should not happen - every entry has a matching
     * block above); fail safe rather than silently mis-dispatch. */
    return 0;
#undef EE_RET
#undef EE_ADVANCE
#undef EE_ADVANCE
}

int ee_hle_thread_get_thread_count(void) { return g.thread_count; }
int ee_hle_thread_get_current_thread_id(void) { return g.current_thread_id; }
uint32_t ee_hle_thread_get_status(int thid)
{
    ee_tcb_t *t = tcb(thid);
    return t ? t->status : 0u;
}
uint32_t ee_hle_thread_get_priority(int thid)
{
    ee_tcb_t *t = tcb(thid);
    return t ? t->priority : 0u;
}
/* Round 612 (task #536): see header comment - read-only accessors for
 * the TCB's own already-tracked wait_type/wait_id fields. */
uint32_t ee_hle_thread_get_wait_type(int thid)
{
    ee_tcb_t *t = tcb(thid);
    return t ? (uint32_t)t->wait_type : 0u;
}
uint32_t ee_hle_thread_get_wait_id(int thid)
{
    ee_tcb_t *t = tcb(thid);
    return t ? (uint32_t)t->wait_id : 0u;
}

/* Round 613 (task #536): see header comment - expose entry/pc/wakeup_count
 * for host-native diagnostic drivers, to identify which real BIOS
 * function each parked thread belongs to. */
uint32_t ee_hle_thread_get_entry(int thid)
{
    ee_tcb_t *t = tcb(thid);
    return t ? t->entry : 0u;
}
uint32_t ee_hle_thread_get_saved_pc(int thid)
{
    ee_tcb_t *t = tcb(thid);
    return t ? t->pc : 0u;
}
uint32_t ee_hle_thread_get_wakeup_count(int thid)
{
    ee_tcb_t *t = tcb(thid);
    return t ? t->wakeup_count : 0u;
}

/* Round 597 (task #447/#536, following Round 596's finding): forced
 * preemption. This project's reschedule() is otherwise only invoked
 * from specific HLE syscall handlers above (StartThread/WakeupThread/
 * SleepThread/ChangeThreadPriority/RotateThreadReadyQueue/thread-exit/
 * SignalSema/WaitSema) - so a thread that never itself calls one of
 * those specific syscalls can starve a higher-priority READY thread
 * indefinitely, even after that thread has been made READY and even
 * signaled via a real WakeupThread() call. Round 596 found exactly
 * this: OSDSYS's real disc-browser dispatcher thread (entry=
 * 0x00204308, identified by disassembly) sits READY with a real,
 * better kernel priority than the currently-RUNNING animation-loop
 * thread, already woken (wakeup_count=1 at the point of discovery),
 * but never actually scheduled because nothing re-checks the ready
 * queue between syscalls. Real EE hardware avoids this via kernel-
 * level forced preemption on interrupt return (the real kernel's
 * exception/interrupt-return path always re-checks the ready queue
 * before restoring context) - this project's own C-level HLE
 * scheduler (Round 569) never had an equivalent, since it only ever
 * reacts to the specific syscalls above.
 *
 * Called once per genuine instruction boundary from ee_core.c's
 * ee_step(), in the exact same `if (!st->branch_pending)` block and
 * calling convention already used for ee_check_timer_interrupt()/
 * ee_check_intc_interrupt()/ee_check_dmac_interrupt() - a cheap
 * O(thread_count) scan (thread_count capped at
 * EE_HLE_THREAD_MAX_THREADS==32) that is a no-op until this project's
 * own scheduler has been engaged at all (thread_count==0, matching
 * every other check function's existing no-op-until-armed
 * convention).
 *
 * Deliberately conservative: only switches when the best real ready
 * priority is STRICTLY better (numerically lower) than the currently-
 * RUNNING thread's own priority - never merely because a same-or-
 * lower-priority thread is ready, so FIFO ordering among equal-
 * priority threads (pick_next_ready()'s own tiebreak) is never
 * disturbed by this function. This mirrors a real priority-preemptive
 * kernel's exact behavior (strictly-higher-priority-preempts, ties
 * don't), just checked far more frequently than real hardware's own
 * timer-tick granularity - an intentional, honest simplification
 * given this project has no cycle-accurate timing model (same
 * established precedent as e.g. ee_step()'s own COP0 Count-advances-
 * by-1-per-instruction comment immediately above in ee_core.c). */
void ee_hle_thread_check_preempt(ee_state_t *st)
{
    /* Round 598 (task #447/#536 follow-up, real regression fix): a real,
     * confirmed-via-user-supplied-disc regression was found in Round 597's
     * original version of this function - it had no guard against
     * preempting while the CPU is genuinely mid-hardware-exception
     * (COP0 Status.EXL or Status.ERL set). ee_check_timer_interrupt()/
     * ee_check_intc_interrupt()/ee_check_dmac_interrupt() immediately
     * above this function's own call site in ee_core.c's ee_step() all
     * correctly refuse to raise a NEW interrupt while EXL/ERL is set
     * (real MIPS semantics - exceptions do not nest by default), but
     * this function had no equivalent check, so it could - and, per
     * Round 598's host-native evidence against the real Tekken Tag
     * Tournament (Europe) (Demo) disc image, DID - swap out the "current
     * thread"'s live register file (including pc) while that thread's
     * CPU state actually belonged to a real EE interrupt handler
     * (Status.EXL=1, COP0 EPC holding the real interrupted return
     * address). A real interrupt-return sequence (ERET) assumes it will
     * resume exactly the context that was live when the exception was
     * taken; this function silently substituting a DIFFERENT thread's
     * saved context in between corrupts that assumption - the newly-
     * loaded thread's code then runs with EXL still set (interrupts
     * effectively masked) and no correct path back to the original
     * caller, which is exactly the symptom Round 598 measured: the real
     * Tekken disc-boot animation thread (tid 3) permanently trapped
     * cycling through EE kernel interrupt-dispatch code
     * (0x8000CC00-0x8000FA00) with zero further VU1/GS activity for
     * over a billion further instructions, instead of baseline's
     * (pre-Round-597) behavior of continuing to render real frames.
     * The fix mirrors the exact same EXL/ERL gating convention already
     * used by this file's three sibling interrupt-check functions -
     * simply refuse to switch threads while genuinely inside a hardware
     * exception; the CPU will still be re-checked on every subsequent
     * instruction boundary once ERET clears EXL, so the disc-browser-
     * dispatcher-starvation fix Round 597 was written for is preserved
     * (see this file's own header/definition comments for that
     * original rationale), just no longer firing during the one window
     * where doing so is unsafe. */
    if (st->cop0[12] & 0x6u) return; /* Status.EXL (bit 1) or Status.ERL (bit 2) set - genuinely mid-exception, never preempt here */
    /* Round 625 (task #536/#607, following Round 624's live-hardware-
     * verified findings): never freeze a thread's context while its PC
     * sits inside the real, fixed generic ERET-glue thread-start
     * trampoline - the exact 5-instruction body this project has fully
     * disassembled and byte-verified twice, independently, against the
     * real BIOS ROM (Round 467's "0x00081FE0 is a fixed generic kernel
     * thread-start trampoline; real entry point travels through $v1,
     * not $a0" finding; Round 619's byte-exact confirmation:
     *   0x00081FE0: lui   $sp, 8
     *   0x00081FE4: jalr  $v1        ; delay slot below
     *   0x00081FE8: addiu $sp, $sp, 0x1fc0
     *   0x00081FEC: addiu $v1, $zero, -5
     *   0x00081FF0: syscall
     * ). $v1 here is a real MIPS calling-convention register: the
     * trampoline's ONLY caller-supplied argument, which must already
     * hold a valid target address by the time control reaches
     * 0x00081FE0 - the trampoline body itself performs no load, no
     * null-check, and no guard of any kind before `jalr $v1` (Round
     * 619's own conclusion, quoted in STATUS.md: "There is no branch,
     * no zero-check, no guard of any kind between the trampoline's
     * entry and the jalr $v1"). Real EE hardware never has a problem
     * with this: an interrupt landing here is always followed by a
     * full, faithful, immediate context restore on return, so $v1
     * (already set by the real caller a few instructions earlier)
     * survives untouched. This project's OWN software scheduler is
     * different in one specific, consequential way - once
     * save_context() freezes a thread here, nothing guarantees WHEN
     * (or whether, for a very long time) load_context() will resume
     * it, unlike real hardware's effectively-instantaneous interrupt
     * round-trip. Round 622's own live-instrumented conclusion
     * ("$ra=0x800027AC... a leftover register value from BEFORE the
     * 2,000,000-slice observation window... set by a real call more
     * than ~154,500,000 slices earlier and never overwritten since")
     * and Round 624's direct RAM comparison (TIMER3's real per-cause
     * handler-table slot is genuinely unregistered/count=0, and the
     * real 0x80001630 dispatcher's own intact blez-guard would never
     * reach this trampoline for it at all) together rule out "the real
     * BIOS dispatch code is buggy" as the explanation - the evidence
     * instead points at exactly this window: this project's own forced
     * preemption (introduced Round 597, moved to fire only on real
     * ERET in Round 598) can still freeze whichever thread happens to
     * be mid-handoff here, for however long its priority keeps it off
     * the ready queue, and later resume it with $v1 stale/zero. The
     * fix is narrow and conservative - defer preemption by the few
     * instructions it takes the CPU to clear this window on its own,
     * exactly the same "genuinely unsafe software-yield point, unlike
     * real hardware which needs no such carve-out" reasoning as the
     * Status.EXL/ERL check immediately above. */
    if (st->pc >= 0x00081FE0u && st->pc < 0x00081FF0u) return;
    if (g.thread_count == 0 || g.current_thread_id == 0) return;
    ee_tcb_t *cur = tcb(g.current_thread_id);
    if (!cur || cur->status != EE_THS_RUN) return;
    int best = pick_next_ready();
    if (best == 0 || best == g.current_thread_id) return;
    ee_tcb_t *bt = tcb(best);
    if (bt && bt->priority < cur->priority) reschedule(st);
}
