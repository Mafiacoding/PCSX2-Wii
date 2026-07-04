# PCSX2-Wii (experimental)

A from-scratch, work-in-progress skeleton exploring what it would take to
boot a PS2 BIOS on a Nintendo Wii, built with devkitPPC + libogc.

**Read [docs/STATUS.md](docs/STATUS.md) before opening issues.** This is a
fun/research project, not a usable PS2 emulator, and it will not become one
without a lot more work than a solo project can realistically deliver. See
that document for exactly what works, what doesn't, and why.

## What's here

- `source/main.c` - Wii bring-up (video/console init via libogc), mounts
  SD/USB via libfat, loads a BIOS image, hands off to the EE core.
- `source/core/bios_loader.c` - loads a raw 4MB PS2 BIOS dump, does a
  best-effort ROMDIR walk to read the ROMVER string.
- `source/core/ee/ee_core.c` - a minimal R5900 (Emotion Engine)
  interpreter covering a small MIPS III subset (ADDIU, ORI, LUI, OR,
  AND, ADDU, SLL, LW, SW, BEQ, BNE, J, JR). No MMI, no FPU, no VU0, no
  MMU/exceptions. It will halt cleanly on the first unimplemented
  opcode it meets and print where it stopped.
- `source/core/recompiler/ppc_dynarec.c` - an experimental proof of
  concept that translates straight-line ADDIU/OR sequences into native
  PPC machine code at runtime (with proper icache/dcache handling via
  libogc). It is not wired into the main boot path by default and
  covers two opcodes, as a demonstration that dynamic codegen works on
  Wii hardware - not as a real recompiler.

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

Project code: MIT (see individual file headers). devkitPPC/libogc are
separately licensed by their respective projects (BSD-style, see
`libogc_license.txt` if you vendor libogc into this tree).
