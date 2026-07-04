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
| BIOS file loader + ROMDIR/ROMVER parse | Working - fixed a real bug (fixed 0x100 offset assumption was wrong; now scans for the RESET+ROMDIR signature) after testing against a real, legally-dumped BIOS - see "First real BIOS boot attempt" below |
| EE interpreter | Full MIPS III integer core (ALU, shifts, MULT/DIV, HI/LO, branches, jumps, load/store) + basic COP0 (MFC0/MTC0) + CACHE/SYNC/PREF no-ops + ~35 of ~90 MMI (SIMD) opcodes: add/sub/logic/copy/extend/pack across byte/half/word lanes, plus MULT1/DIV1/MFHI1/MFLO1 pipe-1 variants. Semantics ported from PCSX2's `R5900OpcodeImpl.cpp`/`MMI.cpp`, unit-tested in `tests/test_ee_core.c`. Still halts on the other ~55 MMI opcodes (saturated arithmetic, compares, QFSRV, PMADD family), FPU, COP2/VU0, unaligned load/store, TLB/exceptions/syscalls |
| IOP core | Not started |
| VU0/VU1 | Not started |
| Graphics Synthesizer | Not started |
| PPC recompiler | Proof-of-concept only: 2 opcodes, no branches inside blocks, not wired into the boot path by default |

## First real BIOS boot attempt

This project's user provided a real, legally-dumped PS2 BIOS image
(SCPH-10000, from their own console) for local testing only - it is
NOT included in this repository (see `data/pcsx2/bios/README.txt`
and `.gitignore`). This let us move from "does our interpreter run
hand-written test programs correctly" to "what actually happens when
it's given a real BIOS" for the first time.

Two concrete, valuable results:

1. **A real bug, found and fixed**: `bios_loader.c` assumed the
   ROMDIR table always lives at file offset 0x100. Wrong - this real
   dump has it at 0x2700. Fixed by scanning for the universal
   RESET+ROMDIR name signature instead of trusting a fixed offset
   (see `tests/test_bios_loader.c`, which reproduces the bug/fix with
   a synthetic, non-copyrighted fixture). ROMVER now parses correctly
   or this BIOS as `0100JC20000117`.

2. **Running EE+IOP together against the real BIOS via
   `system_run_interleaved()` reaches real, informative halt points**
   (not committed as an automated test, since it needs the real BIOS
   file - this was a one-off diagnostic run):
   - The **EE** executes 99,158 real instructions from the actual
     BIOS ROM before halting on an unimplemented COP0 sub-opcode
     (BC0/TLB/ERET - MMU/exception-related instructions this
     project's EE core doesn't implement yet). This means real BIOS
     boot code gets meaningfully far into MMU/exception setup before
     hitting a gap.
   - The **IOP** executes over 3 million real instructions and, along
     the way, makes **27 real calls through this project's A0/B0/C0
     HLE trap** (`source/hw/iop_hle_bios.c`) - concrete proof that
     mechanism is exercised by actual BIOS code, not just
     hand-written test programs. It eventually halts on a raw
     `SYSCALL` MIPS instruction (not the A0/B0/C0 jump convention -
     a different, lower-level kernel-entry mechanism this project's
     IOP core doesn't implement at all yet).

**What this means for "next steps"**: these are now the two most
concretely-justified next targets, since they're not speculative -
they're the exact two places real BIOS execution actually stops.
Implementing IOP SYSCALL handling (even a minimal version) and EE
COP0's BC0/TLB-adjacent opcodes (at least enough to not halt - a full
MMU is a much bigger undertaking, see docs/ROADMAP.md) would let a
real boot attempt get further than any hand-written test program
alone could ever prove.

### Update: both concrete blockers above addressed

Both gaps identified above were real, and both are now fixed - ported
directly from real PCSX2 source, not guessed:

- **EE**: the halt wasn't actually "BC0/TLB unimplemented" in the way
  it first looked - it was the COP0 "CO"-format instructions RFE,
  ERET, EI, and DI, which are dispatched via a 6-bit `funct` field
  (not the `rs` field) once `rs`'s top bit is set. Added, matching
  PCSX2's `tbl_COP0_C0[64]` table and `COP0.cpp`'s ERET/EI/DI
  semantics (see `tests/test_ee_cop0_special.c`, 9/9 checks, and the
  COP0 case in `ee_core.c` for the full reference notes). Real TLB
  instructions (TLBWI/TLBWR/TLBP/TLBR) and actual exception-vector
  raising for the EE are still not implemented - the CO-format gap
  was simply the first thing real BIOS code hit.
- **IOP**: SYSCALL (SPECIAL funct 0x0C) now raises a real exception -
  sets Cause.ExcCode, sets EPC, vectors to the bootstrap
  (`0xBFC00180`) or normal (`0x80000080`) handler depending on
  Status.BEV, and shifts the KU/IE stack - ported from PCSX2's
  `psxException()` in `R3000A.cpp` (see `tests/test_iop_syscall.c`,
  5/5 checks). Also fixed `iop_core_init()`, which incorrectly left
  Status.BEV at 0 on reset via a plain `memset` - real hardware/PCSX2
  set it to 1, which matters because it's what selects the bootstrap
  vector SYSCALL just started actually using.

Re-running the same real-BIOS diagnostic after these fixes:

- The **EE** now runs the full 5,000,000-instruction test slice
  without halting at all - up from 99,158. A 50x+ improvement, and no
  new gap was hit within that slice; the real limit here is just how
  long the diagnostic was allowed to run, not a code halt.
- The **IOP** progressed from halting at 3,054,721 instructions
  (raw SYSCALL) to 3,054,763 - 42 more real instructions executed
  inside the exception handler this time - before hitting a new halt:
  an unimplemented SPECIAL `funct 0x3F` at `pc=0x00000068`. Notably,
  this new halt address is a low RAM address, not near either
  exception vector - suggesting the bootstrap handler dispatches
  execution down into a RAM-resident routine after the SYSCALL, which
  is itself a small, useful data point about what the real IOP
  kernel's exception path actually does. Whether `funct 0x3F` is a
  genuine missing opcode or a symptom of drift from an earlier,
  different bug hasn't been investigated yet - flagged as the next
  concrete IOP target in `docs/ROADMAP.md`.

Both fixes are committed with host-native regression tests
(`tests/test_ee_cop0_special.c`, `tests/test_iop_syscall.c`) alongside
the full existing test suite (21 test files total), all passing, and
the Wii/devkitPPC target rebuilding clean with no warnings.

### Investigating the new IOP halt: not a missing opcode

Dug into the new "unimplemented SPECIAL funct 0x3F at pc=0x00000068"
halt with targeted diagnostics (instruction-trace ring buffer, full
low-memory store logging, and a log of every real A0/B0/C0 HLE BIOS
call made). Conclusion: **this is not a missing MIPS instruction** -
funct 0x3F isn't a real R3000A opcode at all, so "implementing" it
would mean fabricating hardware behavior that doesn't exist. It's a
downstream symptom of something else:

- The real IOP kernel's general exception dispatcher is hardware-
  mandated to live at RAM address 0x80000080 (physical 0x80) - any
  exception (including our now-working SYSCALL) jumps straight there.
  Tracing shows this address, and the ~36 bytes after it, are all
  zero in our emulated RAM - no real dispatcher code ever landed
  there. A short trampoline exists a bit further along at 0x800000A4
  (loads an offset into `$t0` and `JR`s to it), which is itself real
  code that DID get written correctly - so *something* installed
  partway, but not the actual dispatcher stub at the hardware-fixed
  entry point.
- Following that trampoline's target lands in a region (address
  ~0x5D1 and eventually 0x68) that's mostly zero except for a few
  stray non-zero words. One of those, at address 0x000068, is the
  exact byte pattern `0x000000FF` - traced back to a single, explicit
  `SW` instruction executed from ROM (`pc=0xBFC4B860`) very early in
  boot (instruction #20226, long before any HLE BIOS call happens).
  This has the signature of a sentinel/placeholder value (an "empty
  slot" marker in some table or control-block array) that real boot
  code intends to overwrite with real content *later* - and that
  later step evidently never happens in this project's emulation, so
  execution eventually wanders onto the raw sentinel bytes and decodes
  them as a (bogus) instruction.
- Ruled out this project's IOP DMA register-stub limitation (no real
  transfer execution) as the direct cause: no CHCR write with the
  STR/kick bit targeting this address range was observed anywhere in
  the run.
- The chain of 27 real A0/B0/C0 HLE BIOS calls made during the run
  (logged via `iop_hle_bios_get_state()`) was extracted in full; the
  very last one, immediately before the SYSCALL that leads into this
  broken vector, is an A0-table call to function `0x72`. This project
  has no verified, citable reference for what real PS1/PS2 BIOS
  function numbers actually do (see `source/hw/iop_hle_bios.c`'s
  scope notes) - every call, including this one, gets a generic
  default return value of 0 - and it's plausible (though not proven)
  that the real function 0x72 returns something the surrounding code
  branches on before deciding to install the real exception-dispatcher
  code, which a wrong/default return value could cause to be skipped.

**Bottom line**: further progress here needs either (a) a legitimate,
citable reference for real PS1/PS2 BIOS syscall function numbers
(e.g. publicly published community documentation, as opposed to
disassembling the copyrighted BIOS binary itself) so specific A0/B0/C0
functions can be implemented for real instead of generically stubbed,
or (b) substantially deeper reverse-engineering of this specific
dump's boot sequence to pin down exactly which step is supposed to
install the exception dispatcher and why it isn't happening. Both are
a meaningfully bigger undertaking than the RFE/ERET/SYSCALL fixes
above, and represent a scope decision (this project has so far
deliberately avoided implementing guessed BIOS call semantics at all)
rather than a quick, obvious next fix.

### Resolved: InstallExceptionHandlers (C0h/0x07), using psx-spx as a citable reference

Chose option (a) above. **psx-spx**
(https://psx-spx.consoledev.net/kernelbios/) is a long-standing,
publicly published PS1/PS2 community technical reference, distinct
from disassembling this project's copyrighted BIOS binary - using it
as a citable source (rather than guessing) is consistent with this
project's standing policy of not fabricating hardware semantics.

Its documented "BIOS RAM Map" independently confirmed several things
found during the investigation above, word for word: address
`0x00000068` is documented as `"Unknown (set to 000000FFh)"` - i.e.
the `0x000000FF` byte pattern found there is genuinely correct,
expected, real-hardware content, not a bug or a leftover sentinel as
first suspected. Address `0x00000080` is documented as the
`"Exception vector (actually in KSEG0, ie. at 80000080h)"`, and the
BIOS function table documents `C(07h)` as `InstallExceptionHandlers()`
- exactly the real function this project's generic HLE stub was
trapping and silently no-op'ing.

The documentation also gives the *exact* real bytes real BIOS ROMs
write there: a fixed 4-instruction trampoline (`LUI $k0,0` / `ADDIU
$k0,$k0,<addr>` / `JR $k0` / `NOP`), noting the jump-target immediate
varies by BIOS revision. Rather than hardcoding one of the
documentation's example immediates (which could easily be wrong for
this exact SCPH-10000 dump), a Python scan of the actual loaded ROM
for this distinctive, unambiguous 3-of-4-words signature (word0 and
word2/word3 are fixed; only word1's low 16 bits vary) found **exactly
one match**, at ROM offset `0x003C5C`:
`3C1A0000 275A0C80 03400008 00000000` - confirming the `0x0C80`
variant, and, crucially, that this dump really does contain the
documented pattern verbatim.

Implemented in `source/hw/iop_hle_bios.c`: when the trapped call is
specifically `C0h` function `0x07`, the real 16-byte template is
located in the loaded ROM (via the same kind of signature scan used
to confirm it above, done once and cached) and copied verbatim to RAM
address `0x80` (the real exception vector) and mirrored to address
`0` (the documented "Garbage Area" echo) - using the dump's own real,
version-correct bytes, never a hardcoded guess. Every other A0/B0/C0
function number is completely unaffected and still gets the generic
default. Unit tested in `tests/test_iop_hle_exception_install.c`
(9/9 checks, using a synthetic BIOS image with a deliberately
distinctive planted immediate to prove the actual found bytes are
used, not an assumed value).

**Result, re-running the real-BIOS diagnostic**: dramatic further
progress on both cores.
- The **IOP** no longer halts at all - it was re-tested out to
  100,000,000 instructions (20x the previous diagnostic's cap) and is
  still running at the end of that slice, having made the exact same
  27 real HLE calls as before (confirming this fix didn't change *what*
  boot code does, only unblocked forward progress past the point that
  used to halt).
- The **EE**, run against that same larger cap, now gets to
  **53,592,141 instructions** (up from running out the previous
  5,000,000-instruction cap without incident - this is the first time
  the EE has actually been run far enough to hit a NEW wall) before
  halting on an unimplemented primary opcode in COP2/LQ-SQ territory -
  which is an entirely expected, already-documented gap (VU0/COP2 and
  128-bit LQ/SQ are both explicitly listed as not-yet-implemented in
  docs/ROADMAP.md section 1), not a surprise. Real BIOS boot code is
  now demonstrably running deep enough to need the vector unit.

This is the strongest real-BIOS result so far: both cores now make
substantial, real forward progress using nothing but genuinely
verified real-hardware behavior (real PCSX2 source for CPU semantics,
this dump's own actual bytes for the exception handler, and public
community documentation for what those bytes mean) - no fabricated
BIOS call semantics anywhere.

### LQ/SQ implemented - and a correction to the "53M instructions" framing above

Implemented `LQ`/`SQ` (128-bit load/store, primary opcodes 0x1E/0x1F),
ported from PCSX2's `R5900OpcodeImpl.cpp` - see `docs/ROADMAP.md`
section 1 for the exact semantics. Unit tested
(`tests/test_ee_lqsq.c`, 8/8 checks).

Re-running the real-BIOS diagnostic afterward: **the EE halts at the
exact same instruction count and PC as before (53,592,141, same
address)** - proving the earlier "COP2/LQ-SQ territory" halt label was
a guess, and LQ/SQ was never actually the real blocker. Investigating
properly (the same kind of trace-based diagnostic used for the IOP's
funct-0x3F halt) turned up a more important correction: **the "53
million instructions" figure was never 53 million instructions of real
boot progress**. The EE actually executes only about 99,262 real
instructions before a `JALR $ra, $s1` sends it to an address
(`0x03400008`) that lies beyond this project's emulated 32MB EE RAM.
This project's memory access correctly, safely returns 0 for anything
outside the allocated RAM buffer (not a bounds-checking bug - verified
in `ee_mem_read32`/`ee_mem_ptr`), and a MIPS word of `0x00000000`
happens to decode as a harmless `SLL $0,$0,0` (a real NOP) - so instead
of a clean crash or halt, the CPU just marches forward in a dead-straight
line, instruction by instruction, "executing" 53+ million NOPs across
the entire unmapped address space, until it eventually reaches the
hardware register window at `0x10000000+` and reads a LIVE, non-zero
SIF register value (`CTRL`, fixed real value `0xF0000102`) as if it
were an instruction - which is what actually halts (decodes to a
genuinely invalid opcode, `0x3C`). The halt message now reports the
real opcode and address (`ee_core.c`'s default case is enriched the
same way `iop_core.c`'s already was).

Traced `$s1`'s origin: it's loaded via `LW $s1, 8($s6)` at instruction
#99,257, where `$s6` was itself loaded two instructions earlier from
`0($s3)` - a pointer-chasing pattern consistent with reading fields
(possibly an entry-point-like field) out of some in-memory descriptor
structure (an EXE/module header, or similar), following the classic
"module/EXE loading" pattern also seen on the IOP side (`Exec()`/
`LoadExec()` in the psx-spx documentation) - but NOT yet confirmed
against any citable reference for the EE-side equivalent. Unlike the
two IOP fixes above, this doesn't yet have a clean, verified root
cause - it's flagged here as the next concrete EE investigation
target, honestly reported as unsolved rather than guessed at. Two
open, plausible directions: (a) the pointed-to descriptor was itself
never correctly populated (echoing the IOP's earlier "expected
resident data was never installed" category of bug), or (b) the
address is legitimately what real BIOS code computes, and this
project's 32MB EE RAM allocation and/or address-space layout needs
re-checking against real PS2 physical memory maps. Neither is
confirmed yet.

## Clock-rate-aware EE:IOP scheduling

The single most important EE investigation target above (the JALR to
unmapped memory) needs the real BIOS dump to make progress on, and
that dump only ever exists on this project's user's own machine for
local testing (see CLAUDE.md's "Real BIOS testing" section) - it's
never present in a fresh clone or a remote session. With that
particular thread blocked for this pass, picked the next concrete item
already reasoned through in `docs/ROADMAP.md`'s "Suggested near-term
order": the interleaved EE/IOP scheduler's known 1:1 instruction-count
simplification (real hardware's EE/IOP clocks are an exact 8:1 ratio -
294.912 MHz vs 36.864 MHz).

`system_run_interleaved()` (`source/core/system.c`) now steps up to
`EE_IOP_CLOCK_RATIO` (8) EE instructions per IOP instruction each
slice, instead of one each. Still honestly not true cycle-accuracy -
real MIPS instructions vary in cycle cost (loads, branches,
multiply/divide) - so this is an instruction-count ratio approximating
the real clock ratio, not a cycle-level model; `system.h`'s header
comment spells this out. Unit tested in `tests/test_system_ratio.c`
(9/9 checks): a partial run (5 slices, before either core halts) shows
the exact 8:1 count (EE=40, IOP=5), and a full run to completion shows
the IOP - given a much shorter absolute program but only 1
instruction/slice - correctly ending up as the scheduling bottleneck,
finishing after the EE despite starting from a "smaller" program. Also
re-ran the existing `tests/test_system_handshake.c` under the new
ratio - still 9/9, since its SIF poll-loop protocol doesn't depend on
strict 1:1 stepping. One incidental thing this test run confirmed
(not new, already documented in CLAUDE.md's "Known sharp edges"):
`halt()`'s early return means a BREAK-terminated program's final
`instructions_executed` count is the NOP count, not NOP count + 1 -
the halting instruction executes but isn't counted.

## Endianness bug found and fixed

Early memory-access code used `memcpy()` to read/write multi-byte
values, which silently assumes host and guest share the same byte
order. PS2 (EE/IOP) is little-endian; the Wii (PowerPC 750, our build
target) is big-endian. Every load/store and the BIOS ROMDIR parser now
compose/decompose bytes explicitly in little-endian order. This was
caught by the unit test in `tests/test_ee_core.c`, not by code review -
a good reminder that a native host-side test harness is worth having
even for code that only "really" runs cross-compiled on target.

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
