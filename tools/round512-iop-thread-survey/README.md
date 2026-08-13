# Round 512: IOP thread-scheduler survey - corrects Round 511's framing

## Context

Round 511's docs framed the remaining task #447 blocker as "this
project's IOP core currently halts once it finishes running the fixed
set of modules discovered at boot (task #92's documented
simplification) rather than staying alive as an event-driven
scheduler," and scoped Round 512 as "design and implement persistent
IOP threading/scheduler" from scratch, per the user's instruction to
tackle this after the SIF2 DMA fix.

**That framing was stale.** Investigating `source/hw/iop_hle_thread.c`
(1321 lines) and its header (`include/core/hw/iop_hle_thread.h`, 373
lines) before writing any new code revealed a full, real, priority-
based preemptive THREADMAN scheduler was already implemented in an
earlier investigative arc (Round 389-411, referenced in this project's
own task history as "Implement real IOP multi-threading/context-
switching"). It implements real `CreateThread`/`StartThread`/
`WaitSema`/`SignalSema`/`SleepThread`/`WakeupThread`/EventFlags/Alarm
with real ps2sdk-cited ordinals, struct layouts, and priority
semantics (lowest priority number wins, ties broken by ready order).
Separately, `iop_module_loader.c`'s own "task #179" fix (already
landed, predating this session) already changed the post-module-
exhaustion behavior from a hard `halted` state to an `idle` (not
halted) state that stays genuinely interrupt-responsive.

Rather than duplicate this existing work, this round empirically
surveyed what the existing scheduler actually does during a real
organic boot, to find the REAL remaining gap.

## Method

Extended the Round 511 driver with `iop_hle_thread_get_thread_count()`
/`get_sema_count()`/`get_evf_count()`/`get_alarm_count()` (existing
diagnostic getters, not new this round) and built a second driver,
`driver.c` in this directory, that runs an organic boot to a fixed
instruction budget and dumps each real thread's status
(`iop_hle_thread_get_status()`, also pre-existing) via the real
`THS_RUN`/`THS_READY`/`THS_WAIT`/`THS_DORMANT` status bits.

## Result

Across a 20,000,000-instruction organic boot:

- **6 real threads, 3 real semaphores, 2 real event flags are created**
  from actual interpreted module init code (not synthetic/HLE
  shortcuts) - the THREADMAN scheduler is genuinely exercised, not
  dormant.
- Thread status snapshot: thread 1 = **RUN**, threads 2/3 = **DORMANT**,
  threads 4/5/6 = **READY** (created, schedulable, but never yet
  dispatched).
- This state is stable/flat from roughly instruction 6,000,000 onward
  through the full 20,000,000-instruction window (re-confirmed at
  finer granularity in the companion Round 511 driver's chunked runs,
  up to 60,000,000 instructions) - not a transient snapshot.
- The IOP's real PC oscillates between `0x00100000` (module-loader
  trampoline/idle-park address) and `0x80000080` (the real general
  exception vector) repeatedly, consistent with thread 1 fielding
  real, periodic hardware interrupts (most likely the IOP's own timer)
  and returning, without ever calling a THREADMAN primitive that would
  trigger `reschedule()` to reconsider which thread should run.

## Interpretation

This project's `reschedule()` (the scheduler's core dispatch point) is
correctly triggered from every real THREADMAN primitive that could
change readiness - it is not itself a bug. The observed starvation
(three real, live threads permanently READY while thread 1
permanently RUNs) is fully explained by two possibilities that this
round's evidence does not yet distinguish:

1. **Correct, real starvation-by-design**: if thread 1's real priority
   number is numerically ≤ threads 4/5/6's, real IOP hardware would
   behave identically - a higher-or-equal-urgency thread that never
   voluntarily yields legitimately monopolizes the CPU, exactly like
   this project's own scheduler doc comment describes ("real
   cooperative-within-priority scheduling"). This would mean whatever
   real module became thread 1 is, correctly, never relinquishing
   control - and the actual next question becomes what real code path
   SHOULD cause it to yield (e.g. a `WaitSema`/`SleepThread` call this
   project's interpreter isn't reaching, or real code it doesn't yet
   correctly execute).
2. **A genuine dispatch gap**: if thread 1's priority is actually LOWER
   urgency (numerically higher) than 4/5/6, real hardware would have
   switched away from it already, and something in this project's
   model is failing to trigger `reschedule()` at the right point.

This round's evidence (thread status only, no priority values) cannot
yet distinguish these two cases - identifying thread 1's real module
identity and each thread's actual priority number is the concrete,
scoped next step.

## Classification

Diagnostic/investigative round - no tracked emulator source changed
(the getters used already existed; only new scratch driver code was
added under `tools/`). Host-native regression suite and Wii cross-build
correctly skipped per this project's own established convention for
docs-only rounds.

## Correction to Round 511's docs

Round 511's `docs/STATUS.md` entry states the IOP "halts once it
finishes running the fixed set of modules... instead of staying alive
as an event-driven scheduler." This round's evidence shows that
framing is inaccurate as of the current tracked source: the IOP does
NOT halt (task #179's `idle`-not-`halted` fix already landed), and a
real, working, priority-based THREADMAN scheduler already exists and
is actively running multiple real threads. The actual remaining gap is
narrower and different in kind: understanding why the specific threads
that exist don't yet drive the SIF2/SBUS completion the EE is waiting
for - not "build a scheduler from scratch."

## Files

`driver.c` - the thread-status survey driver. Requires a real BIOS
dump (not included - see the leak-prevention rule in `docs/STATUS.md`).
