# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

An experimental, from-scratch attempt to see how far a PS2-BIOS-boot path
can get on real Nintendo Wii hardware (devkitPPC + libogc), starting from
an "impossible" ask ("port PCSX2 with a recompiler"). It is **not** a fork
of PCSX2 and does not reuse PCSX2's JIT/recompiler code (that's all x86-64/
AArch64 machine code generation, not portable to PPC750) - only the
**instruction semantics** of PCSX2's interpreter (`R5900OpcodeImpl.cpp`,
`MMI.cpp`, `FPU.cpp`, `Gif.cpp`, GS register layout) are ported/referenced,
which is why the project is GPL-3.0 licensed as a whole (see
`COPYING.GPLv3`).

**Always read `docs/STATUS.md` and `docs/ROADMAP.md` first** in any new
session. `STATUS.md` is the blunt "what actually works" writeup;
`ROADMAP.md` is the subsystem-by-subsystem checklist (EE, IOP, DMA,
GIF/VIF, VU0/VU1, GS) with a "Suggested near-term order" section at the
bottom that should be the default source of "what's next" - don't
re-derive priorities from scratch, they're already reasoned through there.
Both files must be kept up to date as part of any change (see workflow
below) - they are the project's actual memory across sessions, more so
than this file.

## The mandatory per-change workflow

This project has a strict, consistently-applied ritual for every increment
of work. Do not skip steps, even for small changes:

1. Implement the change.
2. Write (or extend) a **host-native** unit test in `tests/` that exercises
   the real code path - not a reimplementation/formula re-derivation of the
   logic being tested. See "Testing" below for how these are built and run.
3. Fix any bugs the test surfaces.
4. Rebuild the actual Wii/devkitPPC target (`make`) to confirm the
   cross-compiled build still compiles cleanly - host tests passing is not
   sufficient on its own, both must be verified.
5. Update `docs/ROADMAP.md`'s checklist/notes and `tests/README.md` (one
   paragraph per test file: what it covers, exact build command, and any
   bug it caught).
6. `git add -A && git commit` with a detailed message: what was added, what
   real PS2/PCSX2 reference it was cross-checked against, bugs found and
   fixed during development (this project has caught real bugs this way
   almost every single increment - see "Known sharp edges" below), and test
   results.
7. Push to `origin main`.
8. Sync the working tree to wherever the user is viewing it, if applicable.

## Building

Requires devkitPPC (r32 tested) and libogc 1.8.18 under a `DEVKITPRO` tree:

```sh
export DEVKITPRO=/path/to/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
export PATH=$DEVKITPPC/bin:$PATH
make
```

Produces `pcsx2-wii.dol` (via the DOL conversion step below) and
`pcsx2-wii.elf`. `make clean` removes `build/`, the `.elf`, and the `.dol`.
`make run` invokes `wiiload $(TARGET).dol` (needs `wiiload` on `PATH` and
`WIILOAD=tcp:<wii ip>` set, or a running Homebrew Channel).

The `.elf` -> `.dol` step (in the Makefile's `$(OUTPUT).dol` rule) tries,
in order: a system `elf2dol` on `PATH`, then compiles the vendored
`tools/elf2dol.c` natively via host `cc` into `tools/elf2dol_native`, then
falls back to `tools/elf2dol.py`. This exists because some devkitPPC
tarballs ship with an empty `base_tools/` directory - see the README's
"Getting the real devkitPro base_tools" section if `elf2dol`/`wiiload` are
missing entirely.

If devkitPPC's `cc1` fails with a missing `libmpfr.so.4` (built against an
older glibc/mpfr than your host), symlink your host's `libmpfr.so.6` to a
`libmpfr.so.4` name in a scratch directory and put that directory first in
`LD_LIBRARY_PATH` for the `make` invocation - this is an environment quirk,
not a project bug.

## Testing

Every `tests/test_*.c` file is a **host-native** test: compiled with the
regular host `gcc` (not devkitPPC), by `#include`-ing the `.c` file under
test directly into the test file. This is deliberate - it allows fast
iteration on interpreter/hardware-model correctness without needing real
Wii hardware or Dolphin for every check. None of these are wired into the
Wii Makefile; run them individually, e.g.:

```sh
gcc -I../include -I../source -o test_ee tests/test_ee_core.c
./test_ee
```

Several test files need sibling `.c` files linked in explicitly because
`ee_core.c` has grown transitive dependencies on the hardware model
(`dma.c`, `gs.c`, `gif.c`, `gs_mem.c` - since `ee_core_init()` wires up DMA,
GS registers, and the GIF sink). **Check `tests/README.md` for the exact,
current command for each test file** rather than guessing the link line -
it is kept in sync with actual dependencies as they change, and getting it
wrong just produces linker errors (safe to experiment with).

## Architecture

The mental model is a small bus of independent hardware-model modules
under `source/hw/` and `source/core/`, wired together through explicit
function calls from `ee_core_init()` (in `source/core/ee/ee_core.c`) - there
is no central "bus" abstraction, `ee_mem_read*/write*` directly dispatches
to the right module by address range.

- **`source/core/ee/ee_core.c`** - the R5900 (Emotion Engine) interpreter,
  the current center of gravity of the project. `ee_state_t` holds 32
  128-bit GPRs (`ee_reg128_t { ud0, ud1 }`, low/high 64 bits - MMI needs the
  full 128 bits, plain MIPS III only needs `ud0`), COP0 registers, COP1/FPU
  registers (raw IEEE-754 bit patterns in `fpr[32]` + `fcr31`), and a
  pointer to guest RAM. `ee_mem_read8/16/32/64` and `ee_mem_write8/16/32/64`
  are the single chokepoint all instruction implementations go through;
  they route hardware-register-address ranges to `dma_mmio_read32/write32`
  (32-bit MMIO path) or `gs_mmio_read64/write64` (64-bit path, GS registers
  are genuinely 64-bit on real hardware) before falling through to the
  RAM/BIOS pointer path. **Never use `memcpy` for guest memory access** -
  see "Known sharp edges" below for why this matters here specifically.
- **`source/hw/dma.c`** - the EE DMA controller, 10 channels
  (`DMA_CHANNEL_VIF0..TOSPR`). `dma_channel_kick()` implements both NORMAL
  mode (one-shot MADR+QWC transfer) and CHAIN mode (walks DMA tags via
  `read_chain_tag()`, dispatching `REFE/CNT/NEXT/END` tag types; `REF/
  REFS/CALL/RET` are recognized but set an "unsupported" error and stop
  cleanly rather than misbehaving). Writing CHCR with the STR (start) bit
  set auto-triggers a kick, matching real hardware. `dma_set_sink(channel,
  fn)` registers a callback invoked with each transferred chunk of
  quadwords - this is the hook other subsystems (currently just GIF) use
  to actually consume DMA'd data, rather than DMA writing into a generic
  memory buffer.
- **`source/hw/gif.c`** - parses PACKED-mode GIF packets (GIFtag + PRIM/
  RGBAQ/XYZ2/A+D register writes) delivered via `dma_set_sink
  (DMA_CHANNEL_GIF, gif_process_quadwords)`, and rasterizes SPRITE
  primitives (filled axis-aligned rectangles only, for now) directly into
  GS memory. This is the current "how does data actually turn into
  pixels" path, and the newest, least hardened piece of the project - see
  `docs/ROADMAP.md` section 4 for exactly which register/primitive/mode
  combinations are and are not handled.
- **`source/hw/gs.c` / `gs_mem.c` / `gs_wii_output.c`** - three separate
  concerns that together stand in for the real GS: `gs.c` is just the
  privileged register block (PMODE/DISPFB/CSR/IMR/etc, with GS_CSR's
  write-1-to-clear semantics vs. GS_IMR's plain read/write being the one
  subtlety worth remembering here); `gs_mem.c` is a deliberately
  **simplified linear** (not real block-swizzled) model of the 4MB eDRAM,
  PSMCT32 only; `gs_wii_output.c` converts a rectangular PSMCT32 region to
  the Wii's packed Y1CbY2Cr XFB format (RGB->YCbCr port of libogc's
  `console.c`) and blits it into the real framebuffer - direct pixel blit,
  no GX 3D pipeline involved.
- **`source/core/iop/iop_core.c`** - a standalone R3000A (IOP) interpreter,
  architecturally isolated from the EE core right now (separate struct,
  separate 2MB RAM, own copy of the LWL/LWR/etc pattern). **Not yet wired
  to SIF or the EE core** - this is the next major integration point per
  the roadmap's "Suggested near-term order".
- **`source/core/recompiler/ppc_dynarec.c`** - a proof-of-concept dynamic
  PPC codegen path (2 opcodes: ADDIU, OR), demonstrating that runtime
  codegen + icache/dcache invalidation works on Wii hardware. Explicitly
  **not** wired into the main boot path and not a real recompiler - don't
  extend this expecting it to become one without a much larger design
  effort (see `docs/STATUS.md`'s framing of why a real PCSX2-style
  recompiler port isn't realistic here).
- **`source/core/bios_loader.c`** - loads a raw 4MB PS2 BIOS dump and does
  a best-effort ROMDIR walk for the ROMVER string. No BIOS image is or
  should ever be committed to this repo (copyrighted Sony firmware) - see
  `data/pcsx2/bios/README.txt`.

## Known sharp edges (read before touching related code)

These are bugs the project's own test-first workflow has already caught
once - the fixes are in place, but the underlying subtlety can bite again
if new code re-introduces the naive version:

- **Endianness**: PS2 (EE/IOP) is little-endian; the Wii/PowerPC 750 build
  target is big-endian. Any new guest-memory-adjacent code (new MMIO
  range, new loader, new packet parser) must compose/decompose multi-byte
  values explicitly byte-by-byte - never `memcpy` a multi-byte guest value
  directly into/out of a host struct or variable.
- **LWL/LWR/SWL/SWR addressing**: the "R" variant (LWR/SWR) uses the
  `start` address, the "L" variant (LWL/SWL) uses `start+3` - this reads
  backwards from what you'd guess, and getting it wrong produces
  plausible-but-wrong results with no crash. See `tests/README.md`'s
  `test_ee_unaligned.c` entry.
- **DMA channel address decoding**: several channel pairs (fromSPR/toSPR,
  fromIPU/toIPU, SIF0/1/2) pack two channels into one 0x1000 region as
  0x400-byte sub-blocks - a fixed-mask decode aliases them together. The
  current explicit `(base, size, channel)` range table in `dma.c` is the
  fix; don't go back to address masking.
- **GIF A+D register format**: address+data writes are DATA in words 0-1,
  the 8-bit register ADDRESS in word 2's low byte - easy to get backwards
  (this project did, once, before any test caught it).

## Reference material

`README.md` names the exact upstream PCSX2 commit/branch used as the
semantic reference (github.com/PCSX2/pcsx2, master, fetched 2026-07-04),
and the exact upstream source (`devkitPro/gamecube-tools`, `devkitPro/
wiiload`) for the vendored files under `tools/`. When porting a new
opcode, register, or packet format, cross-check against real PCSX2 source
rather than reimplementing from a datasheet/memory - this project's
existing code was built that way, and it's why the bugs that do slip
through are narrow (byte-order, addressing) rather than semantic.
