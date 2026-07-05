# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

An experimental, from-scratch attempt to see how far a PS2-BIOS-boot path
can get on real Nintendo Wii hardware (devkitPPC + libogc), starting from
an "impossible" ask ("port PCSX2 with a recompiler"). It is **not** a fork
of PCSX2 and does not reuse PCSX2's JIT/recompiler code (that's all x86-64/
AArch64 machine code generation, not portable to PPC750) - only the
**instruction semantics** of PCSX2's interpreter (`R5900OpcodeImpl.cpp`,
`MMI.cpp`, `FPU.cpp`, `COP0.cpp`, `R3000A.cpp`, `R3000AOpcodeTables.cpp`,
`Gif.cpp`, `HwWrite.cpp`/`HwRead.cpp`/`Hw.cpp`, `ps2/Iop/IopHwWrite.cpp`/
`IopHwRead.cpp`, GS register layout) are ported/referenced, which is why
the project is GPL-3.0 licensed as a whole (see `COPYING.GPLv3`).

**Always read `docs/STATUS.md` and `docs/ROADMAP.md` first** in any new
session. `STATUS.md` is the blunt "what actually works" writeup, including
a "First real BIOS boot attempt" section documenting a real, legally-owned
PS2 BIOS diagnostic (see "Real BIOS testing" below - important context for
where the project actually is right now). `ROADMAP.md` is the subsystem-
by-subsystem checklist (EE, IOP, DMA, GIF/VIF, VU0/VU1, GS) with a
"Suggested near-term order" section at the bottom that should be the
default source of "what's next" - don't re-derive priorities from scratch,
they're already reasoned through there. Both files must be kept up to date
as part of any change (see workflow below) - they are the project's actual
memory across sessions, more so than this file.

## Current frontier: the "EE JALR investigation" (rounds 1-7, and counting)

This is the single longest-running thread of work in the project and the
most likely thing an interrupted session needs to pick back up. Full
detail is in `docs/STATUS.md` (search for "EE JALR investigation"); this
is the short version so a fresh session isn't lost. (The "Real BIOS
testing" section below has an older, now-superseded summary of rounds
1-2 written when the root cause was still unknown - this section is the
current, up-to-date status.)

- **Rounds 1-4**: chased an EE interpreter halt where `pc` escaped into
  the hardware-register address window via a bad `JALR` target. Multiple
  false leads ruled out (DMA bugs, a missing SYSCALL handler, a "missed
  guard" theory) with no fabricated fix - each round is preserved in
  `STATUS.md` because the *ruling-out* is as valuable as a fix would be.
- **Round 5 - root cause found**: the user connected a real, working
  PCSX2 via a third-party MCP debugger bridge (`github.com/hkmodd/
  PCSX2-MCP`) and captured a live instruction trace + memory dump proving
  real hardware's boot path is completely different from what this
  project executed. Actual root cause: `ee_core_init()` never set COP0
  register 15 (PRId) - the very first instruction of the real BIOS reads
  it and branches on a CPU-revision check. **Fix**: `cop0[15] =
  0x00002e20` (real value, from PCSX2's `R5900.cpp`), in
  `ee_core_init()`.
- **Round 6 - real COP0 TLB implemented**: past the PRId fix, boot hit a
  new, honest wall on `TLBWI` (completely unimplemented). Implemented
  real `TLBR`/`TLBWI`/`TLBWR`/`TLBP` (48-entry `tlb[]`, ported from
  PCSX2's `COP0.cpp`) plus real KUSEG (`<0x80000000`) address translation
  in `ee_mem_ptr()` via `ee_tlb_translate()`. Also fixed a kseg0-ROM-
  mirror bug, added the MIPS "Branch Likely" family (`BEQL`/`BNEL`/etc),
  and `LWC1`/`SWC1`. New wall: a genuine TLB miss on `$sp=0x70003eb0`
  (no installed TLB entry covers it) - silently read as 0 (no exception
  path yet), wandering into "zero-land".
- **Round 7 - real exception delivery implemented**: `ee_raise_exception()`/
  `ee_raise_tlb_exception()` in `ee_core.c`, ported from PCSX2's
  `cpuException()`/`cpuTlbMiss()` in `R5900.cpp`. Real Cause/EPC/
  Status.EXL/Status.BEV-dependent vectoring, correct `Cause.BD` for
  branch-delay-slot faults (needed real delay-slot tracking, added via
  `branch_pending` - previously an unused, vestigial field). Verified
  as dramatic, real progress: a 20M-instruction run against the actual
  SCPH-10000 BIOS now executes **97.62% real instructions** (was
  0.0008%/151-out-of-20M before). Disassembly confirmed the code being
  executed is genuine MIPS exception-handler prologue (GPR context
  save + EPC/Cause save), not zero-decoded filler.
- **New wall (not yet investigated - this is where to start next)**:
  exactly 2 real exceptions fire in a 50M-instruction run (the original
  TLB miss, then an immediate *nested* fault when the handler's own
  register-save routine touches a different unmapped KUSEG page). After
  that, `Status.EXL` never clears (no `ERET`) and the EE just keeps
  running real code in that same handler-prologue region indefinitely
  without resolving. Best current theory (not confirmed): this looks
  like a missing "wired TLB entry" situation - real MIPS kernels reserve
  a few TLB entries via `COP0.Wired` (`cop0[6]`, not modeled at all
  here) specifically so kernel/handler code and its own scratch memory
  can never TLB-miss while a miss is already being serviced - or the
  real boot path expects more `TLBWI` calls to have executed by this
  point than this project's trace has reached. See `docs/STATUS.md`'s
  "round 7" section and `docs/ROADMAP.md` for the full evidence trail
  (diagnostic harnesses referenced there live in `/tmp/diag/` on the
  machine that did this work - throwaway, not committed, may not exist
  in a fresh environment; the *method* - disassemble the hot pc range,
  trace `Cause`/`EPC`/`BadVAddr` changes, check the real vs. zero-decoded
  instruction ratio over tens of millions of steps - is the reusable
  part, not the specific files).

**Tool note**: `github.com/hkmodd/PCSX2-MCP` (third-party, not this
project's own code) gives live debugging access to a real, user-run
PCSX2 instance - breakpoints, memory dumps, register reads, disassembly
- across EE (R5900) and IOP (R3000) address spaces independently. This
was the breakthrough that unblocked round 5 after rounds 1-4 stalled for
lack of a ground-truth reference. If stuck on a similarly opaque EE/IOP
divergence again, asking the user whether they can run this tool against
real PCSX2 (same BIOS) is a legitimate, previously-proven-useful move -
just remember PCSX2-MCP's own README caveat that breakpoints often don't
trigger until a game (not just the BIOS) is running, and that EE vs. IOP
addresses live in numerically-overlapping-but-unrelated spaces in its
debugger UI.

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
5. Update `docs/ROADMAP.md`'s checklist/notes, `docs/STATUS.md` if the
   change affects the overall capability picture, and `tests/README.md`
   (one paragraph per test file: what it covers, exact build command, and
   any bug it caught).
6. `git add` the specific files for the change (not blindly `-A` - see
   "Real BIOS testing" below for why staged-file hygiene matters in this
   repo specifically) and `git commit` with a detailed message: what was
   added, what real PS2/PCSX2 reference it was cross-checked against, bugs
   found and fixed during development (this project has caught real bugs
   this way almost every single increment - see "Known sharp edges" below),
   and test results.
7. Push to `origin main`.
8. Sync the working tree to wherever the user is viewing it, if applicable
   (excluding `.git` and, critically, excluding any real BIOS `.bin` file -
   see "Real BIOS testing" below).

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
gcc -I../include -I../source -o test_ee tests/test_ee_core.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
./test_ee
```

There are 29 test files as of this writing, covering both CPU cores (EE
integer/MMI/FPU/unaligned-access/COP0-CO-format/LQ-SQ, IOP integer/
unaligned/SYSCALL-exception/InstallExceptionHandlers), every hardware
register model (EE DMA + chain-mode transfer engine, GS registers + local
memory + Wii output blit, GIF packet parsing + SPRITE rasterization,
EE-side and IOP-side SIF mailbox, IOP INTC/DMA/timers, IOP HLE BIOS trap +
module registry), the BIOS ROMDIR loader, and the two-core interleaved
scheduler with a real SIF handshake. The EE side has grown a cluster of
more recent, narrowly-scoped test files worth knowing about by name:
`test_ee_lqsq.c` (128-bit load/store), `test_ee_fpu2.c`/`test_ee_fpu3.c`
(SQRT/RSQRT/MAX/MIN/BC1 branches, then the ACC accumulator family), and
`test_ee_mmi_compare.c`/`test_ee_mmi_sat.c`/`test_ee_mmi_permute.c`/
`test_ee_mmi_pvshift.c` (the compare/max/min/abs, saturated-arithmetic,
permute/interleave, and variable-shift MMI batches, added incrementally
across several sessions), plus the newer COP0/TLB/exception cluster:
`test_ee_cop0_prid.c`, `test_ee_cop0_tlb.c`, and `test_ee_exceptions.c`
(see "Current frontier" above). **Check `tests/README.md` for the exact,
current build command for each test file** rather than guessing the link
line - it is kept in sync with actual dependencies as they change (several
tests need sibling `.c` files linked in because `ee_core.c`/`iop_core.c`
have grown transitive dependencies on the hardware model), and getting it
wrong just produces linker errors (safe to experiment with).

**Single-step, don't run-to-completion, when testing a fault/exception
path against a synthetic program with no real handler installed**: as of
the exception-delivery work (round 7), `ee_core_run()`'s loop has no
instruction-count cap - if a fault vectors into all-zero memory that
never reaches a `BREAK`, it hangs forever. Use `ee_core_step()` a fixed
number of times instead and inspect state directly. See
`tests/test_ee_exceptions.c` and the KUSEG-miss case in
`tests/test_ee_cop0_tlb.c` for the pattern.

## Architecture

The mental model is a small bus of independent hardware-model modules
under `source/hw/` and `source/core/`, wired together through explicit
function calls from each core's `_init()` - there is no central "bus"
abstraction, `ee_mem_read*/write*`/`iop_mem_read*/write*` directly dispatch
to the right module by address range.

- **`source/core/system.c`** - the top-level orchestrator. `system_init()`
  initializes both cores against a shared BIOS image; `system_run_
  interleaved()` alternates single-instruction steps between the EE and
  IOP (currently a naive 1:1 ratio - real hardware is roughly 8:1 EE:IOP
  clock rate, not yet modeled) up to a slice cap, stopping early if either
  core halts. This is what actually makes the two independent CPU cores
  behave like one running system, and what `main.c` calls at boot instead
  of driving either core standalone.
- **`source/core/ee/ee_core.c`** - the R5900 (Emotion Engine) interpreter,
  the current center of gravity of the project. `ee_state_t` holds 32
  128-bit GPRs (`ee_reg128_t { ud0, ud1 }`, low/high 64 bits - MMI needs the
  full 128 bits, plain MIPS III only needs `ud0`), COP0 registers, COP1/FPU
  registers (raw IEEE-754 bit patterns in `fpr[32]` + `fcr31`, plus a
  32-bit `acc` accumulator register used by the MADD/MSUB/MADDA/MSUBA/
  ADDA/SUBA/MULA family), and a pointer to guest RAM. `ee_mem_read8/16/32/
  64` and `ee_mem_write8/16/32/64` are the single chokepoint all
  instruction implementations go through; they route hardware-register-
  address ranges to `dma_mmio_read32/write32` (32-bit MMIO path),
  `gs_mmio_read64/write64` (64-bit path, GS registers are genuinely 64-bit
  on real hardware), or `sif_mmio_read32/write32` (EE-side SIF mailbox)
  before falling through to the RAM/BIOS pointer path. **Never use
  `memcpy` for guest memory access** - see "Known sharp edges" below for
  why this matters here specifically. COP0 support: MFC0/MTC0 (generic
  registers, Status, Config) plus the "CO"-format instructions RFE/ERET/
  EI/DI (dispatched via a 6-bit `funct` field once `rs`'s top bit is set -
  NOT via `rs` itself, matching PCSX2's `tbl_COP0_C0[64]` table), a real
  48-entry TLB (`TLBR`/`TLBWI`/`TLBWR`/`TLBP` + KUSEG address translation
  via `ee_tlb_translate()`), and real exception delivery for KUSEG TLB
  misses (`ee_raise_exception()`/`ee_raise_tlb_exception()` - Cause/EPC/
  Status.EXL/BEV-dependent vectoring, correct Cause.BD for delay-slot
  faults). See "Current frontier" above for the full story and current
  state - this is the most actively-changing part of the codebase. Still
  missing: general/interrupt/SYSCALL exception delivery through this same
  path (SYSCALL still uses its own separate hand-written trap, and RFE/
  ERET/EI/DI still only handle the exception-RETURN side).
  COP1/FPU is essentially complete for single-precision scalar work:
  core arithmetic, SQRT.S/RSQRT.S, MAX.S/MIN.S (bit-level signed-int
  compare trick), CVT.W.S/CVT.S.W, C.EQ/LT/LE.S, BC1F/BC1T, and the full
  ACC accumulator family (ADDA/SUBA/MULA/MADD/MSUB/MADDA/MSUBA.S - MADD.S/
  MSUB.S re-clamp the intermediate product through `fpuDouble()` a SECOND
  time before combining with ACC, MADDA.S/MSUBA.S deliberately don't - a
  real, tested asymmetry, not an inconsistency to "fix"). Still missing:
  BC1FL/BC1TL ("likely" branches - no likely-branch infrastructure exists
  for ANY branch in this project yet) and the FPU exception-cause control
  flags (only the condition flag needed for BC1 is modeled). MMI (SIMD)
  coverage is at ~67 of the roughly 90 real opcodes: the add/sub/logic/
  copy/extend/pack family, MULT1/DIV1 pipe-1 variants, the compare/max/
  min/abs family (PCGT*/PMAX*/PCEQ*/PMIN*/PABS*/PADSBH), MMI0's saturated
  arithmetic and PEXT5/PPAC5 (completing MMI0's sub-table entirely), the
  MMI2/MMI3 permute/interleave family, and PSLLVW/PSRLVW. Still missing:
  QFSRV (needs a not-yet-implemented SA hardware register plus MTSA/
  MTSAB/MTSAH), the remaining MMI2/MMI3 HI/LO-touching arithmetic
  (PMADDW/H, PMSUBW/H, PMULTW/H, PDIVW/PDIVBW, PMULTUW/PDIVUW/PMADDUW -
  PCSX2's own source documents a real "division voodoo" rounding
  correction on some of these worth extra care when eventually ported),
  and PMFHL/PMTHL. See `docs/ROADMAP.md` section 1 for the exact,
  currently-maintained opcode-by-opcode checklist rather than trusting
  this summary to stay perfectly in sync.
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
  pixels" path - see `docs/ROADMAP.md` section 4 for exactly which
  register/primitive/mode combinations are and are not handled.
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
- **`source/hw/sif.c`** - the SIF (Sub-CPU IF) mailbox/flag registers on
  BOTH sides: the EE-side registers (MSCOM/SMCOM/MSFLAG/SMFLAG/CTRL,
  0x1000F200-0x1000F260, wired into `ee_core.c`) and the IOP-side flat
  mirror window (0x1D000000-0x1D0000FF, wired into `iop_core.c`). This is
  what let `system.c`'s interleaved scheduler prove a real two-core
  handshake (`tests/test_system_handshake.c`): the EE writes MSCOM and
  sets an MSFLAG bit, the IOP polls MSFLAG, reads MSCOM, echoes it into
  SMCOM and sets an SMFLAG bit, and the EE polls SMFLAG and reads the
  echoed value back - neither side's poll loop can complete without the
  other side genuinely having run in between. NOT yet modeled: the real
  SIF0/1/2 DMA-based RPC protocol PCSX2/real hardware actually uses
  (`Sif0.cpp`/`Sif1.cpp`) - this is a hand-written toy handshake proving
  the plumbing works, not the real protocol.
- **`source/core/iop/iop_core.c`** - the R3000A (IOP) interpreter. Wired
  into `system.c`'s interleaved scheduler and has its own hardware
  register set beyond SIF: the interrupt controller (`iop_intc.c`,
  I_STAT/I_MASK/I_CTRL - I_STAT is write-0-to-clear, the OPPOSITE polarity
  from GS_CSR/SIF SMFLAG elsewhere in this project; I_CTRL clears itself
  on READ, a one-shot latch), its own DMA controller (`iop_dma.c`, 13
  channels, separate register layout from the EE's `dma.c` - register
  stubs only, no actual transfer execution, but DMA_ICR/ICR2 have real
  write-1-to-clear/master-bit-recompute logic ported from PCSX2), and a
  counter/timer register stub (`iop_timers.c`, T0-T5 - plain latched
  storage, no ticking/gating/target-IRQ behavior modeled at all, the most
  limited of the IOP register models). Also implements the classic PS1/
  PS2 A0/B0/C0 BIOS syscall trap (`iop_hle_bios.c` - intercepts PC
  reaching one of three fixed addresses, logs the call, returns a generic
  default via `$v0`/`$ra` instead of decoding whatever bytes are actually
  loaded there - with ONE specific, real exception: `C0h` function `0x07`
  (`InstallExceptionHandlers`, per the public psx-spx community reference,
  see "Real BIOS testing" below) actually locates the real 16-byte
  exception-vector trampoline inside the loaded BIOS ROM itself (a
  distinctive byte signature, not a hardcoded guess) and installs it for
  real at RAM address 0x80 - every other function number still gets the
  generic default) and a standalone module registry scaffold
  (`iop_hle_modules.c`, not yet wired to any specific trap function
  number). Both HLE pieces are explicitly, deliberately NOT ports of
  PCSX2's real `IopBios.cpp` (~1500 lines) - see that file's own header
  comment and `docs/ROADMAP.md` section 2 for the full scope boundary and
  why. SYSCALL (MIPS I, SPECIAL funct 0x0C) raises a real exception now
  (Cause.ExcCode, EPC, BEV-dependent vector, Status KU/IE-stack shift -
  ported from PCSX2's `psxException()`), and `iop_core_init()` correctly
  sets Status.BEV=1 on reset (matching real hardware/PCSX2's
  `psxReset()` - it used to default to 0 via a plain `memset`, which was
  wrong and silently meant SYSCALL would have vectored to the wrong
  handler even after being implemented). BREAK (funct 0x0D) is a
  deliberate exception to "port real semantics": it's kept as a clean
  HALT (not a real exception, unlike on actual hardware) because it's
  this project's own testing convention, used by every test file - do
  not "fix" this to match real hardware.
- **`source/core/recompiler/ppc_dynarec.c`** - a proof-of-concept dynamic
  PPC codegen path (2 opcodes: ADDIU, OR), demonstrating that runtime
  codegen + icache/dcache invalidation works on Wii hardware. Explicitly
  **not** wired into the main boot path and not a real recompiler - don't
  extend this expecting it to become one without a much larger design
  effort (see `docs/STATUS.md`'s framing of why a real PCSX2-style
  recompiler port isn't realistic here).
- **`source/core/bios_loader.c`** - loads a raw 4MB PS2 BIOS dump and does
  a ROMDIR walk for the ROMVER string, by SCANNING for the universal
  RESET+ROMDIR name signature rather than trusting any fixed file offset
  (see "Real BIOS testing" below - this used to assume a fixed 0x100
  offset, which was simply wrong). No BIOS image is or should ever be
  committed to this repo (copyrighted Sony firmware) - see
  `data/pcsx2/bios/README.txt`.

## Real BIOS testing

The project's user provided a real, legally-dumped PS2 BIOS image
(SCPH-10000, from their own console) for **local testing only**. This is
legitimate - a legally-owned hardware backup, consistent with this
project's existing documented policy - but comes with strict, standing
handling rules that must be preserved in every session, not just the one
that introduced them:

- The BIOS `.bin` file must **never** be committed to git, **never**
  pushed to GitHub, and **never** copied into any folder synced back to
  the user (rsync, file copy, artifact, anything). This is enforced by
  `.gitignore`'s existing `data/pcsx2/bios/*.bin` rule (do not remove or
  narrow it) and must additionally be double-checked manually: run `git
  status --short` after every commit in a session that touches this file
  to confirm no `.bin` is staged, and always pass an explicit
  `--exclude='data/pcsx2/bios/*.bin'` on any `rsync` that syncs the
  working tree elsewhere.
- Diagnostic harnesses that load and run the real file (e.g. a throwaway
  `boot_real_bios.c`-style program) should live outside `tests/` (e.g. in
  a scratch/tmp location, not this repo) - they depend on a file that
  can't be part of the automated, shippable test suite. Any *test* that
  needs ROMDIR-walking coverage should use a synthetic, hand-built,
  non-copyrighted fixture instead (see `tests/test_bios_loader.c`).

Why this matters beyond compliance: testing against the real BIOS is what
actually found the biggest real bugs/gaps in this project so far -
synthetic all-zero test buffers never meaningfully exercised
`bios_loader.c`'s ROMDIR-walking code path at all, so its fixed-offset
assumption (wrong - real dump has ROMDIR at file offset 0x2700, not 0x100)
went uncaught until real data was used. This project's policy has always
been "no fabricated hardware/BIOS-call semantics without a verified
reference" - real-BIOS testing eventually made that concrete rather than
just aspirational: `psx-spx` (https://psx-spx.consoledev.net/kernelbios/),
a long-standing, publicly published PS1/PS2 community technical
reference (distinct from disassembling this project's copyrighted BIOS
binary), was adopted as exactly that citable reference once real testing
showed precisely which BIOS call (`InstallExceptionHandlers`, `C0h`
function `0x07`) needed real behavior to make further progress - see
`docs/STATUS.md`'s "Resolved: InstallExceptionHandlers" section. When
citing psx-spx (or similar public documentation) for a specific
function's behavior, prefer confirming the exact bytes/values against
this project's own loaded ROM where possible (as done there - the real
jump-target immediate was located via a signature scan of the actual
dump, not assumed from the docs' example values) rather than hardcoding
documentation examples verbatim, since real values can vary by BIOS
revision. The same real-BIOS diagnostic
(`system_init()` + `system_run_interleaved()` run against the real image)
subsequently identified the exact two opcodes each core was missing (EE:
COP0 CO-format RFE/ERET/EI/DI; IOP: SYSCALL exception handling) by showing
precisely where real boot code halted - both are now fixed (see
`docs/STATUS.md`'s "First real BIOS boot attempt" section for the full,
current diagnostic numbers). **This is the project's most effective
debugging technique available and should be used again** whenever
practical for finding the next real gap, rather than guessing at what
opcode/register might matter next.

**Important correction from a later session, worth internalizing before
trusting any raw "N instructions executed" diagnostic number again**: an
earlier session reported the EE reaching "53,592,141 instructions" before
halting and framed that as real boot progress into "COP2/LQ-SQ
territory". Implementing LQ/SQ (see `docs/ROADMAP.md` section 1) proved
that framing wrong - the halt point didn't move AT ALL after the fix. A
proper trace (same technique as above) found the EE actually executes
only ~99,262 real instructions before a `JALR` sends it to an out-of-
range address; because a zero-filled word happens to decode as a valid
NOP, the CPU then just marches in a straight line through unmapped memory
for 53+ million fake "instructions" until it reaches live, non-zero
hardware register content that finally halts it for an unrelated reason.
**Update: this was later solved** (see "Current frontier" near the top
of this file for the full, current story) - `RAM[0x100] == 0` was itself
just a symptom of `ee_core_init()` never setting COP0 PRId, found via a
live trace of real, working PCSX2. The initial "EE-side table of tables"
hypothesis against ps2tek genuinely didn't hold (that address really is
the CPU's own Debug exception vector, not a kernel table) - it just
wasn't the whole story; the real fix came from ground-truth tracing, not
further hypothesis-checking against docs. Moral (still true, keep this
lesson regardless of which specific investigation it applied to): treat
a large raw instruction count as a hypothesis to verify (did the halt
point actually move after
the fix that supposedly explained it?), not as evidence on its own - see
`docs/STATUS.md`'s "LQ/SQ implemented" and the EE-JALR-investigation
sections for the full trail.

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
- **BIOS ROMDIR table file offset varies by BIOS revision**: it is NOT at
  a fixed offset - locate it by scanning for the universal RESET (entry 0)
  + ROMDIR (entry 1, self-referential) name signature. Module payloads are
  packed sequentially starting from file offset 0, so the table's own
  position always equals the size of whatever entry precedes it, rounded
  up to 16 bytes - not a constant.
- **JAL is segment-limited**: MIPS J-type (pseudo-direct) jumps compute
  their target from the CURRENT pc's upper 4 bits plus a 26-bit immediate
  - they can only reach addresses within the SAME 256MB segment as the
  JAL instruction itself. A test placing code at a BIOS reset vector
  (segment 0xB) and expecting a near-zero `JAL` target (e.g. 0xA0, segment
  0) to work will compute the wrong address (e.g. 0xB00000A0) - write test
  programs that call near-zero trap addresses into RAM (segment 0)
  instead.
- **COP0 "CO"-format dispatch**: once a COP0 instruction's `rs` field has
  its top bit set (`rs & 0x10`), the real operation (RFE/ERET/EI/DI/etc)
  is selected by the 6-bit `funct` field (`instr & 0x3F`), NOT by `rs`
  itself - matches PCSX2's `tbl_COP0_C0[64]` table. ERET additionally has
  NO branch delay slot, unlike ordinary branches.
- **`halt()`'s early return skips the instruction counter**: every
  `halt()` call in both `ee_core.c` and `iop_core.c` returns before the
  shared epilogue's `instructions_executed++` - so a program that
  terminates via a halting instruction (BREAK, or an unimplemented-opcode
  halt) will report one fewer instruction executed than you might expect
  when writing test assertions. Not a bug; just a convention to know.
- **Reserved low-memory addresses are no longer just "empty" for test
  programs**: the IOP's real BIOS RAM map reserves several fixed, small
  regions in the first 64KB (address 0 "Garbage Area", 0x80 exception
  vector, 0xA0/0xB0/0xC0 function vectors, etc. - see psx-spx). As of
  `InstallExceptionHandlers`, some of these now get REAL writes at
  runtime (0x00 and 0x80, mirrored). A hand-written test program placed
  at one of these addresses can have its own instructions silently
  overwritten by such a feature before it ever executes (this bit
  `tests/test_iop_hle_exception_install.c` once, the same way the
  JAL-segment issue below bit an earlier test) - place new IOP test
  programs at a safely-clear address like `0x1000` unless the test is
  specifically about one of these reserved regions.
- **kseg0/kseg1 both decode to the same physical memory** (segment bits
  only affect caching, not the target) - `ee_mem_ptr()` must mask to the
  physical address (`addr & 0x1FFFFFFF`) *before* deciding ROM-vs-RAM,
  not special-case one virtual range (this project only handled the
  kseg1 ROM mirror at first, silently breaking the kseg0 one).
- **KUSEG (`<0x80000000`) is a genuinely MAPPED segment** - unlike
  kseg0/kseg1, it needs a real TLB entry (`ee_tlb_translate()`); a naive
  `addr & 0x1FFFFFFF` physical mask "works" by accident until real code
  uses an address only a TLB entry (not identity mapping) resolves
  correctly, then silently corrupts everything downstream. If a test
  wants a plain, no-TLB-needed scratch RAM address, use a kseg0 address
  (`0x80000000+`), not a raw KUSEG one (this bit `test_ee_unaligned.c`
  once real TLB translation landed - it had been using a bare KUSEG
  address as a scratch-RAM base).
- **MFC0 sign-extends 32-bit COP0 values into the 64-bit GPR** - a COP0
  register value with bit 31 set (e.g. TLBP's "not found" `Index =
  0x80000000`) reads back as `0xFFFFFFFF80000000`, not the raw
  `0x80000000`, when read via `MFC0`. Easy to get backwards in a test's
  expected value (this project did, twice, in the same test file,
  before fixing both).
- **`ee_mem_ptr()`'s NULL return has two different meanings** - a KUSEG
  TLB miss (exception-worthy, see `mem_tlb_miss`) vs. a kseg0/kseg1
  address with no backing ROM/RAM (architecturally NOT a TLB fault on
  real MIPS, still just reads-as-zero/no-ops). Conflating these would
  either raise spurious exceptions for ordinary unmapped-hardware-
  register reads, or (the original bug) silently swallow a real TLB
  miss that should have vectored to the BIOS's own handler.
- **SWL/SWR always report `TLBL` (load) never `TLBS` (store) on a TLB
  miss** - a known, documented, *not yet fixed* inaccuracy: their
  internal implementation does a read-then-write of the same address
  (to merge partial bytes), and the read is what actually triggers the
  fault first. Real hardware would raise `TLBS` for a faulting partial
  store. Low priority unless/until something depends on distinguishing
  this.
- **C block comments and `ee_mem_read`/`ee_mem_write`-style wildcard
  shorthand don't mix**: writing `ee_mem_read*/ee_mem_write*` inside a
  `/* ... */` comment terminates the comment early at the `*/` in the
  middle, even though it obviously reads as a glob in prose. Bit this
  project during the round-7 exception work; write
  `ee_mem_read* / ee_mem_write*` (space before the second `/`) or spell
  out the function names instead.
- **A single instruction can trigger the same class of fault twice**
  (e.g. SWL/SWR's internal read-then-write of one address) - without a
  per-instruction guard (`exc_raised_this_step`), the second call would
  corrupt the first exception's Cause/EPC bookkeeping. Any new opcode
  that touches memory more than once per instruction should route
  through the existing `ee_mem_read`/`ee_mem_write` chokepoint (which
  already has this guard) rather than reimplementing raw pointer access.

## Reference material

`README.md` names the exact upstream PCSX2 commit/branch used as the
semantic reference (github.com/PCSX2/pcsx2, master, fetched 2026-07-04),
and the exact upstream source (`devkitPro/gamecube-tools`, `devkitPro/
wiiload`) for the vendored files under `tools/`. When porting a new
opcode, register, or packet format, cross-check against real PCSX2 source
rather than reimplementing from a datasheet/memory - this project's
existing code was built that way, and it's why the bugs that do slip
through are narrow (byte-order, addressing, fixed-offset assumptions)
rather than semantic.
