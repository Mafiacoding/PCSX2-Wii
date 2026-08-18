#ifndef PCSX2WII_EE_HLE_THREAD_H
#define PCSX2WII_EE_HLE_THREAD_H

#include <stdint.h>
#include "core/ee/ee_core.h"

/*
 * ee_hle_thread.h - clean-room EE (R5900) kernel thread scheduler and
 * semaphore implementation (Round 569+).
 *
 * BACKGROUND: Rounds 567/568 root-caused this project's central
 * remaining EE architectural gap, directly mirroring the IOP-side gap
 * closed by iop_hle_thread.h/.c back in Round 389: real Tekken Tag
 * Tournament game code (confirmed executing for the first time on the
 * disc-boot path in Round 567) creates a locked semaphore
 * (CreateSema, count=0/max=1) and immediately WaitSema()s on it - the
 * real "create a locked gate, kick off a second thread's real init
 * work, wait for it to finish and signal" idiom used throughout real
 * ps2sdk startup code (see this project's own WaitSema syscall-68
 * comment in ee_core.c, tasks #189/#190). This project's previous
 * model has exactly ONE schedulable EE register context and modeled
 * WaitSema as a pure busy-park with no actual second thread ever
 * running - so the real signal this park needs never arrives, and it
 * honestly (not silently) parks forever. Round 568's diagnostic
 * experiment (synthetically releasing the park without actually
 * running the "other thread") confirmed this is a structural gap, not
 * a single missing signal: real progress requires a real second
 * thread to actually execute and do its own real init work.
 *
 * ROUND 569 EXPERIMENT AND ITS NEGATIVE RESULT (important context for
 * why this file exists instead of a smaller patch): CreateThread(32)/
 * StartThread(34)/etc were already being let "vector for real" as a
 * genuine MIPS Syscall exception into the real, unmodified BIOS ROM's
 * own kernel exception handler (Round 240's design, ee_core.c's own
 * "sysnum == 32 || 33 || ..." family) - so it seemed plausible that
 * simply ALSO letting CreateSema(64)/DeleteSema(65)/SignalSema(66/
 * -67)/WaitSema(68)/SetupThread(60) vector for real (instead of this
 * project's own separate g_ee_sema[] C-HLE table) would let the SAME
 * real BIOS-resident kernel code handle everything consistently,
 * with no new C-level scheduler needed at all. Tested directly
 * (scratch-instrumented ee_core.c, /tmp/r569_exp_ee_core.c): this
 * REGRESSED the previously-working true-diskless BIOS boot (GS never
 * configures across 1.28+ billion instructions, vs. the established
 * baseline reaching pmode=0x66 by ~90M) - real-vectoring these
 * specific syscalls does NOT work correctly in this project's
 * environment (most likely because the real BIOS kernel's own
 * WaitSema-park scheduling algorithm depends on precise, real
 * Timer/COP0 Count-Compare-driven preemption semantics this project's
 * interrupt delivery doesn't reproduce closely enough for THIS
 * specific real code path, unlike the simpler CreateThread/StartThread
 * real-vectoring case that only touches TCB memory once and returns).
 * This experiment was reverted; no tracked source was changed by it.
 * CONCLUSION: real-vectoring is not viable for the semaphore/thread
 * family as a whole. This file instead builds a project-owned C-level
 * scheduler - the SAME architecture already proven correct for the
 * IOP side (iop_hle_thread.h/.c, Round 389+) - and therefore ALSO
 * takes over CreateThread/StartThread/etc (moving them OFF real
 * exception-vectoring and onto this file's own TCB table), since
 * mixing "real BIOS owns the TCB format" with "our own C module owns
 * WaitSema" would leave two incompatible views of what threads exist.
 *
 * DESIGN: exactly the same scheduling model as iop_hle_thread.h (see
 * that file's own header for the fuller physical-mechanism rationale,
 * ported here 1:1): ee_state_t's single live register file (gpr[32]/
 * pc/next_pc/hi/lo/sa_reg) IS "whichever thread is currently running",
 * exactly as on real hardware (one physical register file, reloaded
 * on every context switch). Each TCB holds a full saved copy of that
 * same state for every OTHER thread. Any primitive that can change
 * readiness (StartThread, WaitSema blocking, SignalSema/WakeupThread
 * waking a thread, SleepThread blocking, ChangeThreadPriority,
 * RotateThreadReadyQueue, thread exit) invokes this file's internal
 * reschedule(): if the highest-priority READY thread is not the one
 * currently live, the live register file is saved into the outgoing
 * thread's TCB and the incoming thread's own saved TCB state is
 * loaded into the live register file - a plain struct copy, matching
 * real hardware's own physical mechanism exactly. ee_step() simply
 * keeps fetching/decoding/executing from wherever pc now points,
 * seamlessly continuing whichever thread the scheduler just selected.
 * COP0 (Status/Cause/EPC/etc) is intentionally NOT part of the saved
 * per-thread context (same simplification as the IOP side) - real EE
 * threads run at the same COP0 privilege level; only GPR/PC/HI-LO/SA
 * are genuinely per-thread-distinct in this project's own interrupt/
 * exception model.
 *
 * INTEGRATION: unlike the IOP side (which intercepts real IRX-module
 * import calls redirected to project-owned sentinel PC addresses,
 * since the IOP boots via loading real relocatable modules), the EE
 * side already has an established, working "intercept before executing
 * the real syscall trap" mechanism keyed on the decoded syscall number
 * ($v1 at the point of a real `syscall` instruction) - see ee_core.c's
 * own `int32_t sysnum = (int32_t)GPR(3);` dispatch point. This file's
 * single entry point, ee_hle_thread_try_handle(), is called from
 * exactly that spot, BEFORE any of ee_core.c's own pre-existing
 * per-sysnum blocks for the same numbers (32-57's real-vectoring
 * family, 60/64-68's old g_ee_sema-based C-HLE family) - since this
 * function returns 1 (already fully handled, pc/next_pc already
 * advanced) for every syscall number it recognizes, those older
 * blocks simply become unreachable dead code for those specific
 * numbers without needing to be deleted (a safe, minimally invasive
 * integration - cleanup can happen in a later round).
 *
 * Real syscall numbers (verbatim from the user-supplied ps2sdk-master
 * archive, ee/kernel/include/syscallnr.h, AFL-2.0):
 *   0x20(32) CreateThread        0x2f(47)  GetThreadId
 *   0x21(33) DeleteThread        -0x2f(-47) _iGetThreadId
 *   0x22(34) StartThread         0x30(48)  ReferThreadStatus
 *   0x23(35) ExitThread          -0x31(-49) iReferThreadStatus
 *   0x24(36) ExitDeleteThread    0x32(50)  SleepThread
 *   0x25(37) TerminateThread     0x33(51)  WakeupThread
 *   -0x26(-38) iTerminateThread  -0x34(-52) _iWakeupThread
 *   0x27(39) DisableDispatchThread 0x35(53) CancelWakeupThread
 *   0x28(40) EnableDispatchThread  -0x36(-54) iCancelWakeupThread
 *   0x29(41) ChangeThreadPriority
 *   -0x2a(-42) iChangeThreadPriority
 *   0x2b(43) RotateThreadReadyQueue
 *   -0x2c(-44) _iRotateThreadReadyQueue
 *   0x3c(60) SetupThread (RFU060) - kept as-is (ee_core.c's existing
 *     handling, NOT moved into this file - it's used by every single
 *     boot path including the already-proven-working diskless one,
 *     and doesn't need real scheduling, just $gp/$sp computation for
 *     the calling thread's OWN startup - out of scope/unnecessary risk).
 *   0x40(64) CreateSema    0x41(65) DeleteSema
 *   0x42(66) SignalSema    -0x43(-67) iSignalSema
 *   0x44(68) WaitSema      0x45(69) PollSema
 *
 * Real struct layouts (ee/kernel/include/kernel.h, AFL-2.0):
 *   ee_thread_t (CreateThread param, 0x24 bytes): status@0 (unused by
 *     CreateThread itself), func@4, stack@8, stack_size@0xC,
 *     gp_reg@0x10, initial_priority@0x14, current_priority@0x18,
 *     attr@0x1C, option@0x20.
 *   ee_thread_status_t (ReferThreadStatus out param, 0x30 bytes):
 *     status@0, func@4, stack@8, stack_size@0xC, gp_reg@0x10,
 *     initial_priority@0x14, current_priority@0x18, attr@0x1C,
 *     option@0x20, waitType@0x24, waitId@0x28 (project extension -
 *     not in the fetched header snippet, matches IOP-side convention).
 *   Status bits: THS_RUN=1, THS_READY=2, THS_WAIT=4, THS_SUSPEND=8,
 *     THS_WAITSUSPEND=0xC, THS_DORMANT=0x10.
 *   Wait-type: TSW_NONE=0, TSW_SLEEP=1, TSW_SEMA=2 (real EE kernel.h
 *     numbering - deliberately DIFFERENT from the IOP's thbase.h
 *     TSW_SLEEP=1/TSW_DELAY=2/TSW_SEMA=3; not interchangeable).
 *   ee_sema_t (CreateSema param): { count, max_count, init_count,
 *     wait_threads; u32 attr, option; } - EXACT SAME field offsets
 *     already established and used by ee_core.c's pre-existing
 *     g_ee_sema-based CreateSema (task #188): max_count@4,
 *     init_count@8, attr@0x10, option@0x14. This file's own CreateSema
 *     reads the identical offsets for consistency with any existing
 *     citation/expectation elsewhere in the project's docs.
 *
 * SCOPE (first version, Round 569): thread lifecycle (CreateThread/
 * DeleteThread/StartThread/ExitThread/ExitDeleteThread/
 * TerminateThread/DisableDispatchThread/EnableDispatchThread/
 * ChangeThreadPriority/RotateThreadReadyQueue/GetThreadId/
 * ReferThreadStatus/SleepThread/WakeupThread/CancelWakeupThread) and
 * semaphores (CreateSema/DeleteSema/SignalSema/iSignalSema/WaitSema/
 * PollSema). EventFlags, Alarms, and DelayThread are explicitly OUT
 * OF SCOPE this round (same staged-scope precedent as the IOP side's
 * own Round 389 first version) - left for a future round if real boot
 * traffic is found to need them.
 *
 * IMPLICIT ROOT THREAD: exactly the IOP side's own precedent -
 * ensure_root_thread() lazily synthesizes TCB slot 1 representing
 * "whatever EE code was already running" the first time any thread
 * primitive in this file is invoked, so it can be saved/restored/
 * pre-empted exactly like any real thread from that point on.
 */

void ee_hle_thread_init(void);

/* Returns 1 if this exact (sysnum) was recognized and fully handled
 * (st->pc/next_pc already advanced or already redirected by a real
 * context switch - caller should just `return 1;` from ee_step()
 * immediately), 0 if not one of this file's syscalls (caller should
 * fall through to its own further sysnum checks as normal). */
int ee_hle_thread_try_handle(ee_state_t *st, int32_t sysnum, uint32_t this_pc, int in_delay_slot);

/* Diagnostics (mirrors iop_hle_thread's own get_thread_count/etc). */
int ee_hle_thread_get_thread_count(void);
int ee_hle_thread_get_current_thread_id(void);
uint32_t ee_hle_thread_get_status(int thid);
uint32_t ee_hle_thread_get_priority(int thid);

/* Round 612 (task #536, user-supplied real ps2sdk kernel.h confirms
 * ee_thread_status_t really has waitType@0x24/waitId@0x28 - this
 * project's own TCB already tracks the identical fields internally
 * (wait_type/wait_id, set by WaitSema/SleepThread, cleared by
 * SignalSema/WakeupThread - see ee_hle_thread.c) and already exposes
 * them to GUEST code via ReferThreadStatus's 0x24/0x28 writes, but
 * had no HOST-native accessor for this project's own diagnostic
 * drivers - added to directly answer "what is thread N blocked on"
 * instead of inferring it from indirect symptoms. Returns EE_TSW_NONE
 * (0) for an invalid thid, matching get_status()/get_priority()'s
 * existing safe-default convention. */
uint32_t ee_hle_thread_get_wait_type(int thid);
uint32_t ee_hle_thread_get_wait_id(int thid);

/* Round 597 (task #447/#536): forced preemption. Call once per genuine
 * instruction boundary from ee_step() (same convention as
 * ee_check_timer_interrupt()/ee_check_intc_interrupt()/
 * ee_check_dmac_interrupt()). Switches to a READY thread only if its
 * priority is strictly better (numerically lower) than the currently-
 * RUNNING thread's own priority - a no-op otherwise, and a no-op
 * entirely before this project's own scheduler has been engaged
 * (thread_count==0). See ee_hle_thread.c's own definition for the
 * full rationale (Round 596 found a real, higher-priority, already-
 * woken thread that was never getting scheduled because reschedule()
 * is otherwise only invoked from specific syscall handlers). */
void ee_hle_thread_check_preempt(ee_state_t *st);

/* Round 575 (task #550): opaque state-blob accessor for host-native
 * checkpoint/resume tooling. Unlike gs_get_state()/vif0_get_state()/
 * etc. (typed pointers, for callers needing field-level access), this
 * file's static TCB/semaphore scheduler state has no such caller -
 * a raw blob is simpler and just as sufficient for a memcpy-based
 * checkpoint format (memcpy-based save/restore). Fills the output
 * pointer and size with the address/size of this
 * file's entire static state; a checkpoint writer memcpy()s *size
 * bytes from *ptr, a checkpoint reader memcpy()s them back (this
 * struct has no internal pointers, so a raw byte restore is safe -
 * unlike iop_heap.c's g_alloclist, see that file's citation). */
void ee_hle_thread_get_checkpoint_blob(void **ptr, uint32_t *size);

#endif
