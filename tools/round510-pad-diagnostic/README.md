# Round 510: Pad-Command Diagnostic (uLaunchELF idle-loop follow-up)

## Context

Round 509 loaded uLaunchELF's real `BOOT.ELF` via this project's own
syscall-7 `_ExecPS2` trampoline (Rounds 457-469 mechanism). The ELF
loaded and dispatched correctly (entry address, exception-vector
landing, 640M-instruction crash-free run), but the EE ended up
resting in the same shared kernel idle-dispatch loop
(`0x8000CC8C`-`0x8000F868` family) already documented since Rounds
265-271 for Tekken's game code and `osdmenu.elf`.

Round 509's README speculatively suggested, as an unverified "possible
next step", that a mid-run pad-button press might unblock this loop.

The user's explicit instruction this round: **"if its an pad issue
fix it"** - i.e. actually test the hypothesis and fix it if true,
rather than speculate further.

## Method

Following this project's established hit-counter diagnostic
convention (`dispatch_ncmd()` call count, `sif_cmd_iop_get_init_cmd_count()`,
etc.), added a new counter to `source/hw/iop_sio2.c`:

- `g_pad_command_count` - incremented on every entry to
  `pad_process_command()`, the ONLY code path (`iop_sio2_mmio_write8()`
  control-register bit-0 trigger -> `mc_process_command()` ->
  `pad_process_command()`) that ever delivers real SIO2 pad-read
  command/response data to guest code.
- `iop_sio2_get_pad_command_count()` - public getter, exposed via
  `include/core/hw/iop_sio2.h`.

This is purely additive instrumentation: no existing function
signatures or logic were changed.

`driver.c` in this directory is the Round 509 trampoline driver
extended to print `pad_cmd_count` after warmup and at each of 10
sampled post-trampoline chunks (in addition to the existing PC/halted/
instruction-count/GS-state reporting).

## Result

`pad_cmd_count=0` at every single sample point, across the entire run:

- After 40M-instruction warmup: 0
- After trampoline dispatch, before post-run: 0
- At all 10 post-trampoline sampled chunks (up to ~800M total
  instructions this round, extending past Round 509's 640M): 0

The shared kernel idle-dispatch loop **never issues a single SIO2
pad-read command transaction**, regardless of how long it runs or
what button-hold state (`iop_sio2_pad_press()`/`_release()`) is set
beforehand.

## Conclusion

**This is not a pad issue.** The Round 509 "mid-run pad press" hypothesis
is falsified by direct instrumentation, not just reasoned away: the
loop the EE rests in never queries pad state at all, so no pad-input
timing fix (mid-run press, held press, released press, or anything
else pad-related) could possibly change its behavior.

This correctly redirects the diagnosis back to the already-documented,
much deeper blocker: the real IOP-EE SBUS/SIF2 completion handshake
(established Rounds 96/114/115/265-271, open as task #447) that this
project's various test harnesses - organic BIOS boot, disc-mounted
boot, and now the syscall-7 trampoline path for Tekken/osdmenu.elf/
uLaunchELF alike - have never delivered. All three programs land in
the identical loop for the identical underlying reason.

## Classification

No tracked-source fix needed beyond the diagnostic instrumentation
itself (which is intentionally kept - it is a useful permanent counter
for any future pad-related investigation, following project
convention). The real blocker is task #447 (SBUS/SIF2 handshake),
already tracked and out of scope for a pad-specific fix.

## Process note (regression-suite methodology)

While verifying this change, discovered that most of `tests/test_*.c`
are self-contained unit tests that embed their OWN duplicate copies of
the functions/globals under test, and are NOT meant to be linked
against the full `source/` tree (confirmed via linker "multiple
definition" errors). `tests/README.md`'s documented per-test compile
lines are also stale relative to the current source layout. For a
small additive change like this round's, the reliable regression
signal is: (a) a full standalone compile of every tracked `source/*.c`
file, and (b) directly running only the tests that reference the
changed symbols (`test_sio2_pad.c`, `test_iop_sio2_mc.c` - both still
PASS). Blanket-linking every test file against the full tree produces
false-negative "failures" that are actually just this pre-existing
per-test isolation design, not real regressions.

## Files

- `driver.c` - the extended Round 509/510 syscall-7 trampoline driver
  with pad-command-count instrumentation added to its reporting.
  Requires a real BIOS dump and uLaunchELF's `BOOT.ELF` (neither
  included here - see the leak-prevention rule in `docs/STATUS.md`).

No BIOS, disc, or third-party-ELF binary data is included in this
directory - source/diagnostic code only.
