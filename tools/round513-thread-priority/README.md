# Round 513: thread-1 identity + priority survey - classifies the Round 512 starvation finding

## Context

Round 512 discovered a real, pre-existing IOP THREADMAN scheduler (source/hw/iop_hle_thread.c,
Round 389+) already running organically during boot: 6 real TCBs get created from actual
interpreted module init code (not synthetic bookkeeping), but the scheduler settles into a
permanent pattern - thread 1 stays `RUN` forever, threads 2/3 go `DORMANT`, and threads 4/5/6
sit `READY` forever, never dispatched. Round 512 could not tell whether this is correct
real starvation-by-priority or a genuine scheduler dispatch gap, because two facts were
missing: each thread's actual priority number, and thread 1's real identity (what code is it
actually running at pc 0x00100000/0x80000080?).

This round adds the missing diagnostic getters, re-runs the survey, and traces the real
mechanism far enough to answer the question definitively.

## What was added

Two small, purely-additive diagnostic getters, following the exact pattern of every prior
round's `_get_status`/`_get_stats` accessors (no behavior change to any existing code path):

- `uint32_t iop_hle_thread_get_priority(int thid)` (include/core/hw/iop_hle_thread.h,
  source/hw/iop_hle_thread.c) - exposes a TCB's live `priority` field (thbase.h convention:
  lower number = more urgent, HIGHEST_PRIORITY=1, LOWEST_PRIORITY=126).
- `int iop_module_loader_get_module_count(void)`, `const char *iop_module_loader_get_module_name(int)`,
  `uint32_t iop_module_loader_get_module_entry(int)` (include/core/hw/iop_module_loader.h,
  source/hw/iop_module_loader.c) - exposes the real IOPBTCONF-derived module list
  (name + real ELF entry point) already tracked internally in `g.modlist[]`/`g.entry_points[]`,
  used here to check whether a live PC falls inside any real module's code.

`tools/round513-thread-priority/driver.c` runs a 20,000,000-slice organic BIOS boot (same
BIOS/budget class as Round 512's survey) and prints every live thread's status *and*
priority, the live IOP pc, and the full 29-module name/entry-point table.

## Result

```
[R513] thread_count=6 current_thread_id=1
[R513]   thread[1] status=0x1 (RUN) priority=64
[R513]   thread[2] status=0x10 (DORMANT) priority=8
[R513]   thread[3] status=0x10 (DORMANT) priority=10
[R513]   thread[4] status=0x2 (READY) priority=80
[R513]   thread[5] status=0x2 (READY) priority=96
[R513]   thread[6] status=0x2 (READY) priority=96
[R513] live IOP pc=0x00100000
[R513] module_count=29
[R513]   module[0] name='SYSMEM' entry=0x001000a0
  ... (all 29 real IOPBTCONF modules, entries 0x001000a0-0x00145b40)
```

Every real module's entry point is >= 0x001000a0. Thread 1's live pc, 0x00100000, is
**below every single real module's entry point** - it is not executing any real Sony
module code at all.

## Thread 1's real identity

`0x00100000` is exactly `BUMP_BASE` (source/hw/iop_module_loader.c `#define BUMP_BASE
0x00100000u`), and specifically it is `g.trampoline_addr` - the very first thing
`bump_alloc()` ever hands out during boot (source/hw/iop_module_loader.c line 772,
`g.trampoline_addr = bump_alloc(8)`, called before any module is loaded). This address is
written with a self-jump instruction and installed as `$ra` for every module's `_start()`
call (line 812/1018, `st->gpr[31] = g.trampoline_addr`) - it is this project's own
synthetic "module returned, nothing real to run here" landing pad, not real PS2 code.

`iop_module_loader_try_handle()`'s trap check (source/hw/iop_module_loader.c ~line
1286-1320) intercepts every re-entry to `pc == g.trampoline_addr`. Once `g.idle_transition_done`
is set (all 29 real modules have run to completion once - Round 425/426, task #179), every
subsequent re-entry just sets `g_iop.idle = 1` and returns - real CPU fetch/decode/execute is
skipped entirely while idle (source/core/iop/iop_core.c ~line 1770-1785, "Task #179 continued:
real IOP hardware never halts after boot"). The pc only ever leaves 0x00100000 to bounce to
0x80000080 (the real exception vector) when a genuine hardware interrupt fires, runs its
real handler, and RFEs back to the saved EPC - which is 0x00100000 again, re-triggering the
same trap and re-idling. This is exactly Round 512's observed oscillation, now fully
explained: it is the real, correct idle/wake-on-interrupt cycle, not module code.

So "thread 1" in the THREADMAN scheduler's bookkeeping is not a real Sony thread at all -
it is this project's own "IMPLICIT ROOT THREAD" (see include/core/hw/iop_hle_thread.h's own
design note), lazily bridging the module-loader's sequential dispatch loop into TCB slot 1
the first time any real thread primitive is called. Its priority, 64, is confirmed (by
reading source/hw/iop_hle_thread.c lines 161-172) to be an explicitly-uncited placeholder
default - the header comment there states outright "no real citation exists for what
priority the pre-THREADMAN boot context ... should carry."

## Classification: genuine dispatch/integration gap, not correct real starvation

Threads 4/5/6 are real - created by real module init code via real `CreateThread` calls,
carrying real assigned priorities (80, 96, 96). Under real THREADMAN priority rules, thread
1's priority-64 SHOULD legitimately outrank them (lower number = more urgent) for as long as
thread 1 is genuinely runnable. But thread 1 is not genuinely runnable once
`g.idle_transition_done` fires - the CPU-core level correctly recognizes this and sets
`g_iop.idle = 1` (task #179's real idle-not-halt fix, working exactly as designed for the
single-threaded pre-Round-389 model it was built for).

The gap: `g_iop.idle` (the CPU-core's own idle flag, source/core/iop/iop_core.c) and the
THREADMAN scheduler's TCB status (source/hw/iop_hle_thread.c, Round 389+) are two separate
mechanisms built in different rounds that were never integrated. `iop_hle_thread_tick()`
runs unconditionally every step (source/core/iop/iop_core.c line 1756, explicitly
"unconditional-even-while-idle" like the timer/VBLANK ticks beside it) and correctly wakes
DelayThread-expired threads, but nothing anywhere transitions thread 1's own TCB status away
from `RUN` when `g_iop.idle` becomes 1. `reschedule()` (source/hw/iop_hle_thread.c) only runs
from specific trigger events - StartThread, a thread blocking/waking on a semaphore or
event flag, SleepThread, ChangeThreadPriority, RotateThreadReadyQueue, or thread exit - and
"the CPU core going idle" is not one of them. So thread 1 stays the scheduler's `current_thread_id`
forever, at priority 64, permanently outranking threads 4/5/6's priority 80/96, even though
the actual CPU has correctly stopped doing any real work.

This is evidence for a genuine, precisely-located integration gap (not a "correct real
starvation" outcome to accept), but implementing a safe fix requires a real design decision
this round did not have room to make carefully: what TCB status should the implicit root
thread transition into when idle (WAIT would need a matching wake primitive; DORMANT/exit
would need to decide whether interrupt handlers still logically "belong" to it when they run
during idle), and whether real hardware's own boot flow provides a citeable answer (e.g. does
the real Sony loadcore main dispatch loop actually terminate/exit its own thread once
IOPBTCONF is exhausted, handing control to THREADMAN's real idle thread convention). That
citation search and the resulting fix are deferred to a follow-up round rather than rushed
here.

## Process note

Source change (two small diagnostic getters, zero behavior change to existing logic) -
full mandatory workflow applies: 129/129 host-native regression tests pass (56 built/passed
directly against precompiled objects, 73 required per-test conflicting-object exclusion for
tests that `#include` a .c file also in the shared object set - all still verified, zero
failures, zero unresolved build failures), Wii cross-build health check is clean across all
37 tracked source files (excluding the pre-existing, unrelated libfat/main.c gap documented
since Round 507).
