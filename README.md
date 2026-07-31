# PCSX2-Wii (experimental)

A from-scratch, work-in-progress skeleton exploring what it would take to
boot a PS2 BIOS on a Nintendo Wii, built with devkitPPC + libogc.

Upstream reference: [github.com/PCSX2/pcsx2](https://github.com/PCSX2/pcsx2) (master branch, fetched 2026-07-04) - used as the semantic reference for `ee_core.c`'s instruction implementations.

**Read [docs/STATUS.md](docs/STATUS.md) before opening issues**, and see [docs/ROADMAP.md](docs/ROADMAP.md) for the full subsystem-by-subsystem task breakdown (EE, IOP, DMA, GIF/VIF, VU0/VU1, GS) with what's done vs. open. This is a
fun/research project, not a usable PS2 emulator, and it will not become one
without a lot more work than a solo project can realistically deliver. See
that document for exactly what works, what doesn't, and why.

## What's here

- `source/main.c` - Wii bring-up (video/console init via libogc), mounts
  SD/USB via libfat, loads a BIOS image, hands off to the EE core.
- `source/core/bios_loader.c` - loads a raw 4MB PS2 BIOS dump, does a
  best-effort ROMDIR walk to read the ROMVER string.
- `source/core/ee/ee_core.c` - an R5900 (Emotion Engine) interpreter:
  full MIPS III integer core (ALU imm + reg-reg, shifts incl. 64-bit
  D-variants, MULT/DIV, HI/LO moves, branches incl. REGIMM, jumps incl.
  link register, byte/half/word/double loads and stores), basic COP0
  (MFC0/MTC0), CACHE/SYNC/PREF as no-ops, and ~35 of the ~90 MMI (SIMD)
  opcodes (add/sub/logic/copy/extend/pack across byte/half/word lanes,
  plus the MULT1/DIV1/MFHI1/MFLO1 "pipe 1" variants). Instruction
  semantics are ported from PCSX2's own `R5900OpcodeImpl.cpp` and
  `MMI.cpp` (not reinvented), so behavior matches real PCSX2 for the
  opcodes covered - see `tests/test_ee_core.c` for a host-native unit
  test. Still no FPU, no VU0/COP2, no LWL/LWR/SWL/SWR, no LQ/SQ, no
  MMU/exceptions, and ~55 MMI opcodes remain unimplemented. Halts
  cleanly on the first unimplemented opcode and prints where it
  stopped - see `docs/STATUS.md` for the full coverage table.
- `source/core/recompiler/ppc_dynarec.c` - an experimental proof of
  concept that translates straight-line ADDIU/OR sequences into native
  PPC machine code at runtime (with proper icache/dcache handling via
  libogc). It is not wired into the main boot path by default and
  covers two opcodes, as a demonstration that dynamic codegen works on
  Wii hardware - not as a real recompiler.

## Getting the real devkitPro base_tools

The devkitPPC tarball some of us started from had an empty `base_tools`
directory (no `elf2dol`, `wiiload`, `gxtexconv`, ...). Two ways to get
the genuine tools:

1. **Recommended - devkitPro pacman** (gets everything, kept up to date):
   install the devkitPro pacman package manager from
   https://github.com/devkitPro/pacman/releases, then:
   ```sh
   sudo dkp-pacman -S wii-dev gamecube-tools general-tools
   ```
2. **Vendored sources** (what this repo does automatically, or you
   can build yourself): the sources for the specific tools this
   project actually uses are vendored here under their original
   licenses, and built natively (host gcc, not the PPC cross
   compiler):
   - `tools/elf2dol.c` (from `devkitPro/gamecube-tools`) - compiled
     on demand by the Makefile if no system-wide `elf2dol` is on
     `PATH`. `tools/elf2dol.py` is a last-resort pure-Python
     reimplementation if neither is available.
   - `tools/wiiload/` (from `devkitPro/wiiload`) - pushes a `.dol` to
     a running Homebrew Channel over network. Not wired into the
     Makefile automatically (needs `zlib`), build it yourself:
     `gcc -o wiiload tools/wiiload/wiiload.c tools/wiiload/gecko.c -lz`,
     then `WIILOAD=tcp:<your wii's IP>` + running `wiiload pcsx2-wii.dol`
     (or `make run`, which already assumes `wiiload` is on `PATH`).
   - `gxtexconv` (TPL texture conversion) isn't vendored - it doesn't
     appear to have its own public repo under `devkitPro/`, and this
     project doesn't produce any `.tpl` texture assets, so it isn't
     actually needed here. Get it via `dkp-pacman` if some other
     project needs it.

## Building

Requires devkitPPC (r32 tested) and libogc 1.8.18 installed under a
`DEVKITPRO` tree:

```sh
export DEVKITPRO=/path/to/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
export PATH=$DEVKITPPC/bin:$PATH
make
```

Produces `pcsx2-wii.dol`, loadable via HBC (Homebrew Channel) or
Dolphin.

## Running

Put a **legally-dumped** PS2 BIOS you own at `sd:/pcsx2/bios/bios.bin`
(or `SCPH39001.bin`) on an SD card, then launch the `.dol`. No BIOS
image is included or distributed in this repository - PS2 BIOS ROMs
are copyrighted Sony firmware.

## License

**GPL-3.0** (see `COPYING.GPLv3`). `source/core/ee/ee_core.c`'s
instruction semantics are ported from PCSX2 (github.com/PCSX2/pcsx2,
GPL-3.0), so the project as a whole is licensed under GPL-3.0 to match
- this is a legal requirement of using GPL'd reference code, not a
  stylistic choice. `tools/elf2dol.c` is vendored from
  `devkitPro/gamecube-tools` under its own license (see file header).
  devkitPPC/libogc are separately licensed by their respective projects
  (BSD-style, see `libogc_license.txt` if you vendor libogc into this
  tree).
