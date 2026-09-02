# Round 508: minimal bootable PS2 test disc

Built to test whether a genuine BIOS disc-browser boot (SYSTEM.CNF + BOOT2 ELF,
mounted as a real disc image) succeeds where this project's own emulator's
disc-browser idle (task #447) has stalled - and to get real-hardware ground
truth from actual PCSX2 + a real BIOS on what OSDSYS's disc-browser actually
does with a minimal, syntactically-valid disc.

## Contents

- `SYSTEM.CNF` - the standard PS2 boot descriptor real BIOSes read from a
  disc's root directory:
  ```
  BOOT2 = cdrom0:\BOOT.ELF;1
  VER = 1.00
  VMODE = NTSC
  ```
- `build_iso.py` - pycdlib script that packages `SYSTEM.CNF` + a `BOOT.ELF`
  (not included here - see below) into a minimal ISO9660 (interchange level 1)
  disc image.
- No `.iso` or `.elf` binary is committed here. `BOOT.ELF` was a copy of the
  Round 507 diagnostic ELF (`tools/round507-diag-elf/diag.elf`, our own
  original code, not Sony IP) - to regenerate, build that ELF first, copy it
  to `BOOT.ELF` in this directory, then run `python3 build_iso.py`. Even
  though our own diagnostic ELF contains no Sony IP, both it and the built
  `.iso` are kept out of the tracked repo as an extra precaution, matching
  the Round 507 pattern for binary build artifacts.

## Real-hardware result (this round, live PCSX2, JP BIOS scph10000/0100JC200)

1. Mounted via `System > Start File...` pointed at the built ISO. PCSX2's
   window title changed to `BOOT.ELF [?]`, confirming the BIOS/PCSX2 parsed
   `SYSTEM.CNF`'s `BOOT2` line and identified our ELF as the disc's boot
   target (the `[?]` is PCSX2's "unrecognized game database entry" marker -
   expected, since this is a synthetic homebrew disc with no real game
   serial).
2. From OSDSYS's root menu (ブラウザ / システム設定), entering ブラウザ
   (Browser) shows a 3-item carousel cycled with D-pad Left/Right:
   `MEMORY CARD / 1`, `MEMORY CARD / 2`, `PlayStation2 ディスク`.
3. Our disc is correctly, specifically type-detected as
   **`PlayStation2 ディスク`** ("PlayStation 2 Disc") - a genuine,
   disc-type-specific label, not a generic placeholder or error state. This
   confirms our minimal SYSTEM.CNF + ISO9660 structure is sufficient for
   real BIOS-level disc-type identification.
4. Pressing 決定 (Circle) on the `PlayStation2 ディスク` card does **not**
   launch/boot it. Instead the browser silently wraps back to
   `MEMORY CARD / 1` (the first carousel entry) - no loading screen, no
   error message, no black-screen transition, no BOOT2 dispatch observed.
   Reproduced twice from a fresh approach to the disc card, same result both
   times.

This is a direct, real-hardware repro of the user's live observation:
*"the disc shows up in the ps2 menu but it wont load."*

## Interpretation

The real BIOS's disc-browser enumeration/type-detection path and its actual
BOOT2-dispatch path are evidently two separate mechanisms, and our minimal
disc satisfies the first but not the second. Root cause is not isolated at
the code level this round (would need Pine/debugger CDVD register tracing or
BIOS IOP-side disassembly to pin down exactly what additional validation
OSDSYS's browser performs before dispatching BOOT2 - e.g. a full ELF-header
sanity check beyond our stripped Round 507 ELF, ISO9660 level 2/3 structure,
or exact SYSTEM.CNF byte-format expectations). See `docs/STATUS.md` Round 508
entry for the full writeup and how this bears on task #447.
