# Project status - read this first

This document is deliberately blunt. The goal (from the original ask)
was "port PCSX2 to the Wii with a recompiler, just to boot the PS2
BIOS." That goal, as stated, is not achievable as a port - here's why,
and what this repo actually contains instead.

## Why "porting PCSX2" isn't the right framing

PCSX2 (the real, upstream project) is ~500k+ lines of C++ built around
recompilers (dynarecs) for the Emotion Engine, IOP, and both Vector
Units. Those recompilers emit **x86-64 machine code directly** (with a
newer AArch64 backend for Apple Silicon/ARM builds). There is no
PowerPC code generation backend, and none of PCSX2's JIT infrastructure
(register allocation, its internal x86 assembler, block linking, SIMD
codegen for VU math) is architecture-portable - it would all need to be
rewritten for PPC750, from scratch. That's a multi-year effort for a
paid team, not a fork-and-recompile job.

## Why the Wii is the wrong target even if you did rewrite it

- **CPU**: Wii's Broadway is a single-core PowerPC 750 derivative at
  ~729 MHz with no SIMD beyond simple paired-singles. The PS2 Emotion
  Engine runs its main core at 294 MHz but leans heavily on two
  dedicated Vector Units (VU0/VU1) doing 128-bit SIMD math in parallel
  with the main core - the Wii has nothing equivalent, so that work
  would have to run serially on the one PPC core that's also supposed
  to be emulating everything else.
- **GPU**: The PS2 Graphics Synthesizer is a fixed-function but very
  high-bandwidth rasterizer with 4MB of eDRAM and unusual blending/
  texture semantics that PC PCSX2 emulates either via HLE tricks or by
  brute-force GPU shader translation on hardware far more capable than
  Wii's Hollywood GPU (which is itself a fixed-function part derived
  from GameCube's Flipper, with no programmable shaders at all).
- **Memory**: PS2 has 32MB of main RAM; the Wii has 24MB (MEM1, fast)
  + 64MB (MEM2, slower GDDR3). Workable in principle, but leaves very
  little headroom for framebuffers, translation caches, and BIOS/IOP
  RAM shadowing once you actually start emulating the GS.

None of this means "impossible in principle" - it means the effort and
hardware headroom required are far beyond a hobby project, and nobody
in the homebrew scene has shipped this for exactly these reasons.

## What actually exists in this repo right now

| Component | Status |
|---|---|
| devkitPPC/libogc toolchain build | Working - compiles and links a `.dol` |
| Wii video/console bring-up | Working (real libogc init code) |
| SD/USB mount (libfat) | Working |
| BIOS file loader + ROMDIR/ROMVER parse | Working, best-effort |
| EE interpreter | Covers most of the MIPS III integer core (~50 opcodes: ALU imm+reg, shifts incl. 64-bit, MULT/DIV, HI/LO, branches incl. REGIMM, J/JAL/JR/JALR, byte/half/word/double load+store), semantics ported from PCSX2's `R5900OpcodeImpl.cpp`. Still halts on MMI/FPU/COP2/LWL-SWR/LQ-SQ/syscalls/exceptions - real BIOS code uses these within the first few hundred to few thousand instructions, so still not a full boot |
| IOP core | Not started |
| VU0/VU1 | Not started |
| Graphics Synthesizer | Not started |
| PPC recompiler | Proof-of-concept only: 2 opcodes, no branches inside blocks, not wired into the boot path by default |

## What changed in this pass

- Replaced the placeholder `elf2dol.py` output path with the real
  upstream devkitPro `elf2dol.c` (vendored from `devkitPro/gamecube-tools`),
  compiled natively on demand.
- Pulled in PCSX2's actual upstream source (github.com/PCSX2/pcsx2) as
  a reference and ported real instruction semantics into
  `ee_core.c`, replacing the earlier ad-hoc ~12-opcode toy interpreter
  with a ~50-opcode one matching PCSX2's own interpreter behavior.
  This makes the project GPL-3.0 (see LICENSE section in README).
- This is still an interpreter, not a recompiler, and still does not
  boot a real BIOS to completion - see the coverage table above.

## Realistic next steps, if you want to keep going

1. Expand the EE interpreter opcode coverage (this is the highest
   value, lowest risk next step - straightforward, well-documented
   MIPS III + a `docs/ee_opcodes_pcsx2_reference.md` cross-check
   against real PCSX2 source would help).
2. Decide early whether to target **interpreter-only** as the
   realistic end state (much more achievable) rather than a
   recompiler - a good interpreter that boots the BIOS to the OSD menu
   is already a substantial, demonstrable achievement on this hardware.
3. Treat the GS as out of scope for "boot the BIOS" - the BIOS splash
   screen alone will require at least a minimal GS-to-Wii-GX
   translation layer, which is its own project.
