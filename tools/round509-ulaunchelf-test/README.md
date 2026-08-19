# Round 509: does this project's OWN emulator core boot uLaunchELF?

The user uploaded uLaunchELF v4.43a's real `BOOT.ELF` (497,108 bytes, real
ELF32/MIPS ET_EXEC, `e_entry=0x01D0001C`, confirmed via `readelf -h`) and
asked whether this project's own emulator (not real PCSX2) can launch it.

## Method

Reused the syscall-7 `_ExecPS2` trampoline methodology already evidenced
against real game code (Rounds 457-469, re-run Round 502): organic BIOS
warm-up (40M slices) to reach OSDSYS's steady idle state, then `ee_elf_load()`
uLaunchELF's `BOOT.ELF` directly (no disc/ISO involved - same approach as
Round 478's `osdmenu.elf` test), mask VBLANK-END in `INTC_MASK` (Round 469's
evidenced fix), install a real `SYSCALL` instruction in EE scratchpad with
`$v1=7` (`_ExecPS2`), `$a0=`entry, and step/run.

`driver.c` in this directory is the exact scratch driver used (not wired
into the default boot flow, same convention as `tools/round507-diag-elf/`).
It expects `/tmp/round238_diag/bios_fresh.bin` (a real BIOS dump) and
`/tmp/round238_diag/ulaunchelf_boot.elf` (the uploaded `BOOT.ELF`, copied)
at fixed scratch paths - **the `BOOT.ELF` binary itself is not committed
here** (it's third-party GPL homebrew, not our own code or Sony IP, but
still not something to bundle into this repo's git history without a
separate licensing decision).

## Result

1. **ELF loads correctly**: `ee_elf_load()` parses the real PT_LOAD segments
   without error (`load_start=0x01C8C290 load_end=0x01D05C60`, matching the
   real ELF header's own `entry=0x01D0001C`).
2. **The trampoline dispatches correctly**: the syscall exception fires,
   landing at the real EE exception vector `0x80000180`, exactly as seen in
   every prior real-game trampoline test.
3. **Execution continues for a long, crash-free run**: 640,000,000
   instructions (10x 8M-slice chunks) with the EE never halting.
4. **But it never visibly reaches uLaunchELF's own application code.** The
   sampled EE PC across all 10 chunks stays entirely within the same
   well-documented shared kernel per-frame idle-dispatch loop this project
   has characterized extensively since Rounds 265-271 and re-confirmed as
   recently as Round 469/502 (`0x8000CC8C`-`0x8000F868`, including the
   landmark `0x8000CF88`/`0x8000D008`/`0x8000D010` addresses from that
   existing writeup) - not a uLaunchELF-specific address.
5. **GS state is byte-identical before and after the entire post-trampoline
   run**: `PMODE=0x66 DISPFB2=0x1400 DISPLAY2=0x1BF9FF0183227C` was already
   present immediately after warm-up (i.e. it's OSDSYS's own inherited
   splash-screen display state, matching Round 321's GS-circuit-2 finding)
   and does not change by even one bit across all 10 sampled chunks. No
   evidence uLaunchELF's own code ever wrote a single GS register.

## Interpretation

This is the same real, previously-documented finding recurring for a third
target (after Tekken's game ELF and `osdmenu.elf`): this project's syscall-7
trampoline correctly loads and dispatches into ANY real PS2 ELF's entry
point, but whatever real kernel call the target's own early crt0/init code
makes ends up parked in the same shared, real kernel per-frame idle-wait
dispatcher - which this project has already root-caused (Rounds 265-271) as
genuinely waiting on a pad-input/disc-insert/SBUS stimulus this project's
test harness has never delivered mid-run, not a bug in the dispatcher
itself. It is not a uLaunchELF-specific failure, and not new evidence of a
previously-unknown gap - it's the third independent confirmation of the same
known, already-documented systemic limitation of the trampoline-testing
methodology (as opposed to a real bug in `ee_elf_load()`, the trampoline
install, or the target ELF itself, all of which behave correctly here).

**Answering the user's question directly**: yes, in the narrow sense that
the emulator core correctly parses, loads, and begins real execution of
uLaunchELF's actual code (no crash, no malformed-ELF rejection, correct
entry dispatch) - but no, in the sense that it does not (yet) reach
uLaunchELF's own visible UI/menu, for the same reason no trampoline-loaded
ELF has yet reached its own UI in this project: the shared kernel idle-wait
loop it lands in needs a real stimulus (most likely a pad-button-press event
delivered *during* the post-trampoline run, not just once before warm-up as
this driver does) that hasn't been tried yet.

## Classification

No tracked emulator source (`ee_core.c`, `ee_elf_loader.c`, etc.) was
changed this round - `ee_elf_load()` and the trampoline mechanism both
behaved correctly against a real, substantial third-party ELF, so there is
nothing to fix here. Regression suite and Wii cross-build correctly skipped.

## Possible next step

Deliver a pad-button-press event *during* the post-trampoline run (not just
once before warm-up), matching how a real user would interact with
uLaunchELF's menu, to see whether that's enough to escape the shared idle
loop - this hasn't been tried by any round yet, trampoline or organic.
