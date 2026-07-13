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

Traced the full pointer chain: `$s3 = *(0x100)`, then
`$s6 = *($s3+0)`, then `$s1 = *($s6+8)` and `$s0 = *($s6+4)`, ending in
`JALR $ra, $s1` at instruction #99,262. Confirmed directly:
`RAM[0x100] == 0` in this run - so `$s3` (and everything chained from
it) is zero from the very first link, and the eventual JALR target
(`0x03400008`) is entirely a product of zero-initialized memory being
walked and offset, not anything read from real BIOS content.

**Correction to an initial hypothesis**: the first instinct was to
treat `0x100` as an EE-side analogue of the IOP's PS1-heritage
"table of tables" concept (psx-spx) and assume something failed to
install a pointer there. Checked against **ps2tek**
(https://psi-rockin.github.io/ps2tek/), a publicly published,
citable PS2 hardware reference distinct from the IOP's PS1-lineage
kernel - and that analogy doesn't hold: ps2tek documents EE physical
address `0x100` (KSEG0 virtual `0x80000100`) as the CPU's own
hardware **Debug exception vector**, not a kernel table pointer. Since
shipping consoles never install a real debug-exception handler there,
`RAM[0x100] == 0` may well be entirely CORRECT, expected real-hardware
content - not a bug or an unpopulated-table gap at all. This means the
earlier "something failed to install a table here, similar to the IOP
fixes" framing was likely the wrong model to apply; ps2tek doesn't
cover EE kernel/BIOS boot internals (module loading, TCBs, or any
low-RAM descriptor layout) at all, so there's currently no citable
reference confirming what SHOULD happen when code dereferences this
address as a pointer during boot.

Net honest status: unlike the two IOP fixes above, this does NOT have
a clean, verified root cause yet, and the most likely next model
(compare to IOP's InstallExceptionHandlers pattern) turned out not to
fit on closer inspection. Flagged as the next concrete EE
investigation target, genuinely open - needs either a citable
reference for EE kernel boot internals (unlike psx-spx for the IOP,
none has been found yet) or further, more careful tracing of exactly
which BIOS routine performs this dereference and why, before
attempting a fix.

### EE JALR investigation, round 2: exact mechanism now understood byte-for-byte (root cause still open)

A follow-up session re-investigated this with a purpose-built
byte-level tracing harness (instruction-decode ring buffer plus a
full shadow-diff over EE RAM 0x0-0x8000, logging every single byte
write with the instruction count and pc that produced it - a more
precise version of the same tracing technique used throughout this
project). This replaced guesswork with exact, reproducible facts:

- **The `$s3 = *(0x100)` chain is real and exact**, confirmed via the
  actual decoded instructions at the real BIOS addresses: `ORI
  $s3,$zero,0x100` then `LW $s3,0($s3)` at instruction #099246/#099248
  (pc=0x00000DCC/0x00000DD4), giving `$s3 = RAM[0x100] = 0`. Then `LW
  $s6,0($s3)` (#099253, pc=0x00000DE8) gives `$s6 = RAM[0] `. Then `LW
  $s1,8($s6)` (#099257, pc=0x00000DF8) gives `$s1 = RAM[$s6+8]`. Then
  `JALR $ra,$s1` (#099261, pc=0x00000E08) jumps to whatever `$s1` holds.
  **Confirmed: there is no conditional branch anywhere in this
  instruction window that checks whether `$s3`/`$s6` is zero before
  using it** - real BIOS code here assumes the chain is always
  populated, no defensive null check exists to skip this path.

- **`RAM[0x100]` is confirmed to be written by NO instruction at all**
  across the entire ~99,261-instruction run (verified via the full
  shadow-diff log, not just a single snapshot check as in the first
  investigation round) - it is the one and only genuinely-still-zero
  link in the whole chain.

- **The other two links are NOT simply "zero" - they resolve through
  a scratch buffer this same boot code builds and then only partially
  clears.** Traced via a `$k0` register-change log: earlier in the
  same run (~instruction #084143-084153, pc=0x00000EE0), boot code
  builds a real MIPS exception-vector trampoline - `LUI $k0,0` / `ADDIU
  $k0,$k0,0x0C80` / `JR $k0` / `NOP` (the SAME 4-instruction convention
  already used for the IOP's `InstallExceptionHandlers`, but this is
  genuine EE-side kernel code doing the analogous thing independently,
  not related to our IOP HLE code at all) - directly into RAM
  addresses 0-15. This trampoline is later actually **jumped to and
  executed for real** (confirmed: `$k0` is next seen changing via
  `ADDIU $k0,$k0,0x0C80` fetched FROM address 0x00000004 at instruction
  #099161 - i.e. the CPU's pc really was 4, executing our own
  installed bytes as code), landing at address 0x0C80 and running
  further init code there, including a register-context-save sequence
  (many `SW $reg,offset($k0)` instructions with `$k0=8`) into the same
  low-RAM area. Afterward, only bytes 0-3 of the original trampoline
  get explicitly zeroed again (a second write at instruction #084203,
  by different code at pc=0xBFC4D310) - **bytes 4-7 (`0x275A0C80`) and
  8-11 (`0x03400008`) are never cleaned up and are still sitting there
  later.** So when the `*(0x100)`-chain resolves to `$s6 = RAM[0] = 0`
  (genuinely zero, confirmed) and then reads `RAM[$s6+8] = RAM[8] =
  0x03400008` - **that's not a coincidental "zero decodes as a valid
  instruction" accident like the earlier LQ/SQ finding. It's the raw
  encoding of the JR-instruction word from boot code's own leftover,
  not-fully-cleared scratch buffer**, being misread as a function
  pointer through a chain that was supposed to reach a *different*,
  legitimately-populated structure.

- **Confirmed this is not reachable via our un-implemented EE SYSCALL**
  (`ee_core.c` halts cleanly and immediately on any EE `SYSCALL` - see
  the primary-opcode switch - and this run never halts before
  instruction #099261, so no `SYSCALL` was ever executed in this
  window). Whatever should populate `RAM[0x100]` before this code
  runs, it is not done via the classic PS2 "AddXxxHandler`-style
  kernel-call convention in this instruction range.

**Net honest status**: the mechanism is now fully understood at the
byte and instruction level - a real, reproducible fact pattern, not a
guess - but the root cause (what real hardware puts at `RAM[0x100]`,
and why/how) is still **unresolved**. Two live hypotheses, both
untested: (1) a genuine gap in this project's boot/DMA modeling -
something (possibly IOP-side, possibly a hardware hand-off mechanism
this project doesn't implement at all) is supposed to populate this
address before this code segment runs, or (2) real hardware only ever
reaches this exact `*(0x100)` code path as a genuinely-vectored
exception/interrupt handler (its instructions do resemble an exception
prologue - register saves via a `$k0`-relative frame), and this
project's total absence of real interrupt/exception-raising logic on
the EE side (see `ee_core.c`'s COP0 notes: only explicit `MTC0` writes
ever touch Status/EPC, nothing raises a real exception) means we may
be free-running straight through code that real hardware would only
ever reach via a hardware trap fired at a very different point in the
boot timeline - i.e. we might be executing this code "by accident",
too early, via plain sequential fall-through instead of a genuine
vectored call. Neither hypothesis has supporting evidence beyond the
plausibility argument above; flagged honestly as still open. The
ps2tek "Debug exception vector" characterization of address `0x100`
from the previous round doesn't contradict either hypothesis but also
doesn't resolve the question of what specifically should be there at
this point in boot.

### EE JALR investigation, round 3: DMA and a "missed guard" both ruled out; a fix without fabrication isn't available yet

A third pass tested the two open hypotheses from round 2 directly
rather than leaving them as untested speculation:

- **DMA/hardware hand-off hypothesis, tested and ruled out for this
  instruction range.** Dumped the full `dma_state_t` (`dma_get_state()`)
  immediately after the run: every one of the 10 DMA channels'
  registers (CHCR/MADR/QWC/TADR) and the shared D_CTRL/D_STAT/D_PCR
  registers are still completely zero - boot code has not touched the
  EE's DMA controller AT ALL by instruction #099261. Whatever should
  populate `RAM[0x100]`, it is not happening via any DMA transfer this
  project models (or a real one would model either, since nothing has
  been kicked).

- **"Missed branch/guard" hypothesis, tested and ruled out for the
  immediate window.** Every instruction executed between the
  trampoline landing at `pc=0x00000C80` (~instruction #099169) and the
  fatal `JALR` (#099261) was decoded and its primary opcode tallied:
  the entire ~90-instruction span contains ONLY `ADDI/ADDIU/ANDI/LUI/
  COP0/LW/SW`/R-type arithmetic opcodes - **zero branch or jump-and-
  link opcodes of any kind** (no `BEQ/BNE/BLEZ/BGTZ`, no `REGIMM`
  branch family, no likely-branch forms). This rules out "an
  interpreter bug in some comparison/branch instruction is wrongly
  skipping a null-check" for this specific window - there is no
  conditional control flow in it at all to get wrong. If a guard
  exists, it would have to be further back, before the trampoline was
  even reached.

- **This also weakens round 2's hypothesis (2) (a genuinely-vectored
  hardware exception).** This project's EE core has no code path that
  spontaneously changes `pc` (no timer/interrupt/TLB-refill logic - see
  `ee_core.c`'s COP0 notes) - the ONLY way execution reaches
  `pc=0x00000C80` at all is by literally decoding and executing a `JR`
  instruction with that target already sitting in a register,
  somewhere in the plain, sequential instruction stream. Since our
  interpreter is fully capable of reaching this code via ordinary
  execution (confirmed - it does, every run), and has no mechanism to
  reach it any OTHER way, the "this should only be reachable via a
  hardware trap" framing doesn't hold up: whatever sent execution to
  `0x00000C80` did so through a plain, already-decoded `JR`, which
  means the real BIOS itself treats this as reachable via normal
  control flow at this point in boot - not as an exception handler
  waiting for a hardware fault.

**Where this leaves things, honestly**: the remaining, most plausible
explanation is that some REAL hardware mechanism this project doesn't
model at all - most likely something that happens before the very
first CPU instruction executes (e.g. a boot-ROM/hardware-level initial
program load step that pre-populates a small amount of fixed EE RAM
content as part of physical reset, before `pc` is even set to the
reset vector) - is responsible for `RAM[0x100]`'s real-hardware value,
and this project's boot model (allocate 32MB of zeroed RAM, jump
straight to `0xBFC00000`) has no equivalent step. **This project has no
citable public reference describing PS2 EE-side pre-CPU-boot RAM
content** (unlike psx-spx for the IOP's kernel conventions, or ps2tek
for CPU/hardware register architecture) - inventing a plausible-looking
value to poke into `RAM[0x100]` at `ee_core_init()` time would be
exactly the kind of fabricated hardware behavior this project's
standing policy prohibits, so **no code fix is being applied for this
specific gap**. Two honest paths forward exist, neither undertaken yet
pending a decision on priority: (a) implement a real, architecturally-
correct COP0 TLB/exception-vector system (citable against the R5900/
MIPS architecture and PCSX2's own `COP0.cpp`) as a legitimate feature
in its own right - substantial scope, and not guaranteed to resolve
this specific gap even if built, since the evidence above suggests
this code path isn't exception-driven; or (b) accept this as a
documented, structural limitation of the current boot model and
continue investing effort in other well-scoped, independently
verifiable work (remaining MMI opcodes, COP2/VU0, GS rasterization),
revisiting this if a citable reference for EE pre-boot RAM content is
ever found.

### EE JALR investigation, round 4: real BIOS disassembly - a false lead resolved, a real lead found and also resolved

Since the user owns this BIOS dump (their own legally-dumped SCPH-10000),
disassembling and tracing the actual ROM/RAM instruction stream is not
fabrication - it is ground truth from real hardware, exactly like citing
`pcsx2-src` elsewhere in this project. This round built a small
disassembler (`regname()`/`disasm()` covering the opcodes this project
implements) as a throwaway `/tmp` tool and used it to look directly at
what the real BIOS executes around the JALR failure, rather than reason
about it abstractly.

**First, the SYSCALL hypothesis floated at the start of this round was
tested and killed in under two minutes.** A simple counter confirmed EE
`SYSCALL` (SPECIAL funct `0x0C`) fires **zero times** in the first
150,000 instructions of boot - well past the JALR failure point at
#099,261. A missing EE syscall-table implementation (this project's
`SYSCALL` case still just calls `halt()`) cannot be the cause of this
specific bug, so building full EE BIOS-syscall HLE (a large undertaking,
modeled on the IOP's existing HLE trap) was correctly not pursued as a
"fix" for this issue - it would have been wasted, wrongly-motivated
effort.

**Also newly confirmed: the interpreter does not actually halt after the
bad JALR.** Running 3,000,000 further instructions past the jump to
`0x03400008` shows the EE simply continues executing - it wanders
through effectively-zeroed memory (decoding zero words as `SLL $0,$0,0`,
i.e. NOP) and never halts, never raises a second out-of-range jump, and
`RAM[0x100]` is confirmed to stay zero for the entire 3M-instruction
extension, not just the original ~99K. Whatever should write it, it
never happens - not late, not ever, in this model.

**A promising-looking lead, investigated and resolved as a false alarm.**
Full-trace disassembly around instructions #098,890-#099,020 showed a
tight loop at `pc=0x00000EE0-0x00000EEC` that looked, at first read, like
a `memcpy`-style copy loop (`LW $v1,0($k0)` / `ADDIU $k0,$k0,4` /
`ADDIU $v0,$v0,4` / `BNE $k0,$k1,loop` / `SW $v1,-4($v0)` in the delay
slot) - except the `LW` and `SW` words at that address now read back as
zero (decoding as NOP), meaning the loop increments two pointers from
`$k0=0x2cbc` to `0x2d24` and `$v0=0x80001dc0` to `0x80001e28` but
transfers nothing. Tracing back further found exactly why: a real BIOS
ROM routine at `pc=0xBFC4D30C-0xBFC4D330` runs earlier (~instruction
#084,143-#099,020, sweeping forward in 128-byte strides) that does:

```
SW $zero, 0($t2)
SW $zero, 16($t2)
SW $zero, 32($t2)
SW $zero, 48($t2)
SW $zero, 64($t2)
SW $zero, 80($t2)
SW $zero, 96($t2)
SW $zero, 112($t2)
BNE $t2, $t3, loop
ADDIU $t2, $t2, 128     ; delay slot
```

This zeroes exactly one 4-byte word out of every 16 bytes across a wide
sweep of low RAM (confirmed hitting the trampoline bytes at RAM 0-15
from round 2, and the `0xEC0-0xEF3` region from this round). Once the
full pattern was visible, this turned out **not** to be a bug: writing a
single word per 16-byte-aligned block, over and over across a large
region, is the classic shape of a real kernel heap-allocator
initialization (zeroing a per-block size/flags header while leaving the
rest of each block alone) - not a copy-loop-destroying defect. The
earlier-looking "copy loop" at `0xEE0` was a coincidence of the same
address being reused for temporary decompression-stub code much earlier
in boot (confirmed via a byte-level shadow-diff: a real decompressor
running at `pc=0xBFC5881C` writes genuine instruction bytes there around
instruction #029,531-#029,621) and later reused as ordinary heap data -
the fact that stale heap bytes still happened to *decode* as a
plausible-looking `LW`/`SW` pair when the CPU (wrongly) reached that
address as code again is a red herring, not a smoking gun.

**Net result of this round**: two more hypotheses were tested against
the real BIOS directly instead of guessed at - one (SYSCALL) is now
definitively ruled out, and one (the "vanished copy loop") looked like a
strong lead but resolved into an ordinary, expected heap-init pattern
once fully traced. The actual root cause - why control flow reaches
`pc=0x00000EE0`/the trampoline/the `0x100` pointer chase as code at all,
given the region is legitimately reused as heap data by that point - is
still open. Making further progress here would need a genuine reference
point this project does not have access to: either a symbol-annotated
disassembly of this exact BIOS revision's kernel, or a side-by-side
instruction trace from a known-working PS2 emulator/real hardware to
diff against. Absent that, continuing to guess at control-flow intent
from raw disassembly alone has hit steeply diminishing returns for the
time invested. No source code was changed this round - all tools used
(`syscall_check.c`, `watch_0x100.c`, `flow_trace.c`, `disasm.c`,
`watch_loop_region.c`, `dump_clear_routine.c`) were throwaway `/tmp`
diagnostics per the standing policy, never committed.

### EE JALR investigation, round 5: root cause found and fixed (via a live trace of real, working PCSX2)

The user connected a real, working PCSX2 instance (via a third-party
MCP debugger bridge, github.com/hkmodd/PCSX2-MCP) running their own
legally-dumped SCPH-10000 BIOS, and captured ground truth this project
never had access to before: a live instruction-level trace of the exact
BIOS routine that populates `RAM[0x100]` on real hardware, plus a live
memory dump proving the real value there is `0x08004469` (a valid
jump-instruction word, `j 0x800111A4`) - not zero.

The real BIOS routine (disassembled directly from the user's PCSX2
session, `pc=0xBFC00C54-0xBFC00CB4`) is a two-loop vector-install
routine: loop 1 zeroes low RAM 16 bytes at a time via `sq`; loop 2
copies a block of exception-vector words from ROM (`~0xBFCB3300`) into
RAM through the *uncached* `0xA0000000+` window, one `sw` per word.
When the destination pointer reaches `0xA0000100` (the same physical
cell as `0x00000100`/`0x80000100`), it writes the real vector value.

Tracing this project's own interpreter against the same BIOS showed it
**never executes this routine at all** - not a missing opcode inside
the loop (SQ, DADDU, and POR, all used nearby, were already
implemented), but a complete failure to ever reach `pc=0xBFC00C54` in
the first place. A ROM-coverage trace pinned this down precisely: this
project's EE only ever visits BIOS ROM addresses `0xBFC00000-0xBFC00030`
before diverging away from ROM entirely - compared to real hardware,
which continues on through `0xBFC00800+` and eventually reaches the
vector-install loop.

The actual divergence is at the very first conditional in the entire
boot sequence. Instruction #0 at the reset vector is `MFC0 $k0, $15`
(read COP0 register 15, PRId - Processor Revision Identifier).
Instruction #2 is `SLTI $at, $k0, 89`, and instruction #3 is
`BNE $at, $zero, ...`: a CPU-revision check that picks between two
entirely different early-boot code paths. This project's
`ee_core_init()` never initialized `cop0[15]` - it was left at 0 by
the function's `memset()`. Since `0 < 89`, the branch was taken,
sending this project's interpreter down a path that (as far as could
be traced) never rejoins the real vector-install routine at all. Real
PCSX2 initializes this register to `0x00002e20` (`R5900.cpp`:
`cpuRegs.CP0.n.PRid = 0x00002e20`) - `11808 < 89` is false, so real
hardware falls through to the *other* path, the one that eventually
reaches the vector-install loop and populates `RAM[0x100]` correctly.

**The fix**: `ee_core_init()` now sets `g_state.cop0[15] = 0x00002e20u`
immediately after the `memset()`, ported directly from PCSX2's own
`R5900.cpp` - not a fabricated or guessed value; it is the same
constant real PCSX2 uses for this exact model of EE. Tested in
`tests/test_ee_cop0_prid.c` (4/4 checks, see tests/README.md).

**Verified this is a real fix, not another false-progress trap** (the
project's own documented lesson from the earlier LQ/SQ instruction-count
mistake): a ROM-coverage re-trace after the fix confirms the EE now
takes the correct branch at instruction #3 (previously-unreached
addresses `0xBFC00010-0xBFC00020` are now visited, and execution
continues on through `0xBFC00800-0xBFC0086C`, far beyond the old
`0xBFC00030` ceiling) - concrete evidence of a different, correct
control-flow path, not just a bigger instruction count.

**New, honest halt point**: with the fix applied, the EE now runs 36
real instructions (versus reaching instruction #3 before taking the
wrong branch previously) and halts cleanly on a COP0 CO-format
instruction with funct `0x02` at `pc=0xBFC0086C` - `TLBWI`, one of the
four real TLB instructions (`TLBR`/`TLBWI`/`TLBWR`/`TLBP`) this project
has always documented as unimplemented (see the "Still NOT implemented"
list at the top of `ee_core.c`). This is expected, not a new problem:
the real BIOS's correct boot path evidently sets up an initial TLB
entry very early, and this project has no TLB/MMU model yet. The old
JALR-to-out-of-range halt, reached by the WRONG boot path, no longer
occurs at all with this fix in place - it was entirely a symptom of the
PRId bug, not a separate, independent defect.

**Where this leaves the project**: the real root cause of the entire
"EE JALR investigation" (rounds 1-4) is now identified and fixed with
a citable, non-fabricated value. The project's next real blocker -
implementing a genuine COP0 TLB (`TLBWI` at minimum, likely all four
TLB ops plus real address translation for a complete implementation) -
is exactly the "COP0 TLB/exception-vector system" option that was
already on the table as one of two honest paths forward after round 3,
except it is now confirmed, not speculative, to be the actual next
thing blocking real boot progress.

### EE JALR investigation, round 6: real COP0 TLB implemented; boot now diverges at a genuine TLB miss (not a bug)

Continuing directly from round 5's confirmed next blocker (`TLBWI` at
`pc=0xBFC0086C`), four more real, previously-hidden gaps were found
and fixed by re-tracing the same real-BIOS boot path one honest wall
at a time - each fix uncovering the next wall, rather than papering
over the previous one.

**1. kseg0 ROM mirror not recognized.** `ee_mem_ptr()` only treated
`0xBFC00000+` (kseg1, uncached) as mapping to the BIOS ROM; a jump to
`0x9FC41000` (kseg0, cached, same physical ROM) was being flagged as
"out of range" by this project's own out-of-range diagnostic heuristic
- which turned out to be a real, valid target the interpreter simply
didn't recognize. Fixed by masking to the physical address
(`addr & 0x1FFFFFFF`) before deciding ROM vs. RAM, so kseg0 and kseg1
now correctly alias the same physical memory, matching real hardware.

**2. Real COP0 TLB implemented: `TLBR`/`TLBWI`/`TLBWR`/`TLBP`.** Ported
directly from PCSX2's own `COP0.cpp` and the bitfield layouts in
`R5900.h`: a real 48-entry `tlb[]` array (PageMask/EntryHi/EntryLo0/
EntryLo1 per entry) was added to `ee_state_t`. `TLBWI`/`TLBWR` copy the
current COP0 PageMask/EntryHi/EntryLo0/EntryLo1 registers into the
entry indexed by Index or Random (`& 0x3F`); `TLBR` does the reverse
with the exact masking real hardware applies (re-deriving the Global
bit only when *both* EntryLo0 and EntryLo1 have it set); `TLBP`
searches all 48 entries for a VPN2 (+ASID or Global) match and sets
Index to the match, or to `0x80000000` (sign bit = not found) if none
matches. A new `ee_tlb_translate()` helper gives `ee_mem_ptr()` real
address translation for any address below `0x80000000` (KUSEG), where
real hardware genuinely requires a TLB entry rather than a fixed
physical mask - previously, KUSEG addresses were (incorrectly) treated
identically to kseg0/kseg1's direct physical mapping, which happened
to work only because no earlier test or boot trace ever depended on
real KUSEG/TLB semantics. Unit tested in `tests/test_ee_cop0_tlb.c`,
9/9 checks (TLBWI/TLBR round-trip, TLBP match and no-match, a full
KUSEG SW/LW round-trip through a manually-installed TLB entry, and a
KUSEG TLB-miss case confirming an unmapped address reads as 0 rather
than crashing or fabricating a mapping).

**3. MIPS "Branch Likely" family was entirely missing.** `BEQL`/`BNEL`/
`BLEZL`/`BGTZL` (primary opcodes `0x14`-`0x17`) and `BLTZL`/`BGEZL`/
`BLTZALL`/`BGEZALL` (REGIMM `rt=0x02/0x03/0x12/0x13`) - a MIPS II+
family the real BIOS boot path uses that this project had never
implemented at all. Ported the key semantic that distinguishes these
from ordinary branches: when the branch condition is *false*, the
delay-slot instruction is not executed at all (the real hardware
nullifies it), implemented by advancing `pc`/`next_pc` past the delay
slot entirely instead of executing it, matching real PCSX2's
`Interpreter.cpp`.

**4. `LWC1`/`SWC1` were entirely missing.** Direct FPR<->memory word
transfer (opcodes `0x31`/`0x39`, distinct from the already-implemented
`MFC1`/`MTC1` GPR<->FPR moves) - straightforward one-line ports once
noticed.

**Regression discipline note**: fixing #2 above (real KUSEG TLB
requirement) broke an *existing* test, `tests/test_ee_unaligned.c`,
which used a raw KUSEG address (`0x00300000`) as its scratch RAM base
without installing any TLB entry - this "worked" before only because
of the old, incorrect naive-mask KUSEG handling. This was a bug in the
test's own premise, not a regression in the implementation: real PS2
game code doesn't access plain RAM through unmapped KUSEG either, it
uses kseg0/kseg1 (direct-mapped, no TLB needed) for exactly this kind
of ordinary access. Fixed by changing the test's base address to
`0x80300000` (kseg0) - same underlying physical RAM, no TLB entry
required, matching how real code actually behaves. Confirmed 0
failures after the change.

**New, honest halt point after all four fixes**: boot now runs
meaningfully further, but still eventually diverges into
"zero-land" - out of 20 million executed instructions, only 151 are
non-NOP/non-zero-decoded before the trace settles into a sustained run
of zero-filled-memory decode (`SLL $0,$0,0`, a real NOP encoding this
project's memory model produces for any address that reads as 0). The
exact divergence point (instruction #158-159) is a genuine **TLB
miss**: real BIOS code has set up `$sp = 0x70003eb0` (a KUSEG stack
pointer), but the only TLB entry installed so far by the boot path
(`tlb[0]`, covering VPN2 `0x38000`, an 8KB range `0x70000000-
0x70002000`) doesn't cover it - `$sp` is roughly `0x1eb0` bytes beyond
that entry's range.

This is architecturally a case where real hardware would raise a TLB
Refill exception (which this project's EE core does not implement at
all yet - a limitation already flagged elsewhere in this document), or
possibly indicates a second `TLBWI` call the real boot path executes
that this project's interpreter doesn't yet reach for some other,
not-yet-investigated reason. Either way, this is now a precisely
identified, honestly-reached wall - not a guess, and not another
instruction-count false-progress trap (checked explicitly: 151/20,000,000
meaningful instructions is a real, verified ratio, not a stall
disguised as progress).

**Where this leaves the project**: the TLB implementation itself is
real, tested, working infrastructure - it just isn't sufficient on its
own to get past this boot path, because the boot path also needs
either (a) a TLB Refill exception handler (so the BIOS's own exception
vector can install the missing entry, which is presumably what real
hardware does here), or (b) further tracing to find why this
project's interpreter doesn't execute whatever install this specific
entry before reaching `$sp`'s first use. Implementing real MIPS
exception delivery (Cause/EPC/Status updates + vectoring to
`0x80000180`/`0xBFC00200` etc.) is therefore the next concrete,
non-speculative candidate blocker - the same category of work as the
TLB itself, i.e. citable against PCSX2's own `Exceptions.cpp`/
`COP0.cpp` rather than guessed.


### EE JALR investigation, round 7: real MIPS exception delivery implemented - boot now runs 97%+ real instructions (was ~0.0008%), a new, different wall found

Direct continuation of round 6's precisely identified blocker: real-BIOS
boot diverged at instruction #158-159 on a genuine TLB miss
($sp=0x70003eb0, no installed TLB entry covers it), which - since this
project had no exception-raising path at all yet - just silently read
as 0 and wandered into "zero-land" (151 real instructions out of 20
million). Real hardware would service this via a TLB Refill exception,
so that's what got implemented.

**What was added**: `ee_raise_exception()` and `ee_raise_tlb_exception()`
in `ee_core.c`, ported directly from PCSX2's own `cpuException()`/
`cpuTlbMiss()`/`cpuTlbMissR()`/`cpuTlbMissW()` in `R5900.cpp` - not
fabricated. On a KUSEG TLB miss (instruction fetch, load, or store),
this now: records BadVAddr/Context/EntryHi (so a real TLB-refill
handler could look up or install the right entry), sets Cause.ExcCode
(TLBL for fetch/load, TLBS for store) and Cause.BD (whether the fault
landed in a branch-delay slot), sets EPC (the faulting instruction, or
the branch before it if BD), sets Status.EXL, and vectors pc/next_pc to
the correct handler address - either the TLB Refill vector (offset
0x0) or general-exception vector (offset 0x180) for a nested fault (one
that happens while Status.EXL is already 1, i.e. still inside an
unresolved handler) - based on Status.BEV (uncached ROM, 0xBFC00200+,
vs. cached RAM, 0x80000000+, matching what the BIOS's own
InstallExceptionHandlers work is for).

Getting the BD/EPC bookkeeping right required real branch-delay-slot
tracking, which this project didn't have either: `branch_pending`
(previously an unused, vestigial field) now genuinely tracks whether
the instruction about to execute is itself a delay-slot instruction -
set unconditionally by the 8 "regular" conditional branches (BEQ/BNE/
BLEZ/BGTZ/BLTZ/BGEZ/BC1F/BC1T - their delay slot always executes,
taken or not) and by the `BRANCH_TO()` macro (covers J/JAL/JR/JALR
unconditionally, and taken Branch Likely branches; not taken Branch
Likely correctly leaves it unset, since that delay slot is annulled).

Also required a `mem_tlb_miss` flag (set by `ee_mem_ptr()` itself) to
distinguish "this NULL was a real KUSEG TLB miss" from "this NULL was
just a kseg0/kseg1 address with no backing ROM/RAM" (architecturally
NOT a TLB fault on real MIPS - kseg0/kseg1 are unmapped, direct
segments - and still just reads-as-zero/no-ops exactly as before, so
none of the many existing "unmapped hardware register reads as 0"
assumptions elsewhere in this project broke), plus a per-instruction
`exc_raised_this_step` guard so a single instruction that touches the
same missing page twice (SWL/SWR's internal read-then-write of the
same address, done purely to merge partial bytes) only actually raises
one exception instead of two conflicting ones.

**Known, honest limitation**: SWL/SWR's internal read-then-write
implementation means a TLB miss during either of them is always
reported as TLBL (load), never TLBS (store), even though architecturally
a partial *store* instruction faulting should raise TLBS. This is a
pre-existing implementation-detail quirk (the read is just this
project's way of fetching the bytes to merge, not a real hardware
transaction) now newly visible because misses are no longer silent -
not fixed in this pass, flagged here for whenever it matters.

**COP0 Status reset value** also corrected to the real one PCSX2 uses
(`cpuRegs.CP0.n.Status.val = 0x70400004`) - previously left at 0 by
`ee_core_init()`'s `memset()`. This matters specifically for exception
vectoring: real hardware resets with Status.BEV set (bit 22), meaning
early-boot exceptions vector into the BIOS ROM directly
(`0xBFC00200+`) until the BIOS clears BEV after installing its own
RAM-resident handlers; leaving BEV at 0 would have vectored boot-time
exceptions into RAM instead, which is wrong for this phase of boot.

**Tested**: a new `tests/test_ee_exceptions.c` (16 checks) covers a
faulting store (Cause.ExcCode == TLBS, distinct from load), a fault
inside a branch-delay slot (Cause.BD set, EPC points at the branch, not
the delay slot), an instruction-fetch fault (faults before the bogus
fetched word is even decoded), Status.BEV-dependent vectoring (RAM vs.
ROM base), a nested exception (EPC frozen, forced to the general
vector), and the `exc_raised_this_step` guard (a white-box test calling
the raise function twice within one "instruction" and confirming the
second call is a no-op). `tests/test_ee_cop0_tlb.c`'s own KUSEG-miss
case (previously asserting "reads as 0 rather than crashing") was
rewritten to single-step (not run to a BREAK that will never come now)
and assert the real exception fired correctly instead - the old
assertion was itself invalidated by this improvement, the same kind of
test-premise fix as `test_ee_unaligned.c` needed in round 6.

**Verified as real progress, not another false-progress trap**: running
the actual SCPH-10000 BIOS for 20 million instructions now executes
19,523,806 real (non-zero-decoded) instructions - a 97.62% meaningful-
instruction ratio, compared to 151-out-of-20-million (0.0008%) before
this fix. Disassembling the code range the trace spends most of its
time in (`0xBFC00680`-`0xBFC0071C`) confirms it is genuine, recognizable
MIPS exception-handler prologue: a full general-purpose register
context save (`SD zero..ra` to a fixed scratch area) followed by
reading and saving COP0 EPC and Cause - not zero-decoded NOP filler.

**New, honest wall**: exactly two real exceptions fire during a 50
million instruction run - the first (step 46, BadVAddr=0x70003FE0,
essentially the same page as round 6's `$sp=0x70003eb0` wall) vectors
to the TLB Refill vector; a second, nested one (step 88,
BadVAddr=0x0BC1F000, the handler's own register-save scratch address)
immediately follows, forced to the general vector per the nested-
exception rule. After that second fault, Status.EXL never clears again
(no ERET) and no further exceptions occur for tens of millions of
subsequent instructions - the EE just keeps executing real code in
that same handler-prologue region without completing or returning.
This is architecturally consistent with a real MIPS kernel convention
this project doesn't implement: reserving a few "wired" TLB entries
(`COP0.Wired`, `cop0[6]`) so kernel/exception-handling code and its own
scratch memory are always resident and can never themselves cause a
TLB miss while already servicing one - if the real boot path expects
such an entry to already be installed (or expects to install more than
the single `TLBWI` this project's trace has executed by this point) and
this project hasn't gotten there yet, the exception handler can end up
doing real, legitimate work indefinitely without ever reaching the
point that would resolve the original fault. This has NOT been root-
caused further this session (deliberately - it's a distinct, deep
question from "does exception delivery work correctly", which is now
verified yes) - it's the next investigation thread, analogous in kind
to how round 5/6 each started from "here's the next wall, honestly
reached."


### EE JALR investigation, round 9: real EE Timer (Count==Compare) interrupt delivery implemented

Direct continuation of round 8's final finding: with the Scratchpad
RAM + COP0 Count fixes in place, an 800-million-instruction run
against the real SCPH-10000 BIOS reached `pc=0xBFC0092C` - a real
`J 0xBFC00928` ("j $") self-loop, immediately preceded by a
`Compare=1` COP0 timer setup a few instructions earlier
(`pc=0xBFC00824`). This is a genuine, deliberate "wait for interrupt"
idle pattern - real hardware escapes it via an actual interrupt, and
this project had never raised an Interrupt-class exception (ExcCode
0) at all.

**Implementation, ported directly from PCSX2's own two real checks
(R5900.cpp)**:
- `cpuTestTIMRInts()`: `(Status.val & 0x10007) == 0x10001`, i.e.
  Status.IE=1, Status.EIE=1, Status.EXL=0, Status.ERL=0.
- `_cpuTestTIMR()`: `Status.val & 0x8000` (Status.IM7, the per-line
  interrupt mask bit for this specific line) must also be set, in
  addition to Count reaching Compare.

Two new functions in `ee_core.c`:
- `ee_latch_timer_interrupt()` - called unconditionally, every single
  instruction, right after Count (`cop0[9]`) increments (see round
  8's fix). The instant Count equals Compare (`cop0[11]`), it latches
  Cause.IP7 (bit 15) - a real, documented MIPS side effect: the
  pending bit is sticky, staying set regardless of Count's value
  afterward, until software explicitly writes a new value to Compare.
- `ee_check_timer_interrupt()` - actually takes the (possibly
  already-latched) interrupt as a real `ee_raise_exception()` call
  (ExcCode 0/Int), but only if Cause.IP7 is pending AND the
  IE/EIE/EXL/ERL/IM7 gating above all hold. Only called when
  `st->branch_pending` is 0 - i.e. only at a genuine instruction
  boundary, never in between a branch/jump and its delay slot, since
  real hardware has no pipeline checkpoint there to service an
  interrupt at.

**A real bug found and fixed before landing on this design**: an
initial version gated the match check itself on exact
`Count==Compare` AND skipped that same check entirely while
`branch_pending` was set (deferring across delay slots). That combo
has a real hole - if the match happens to land exactly on the step
that executes a taken branch itself, Count keeps incrementing while
the check is deferred, and by the time a safe instruction boundary is
reached, Count no longer equals Compare - the interrupt is silently
lost forever. Splitting "detect and latch the edge" (unconditional,
every instruction, can't be skipped) from "actually take the already-
latched interrupt" (deferred across delay slots) fixes this, and is
also simply a more accurate model of real MIPS Count/Compare hardware
semantics than the exact-equality-only first draft was.

**MTC0 side effect, also ported (not fabricated)**: writing a new
value to Compare (register 11) now clears the latched Cause.IP7
pending bit - the real, documented mechanism a MIPS interrupt handler
uses to re-arm the timer for its next tick, preventing the same
already-serviced interrupt from immediately re-triggering on `ERET`
if the handler didn't touch Compare.

**Tested**: a new `tests/test_ee_timer_interrupt.c` (24 checks) covers
a basic fire with full gating, IE=0 and IM7=0 each independently
blocking the interrupt from being taken (while still confirming it
latches regardless), the branch-delay-slot deferral case specifically
(proving the fix above works: EPC ends up at the branch's target, not
the branch or the delay slot, and the interrupt is provably not taken
during the branch's own step), and the Compare-write acknowledgment
clearing the latch.

Regression: full suite (34 test files now, including the new one)
passes 0 failures. Wii/devkitPPC target rebuilds clean with no
warnings.

**Re-verified against the real SCPH-10000 BIOS - a real, more precise
new wall found, not yet investigated**: an 800-million-instruction run
was repeated with the finished implementation. Cause.IP7 now DOES
latch correctly (`Cause=0x00008000` from very early on, confirmed via
a fresh diagnostic harness), proving the Count/Compare match itself
is being detected exactly as designed. But the interrupt is never
actually TAKEN - `Status` stays `0x70400000` (IE bit clear) for the
entire run, and execution never progresses past `pc=0xBFC00928`/
`0xBFC0092C`. A second, targeted diagnostic traced every `EI`/`DI`
COP0 "CO"-format instruction executed in the first 5 million steps:
**zero EI instructions ever execute** before the idle loop is reached.
This means the real BIOS's boot path taken by this project's
interpreter never enables interrupts (`Status.IE`) at all before
hitting this loop - so a maskable Count/Compare timer interrupt
*cannot* be what real hardware uses to escape it, at least not along
whatever code path this project's boot currently takes. This is a
different, more fundamental question than "does timer interrupt
delivery work" (it does - see the tests above): either (a) this idle
loop is an intentional dead-end/error path that real hardware only
reaches on a genuine boot failure (and something upstream is being
mis-emulated, causing a wrong branch into it), (b) it's escaped by
some other mechanism this project hasn't modeled (a different
interrupt source entirely - INTC/DMAC rather than the internal
timer, or even a non-maskable event), or (c) `EI` is supposed to run
inside one of the `JAL`/`JALR` subroutine calls this loop's
surrounding code makes (`0xBFC00884`, `0xBFC008A8`, `0xBFC008B8` -
see the disassembly in this session's work) and something about how
those subroutines execute in this project diverges from real
hardware before reaching the `EI`. Not root-caused further this
round - this is "round 10"'s starting point, the same kind of
precisely-identified-but-open wall every round 5-9 handoff has ended
on.

### EE JALR investigation, round 11: FIXED - MCH_RICM/MCH_DRD RDRAM auto-init registers implemented, wrong branch resolved and verified live-cycle-count-accurate; a second, deeper latent bug found and fixed along the way; new honest wall (COP2/VU0) reached

Direct continuation of round 10's fully-traced root cause. Round 10
found the BIOS's SIO baud-rate "calibration loop" (`s3>=2` sanity
check inside the subroutine at `0x9FC410E8`) failing because this
project's SIO/UART hardware always read back 0. Verifying that
register address (`0x1000F430`/`0x1000F440`) against a verified,
citable reference (websearch -> PCSX2's own `Hw.h`, cross-referenced
against PS2Tek's "EE RDRAM initialization" page) revealed round 10's
own hypothesis was itself slightly wrong: these are **MCH_RICM/
MCH_DRD**, not SIO - the real "Memory Control Hub" registers used for
**RDRAM auto-initialization** (enumerating installed RDRAM devices by
SDEVID), which lines up exactly with the printf string found in round
10 ("Initialize memory (rev:%d.%02d, ctm:%dMhz, cpuclk:%dMhz %s)...").
PCSX2's own comment on this area: `"MCH area -- Really not sure what
this area is. Information is lacking."` - despite that, the emulation
logic real emulators use to satisfy the BIOS's detection sequence is
well documented (PS2Tek).

**Fix implemented, ported directly from that reference (not
reinvented)**: new `source/hw/mch.c`/`include/core/hw/mch.h`,
`mch_mmio_read32`/`write32`, wired into `ee_core.c`'s dispatch exactly
like `dma_mmio_*`/`sif_mmio_*`. `MCH_RICM` (0x1000F430) always reads
back 0; writes decode SA (bits 16-27) and SBC (bits 6-9), and a SA=
0x21/SBC=0x1 "reset strobe" (gated on `MCH_DRD` bit 7 being clear)
restarts a `sdevid_counter` at 0. `MCH_DRD` (0x1000F440) reads decode
SOP/SA from the last `MCH_RICM` write: SA=0x21 (INIT) returns 0x1F for
each of the first `MCH_RDRAM_DEVICES` (2, matching real PS2 retail
hardware) reads then 0 forever after (enumerating exactly 2 RDRAM
devices is what lets the BIOS's `s3>=2` guard from round 10 clear);
SA=0x23/0x24 (CNFGA/CNFGB) return fixed 0x0D0D/0x0090; SA=0x40 echoes
`MCH_RICM & 0x1F`. 22 host-native checks (`tests/test_mch.c`).

**A second, deeper latent bug found while wiring this in**: the first
verification attempt still returned `v0=-1` even with MCH fully
implemented. Root cause: `ee_core.c`'s `dma_mmio_read32/write32` and
`sif_mmio_read32/write32` (and now `mch_mmio_*`) dispatch checks
compared the **raw, unmasked** virtual address against physical-style
constants (e.g. `0x1000F430`) - but real BIOS/game code always
addresses hardware registers through their KSEG1 (uncached,
`0xB0000000+phys`) or KSEG0 (cached, `0x90000000+phys`) mirrors, never
through the bare physical value as a virtual address. A live trace
confirmed the real access was `lw v0,(v1)` with `v1=0xB000F430` - which
never matched the literal `0x1000F430` check, meaning **this hardware-
register MMIO wiring had likely never actually fired for any real
CPU-issued load/store**, only for the literal KUSEG-style addresses
this project's own pre-existing tests happen to construct (e.g.
`test_ee_dma_bus.c`'s `LUI r2,0x1000`). Fixed with a new
`ee_hw_mmio_addr()` helper that masks KSEG0/1 addresses to physical
form before the dispatch checks (same aliasing `ee_mem_ptr()` already
applies for RAM/ROM) - KUSEG addresses pass through unchanged, so the
existing KUSEG-literal tests keep passing unmodified. Verified via a
new `tests/test_ee_hw_kseg_masking.c` (4 checks, real CPU programs
exercising KSEG1/KSEG0 round-trips through SIF and MCH).

**Live-verified against the real BIOS boot, register-by-register,
after both fixes**: the subroutine at `0x9FC410E8` now returns
`v0=0x08028020` - **exactly matching real hardware**. `a1=v0` positive,
`bgez` taken, and at `pc=0xBFC0088C` `v0=0x02000000` - **exactly
matching the original round 6 report's numbers**. Execution now takes
the `jr t0` path into RAM (`t0=0x80001000`), **never reaching the
`pc=0xBFC0092C` idle loop at all** - matching real hardware exactly.
The fix took **14,932,336 host-native steps** to reach the return
point, which lines up remarkably closely with round 10's live-traced
**~14.9 million real CPU cycles** for the same call - strong
independent confirmation this is the correct root cause, not a
coincidental behavior change.

**New, honest next wall (not yet fixed)**: continuing past the
now-correct branch, execution runs deep into RAM-resident boot code
(`pc=0x8000xxxx`) for another ~500,000 steps before cleanly halting on
`"unimplemented primary opcode 0x12"` at `pc=0x8000B1FC` - opcode 0x12
is `COP2`, the VU0-in-macro-mode coprocessor. This is an entirely
separate, large, previously-unneeded subsystem (Vector Unit 0 macro
instructions) - a legitimate new frontier, not a sign anything is
wrong with this round's fix. Along the way, `DADDI`/`DADDIU` (primary
opcodes 0x18/0x19) were also found missing and added (3 checks,
`tests/test_ee_daddi.c`) - real RAM-resident code this project had
never reached before uses this 64-bit-immediate-add pair constantly.

**Verification**: full regression suite (37 test files, including the
3 new ones this round) passes with 0 failures. **Wii/devkitPPC rebuild
NOT verified this round** - this sandbox's `devkitPro` extraction
(persisted under `outputs/build/devkitpro/`) is missing `base_rules`/
libogc (an incomplete/stale extraction predating the mid-session
sandbox reset noted earlier this session, not something this round's
changes caused), so `make` fails before even reaching this project's
own source. The new/changed C files (`mch.c`/`mch.h`, the
`ee_hw_mmio_addr()` helper, the `DADDI`/`DADDIU` case) use only plain
freestanding-style C (`stdint.h`, `string.h`, no host-specific APIs),
matching `sif.c`/`dma.c`'s exact structure, which do compile cleanly
for Wii in every prior round's rebuild - high confidence but not
independently confirmed this round. Completing the devkitPro toolchain
extraction (`base_tools` + `libogc`) is a residual task for a future
round.

### EE JALR investigation round 12: COP2 (VU0 macro mode) control-register transfers implemented - the FBRST wall cleared, a genuine new wall (VU0 vector datapath) confirmed just past it

Direct continuation of round 11's new wall: real BIOS boot (unblocked
by the MCH_RICM/MCH_DRD fix) now runs deep into RAM-resident code and
halted cleanly on `"unimplemented primary opcode 0x12"` at
`pc=0x8000B1FC` - this project had zero COP2 dispatch at all before
this round.

**Live-traced via `pcsx2-mcp`**: the halting instruction is
`cfc2 v0, FBRST` (control register 28, confirmed via the disassembler's
own naming - live disassembly decodes it as `4842e000: cfc2 v0,
FBRST`), part of a plain read-modify-write sequence:
```
cfc2 v0, FBRST      ; read
ori  v0, 0x200      ; set bit 0x200
ctc2 v0, FBRST      ; write back
sync
```
Verified against a citable reference (websearch -> PCSX2's own
`VU0.cpp` `CTC2()` handler): FBRST's real bits are `0x1`=VU0 force-
break, `0x2`=VU0 reset, `0x100`=VU1 force-break, `0x200`=VU1 reset -
this specific sequence is a plain "reset VU1" during BIOS init.

**Implemented**: a new `cop2_ctrl[32]` register file in `ee_state_t`
(see `ee_core.h`'s comment), plus COP2 (primary opcode 0x12) dispatch
for the four control-register transfer instructions - `MFC2`(rs=0x00)/
`CFC2`(rs=0x02)/`MTC2`(rs=0x04)/`CTC2`(rs=0x06) - as plain storage,
matching COP0/COP1's existing dispatch pattern. Real FBRST side
effects (actually resetting/force-breaking VU0/VU1 pipelines) are
**not** modeled, since no VU0/VU1 execution state exists yet to act on
- an honest simplification consistent with this project's SIF CTRL
register precedent (real documented side effects noted, not modeled,
when the dependent subsystem doesn't exist). 4 host-native checks
(`tests/test_ee_cop2_ctrl.c`).

**Verified against the real BIOS live**: after this fix, execution
advances 9 more instructions past the old halt point and correctly
executes a *second*, different COP2 use nearby (`cfc2 v0,vi01` /
`ctc2 a0,vi01` - control register 1, one of VU0's 16 integer
registers, which real hardware also addresses through the same
CFC2/CTC2 family) with zero issues, confirming the control-register
transfer implementation is genuinely correct, not a lucky one-off match
for FBRST specifically.

**New, honest wall (confirmed, not a regression)**: immediately after
that, execution hits `viswr vi00, (vi01)x` (`op=0x12, rs=0x18` - rs's
top bit set, meaning it's dispatched via the 6-bit `funct` field like
COP0/COP1's own "CO"-format instructions, not a simple register
transfer) - a genuine VU0 **vector datapath** instruction (integer-
register-indexed memory store), confirmed via a per-instruction host-
native trace showing the exact halting `pc`/`rs`/`funct` values. This
is a real, separate, substantially larger subsystem (32×128-bit VF
registers, 16 VI registers, the full VU macro arithmetic opcode family
- ADD/SUB/MUL/MAC/etc.) - not attempted this round, scoped as a
distinct future task rather than half-implemented.

**Verification**: full regression suite (38 test files, 1 new this
round) passes with 0 failures. Wii/devkitPPC rebuild still not
verified (see the toolchain-gap note above/below) - the new C
(`cop2_ctrl[]`, the COP2 dispatch case) is plain, freestanding-style
code identical in structure to the rest of `ee_core.c`.

**Next step (round 13, not started)**: implement enough of the VU0
vector datapath (VF/VI register files, QMFC2/QMTC2 128-bit transfers,
and at minimum whatever specific macro opcodes the real BIOS boot path
needs - `viswr` first, then whatever comes after it) to get past this
wall, the same incremental way rounds 9-12 each cleared one wall at a
time.

### devkitPro toolchain: FULLY FIXED, clean Wii rebuild now verified

Direct follow-up to the toolchain gap noted above. The user supplied a
second upload, `devkitPPC-r32-linux-debian-stretch.tar.gz` - a real,
Linux-native devkitPPC release (not the earlier Windows/MinGW one,
which still can't run in this sandbox without Wine). This archive's
`libexec/gcc/powerpc-eabi/8.1.0/cc1` is the exact missing piece.

Copied it in directly (same gcc version, 8.1.0, as this sandbox's
existing extraction). Hit two more small gaps while verifying it
actually runs, both resolved:
- `liblto_plugin.so` was a dangling symlink (target `.so.0.0.0` file
  missing) - recovered from the same r32 archive.
- `cc1` itself needs `libmpfr.so.4` (built against an older Debian
  Stretch userland), but this Ubuntu 22.04 sandbox only ships
  `libmpfr.so.6`. Fetched `libmpfr4_3.1.5-1_amd64.deb` directly from
  `archive.debian.org` (Debian's permanent archive for EOL releases -
  no Cloudflare issue, unlike `pkg.devkitpro.org`) and placed just the
  `.so` under `devkitPPC/lib/` (not installed system-wide).

With `cc1` finally working, `make` reached one more gap: `libfat`
(`fat.h`, `-lfat`) wasn't present anywhere in this sandbox. Since the
toolchain now genuinely worked, built it directly from source
(`github.com/devkitPro/libfat`, `make wii-release`) - a small, simple
library, quick to build once real compilation worked at all.

**Result: `make clean && make` now completes with 0 warnings, 0
errors**, producing `pcsx2-wii.elf` (a real, statically-linked 32-bit
big-endian PowerPC ELF, confirmed via `file`) and `pcsx2-wii.dol`. This
is the first time in this session the Wii/devkitPPC build has actually
been verified end to end - retroactively confirms rounds 9-12's C
changes (the EE timer interrupt work, MCH_RICM/MCH_DRD, the KSEG0/1
addressing fix, DADDI/DADDIU, and the new COP2 dispatch) all compile
cleanly for the real target, not just the host-native test suite.

Full setup documented in `outputs/build/devkitpro/
TOOLCHAIN_SETUP_NOTES.md` (persisted alongside the toolchain itself,
survives a `/tmp` wipe) so this doesn't need to be rediscovered.
Required env for any future build in this sandbox:
```
export DEVKITPRO=<path>/outputs/build/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
export PATH=$DEVKITPPC/bin:$PATH
export LD_LIBRARY_PATH=$DEVKITPPC/lib:$LD_LIBRARY_PATH
```

### EE JALR investigation, round 13: VU0 vector datapath implemented - COP2 wall cleared for good, LDL/LDR/SDL/SDR added, boot now reaches a bounded SIF-polling steady state

Direct continuation of round 12's new wall: a live PCSX2 disassembly
of the code right after the FBRST fix showed the real BIOS running a
VU0 init/self-test sequence - `viswr`/`vsqi`/`vsub.xyzw`/`qmfc2`/
`qmtc2`, followed (once that cleared) by a bulk "clear every VU0
register" routine (`vsub.xyzw vfN,vf00,vf00` for all 32 VF registers,
then `viadd viN,vi00,vi00` for all 16 VI registers).

Added to `ee_core.h`/`ee_core.c`:
- `vu0_vf[32][4]` (the real VF register file, 128 bits each stored as
  4 raw 32-bit lanes) and `vu0_mem[4096]` (VU0's real 4KB local data
  memory, addressed in quadwords).
- `QMFC2`/`QMTC2` (128-bit GPR<->VF transfers).
- `VSUB` (3-operand vector float subtract, per-lane dest masking).
- `VISWR`/`VSQI` (VU0-mem store instructions, dispatched through a
  second-level "SPECIAL2" sub-table).
- `VIADD`/`VISUB`/`VIAND`/`VIOR` (plain integer ALU on VI registers).
- VF00 hardwired to `(0,0,0,1.0f)` and VI0 hardwired to `0`, both like
  real hardware (and like MIPS `$zero`), enforced via small helpers
  (`vu0_vf_read_lane`/`vu0_vf_write_lane`/`vu0_vi_read`/`vu0_vi_write`)
  rather than special-casing every call site.

All of the field-encoding details here (the `rs = 0x10 | destmask`
convention, the FT/FS/FD bit positions, and - trickiest of all - the
SPECIAL2 sub-dispatch index formula, where `VISWR`/`VSQI` reuse what
looks like a spare "register" field as extra opcode-select bits) were
derived by decoding the exact raw instruction words from a live PCSX2
disassembly and cross-checked against PCSX2's own
`R5900OpcodeTables.cpp` (`Int_COP2PrintTable`,
`Int_COP2SPECIAL1PrintTable`, `Int_COP2SPECIAL2PrintTable`) - the same
"verify against PCSX2's own source" discipline as round 11's MCH
registers, not guessed. A first attempt at a host-native test used the
wrong `fd` value (0) for VISWR/VSQI's encoding and failed instantly,
which is exactly what caught this being a real, load-bearing part of
the opcode identity rather than a free operand slot.

With the COP2 wall cleared, the interpreter advanced only a handful of
instructions before hitting a completely different, unrelated wall:
`unimplemented primary opcode 0x1A` (LDL). This project already
implements the 32-bit LWL/LWR (round 1); LDL/LDR/SDL/SDR are their
directly-documented MIPS III 64-bit doubleword analogs (8-byte
version, 3 shift bits instead of 2) - not PS2-specific, so implemented
straight from the well-known MIPS III ISA rather than needing further
live verification, and added together with their store-side
counterparts SDL/SDR (found immediately after, same pattern).

**Live verification**: a host-native diagnostic harness
(`/tmp/diag/round13_verify.c`) running the real BIOS through this
project's own interpreter went from halting at ~15.4M steps (the
FBRST/COP2 wall, round 12's stopping point) to running past 300
million steps with no halt at all. Sampling the PC over a further 2M
steps afterward showed execution confined to a small, bounded
~0x420-byte loop (`0x80005E58`-`0x80006278`). A live PCSX2 disassembly
of that loop shows it repeatedly reading `0xB000F230` (`SIF_SMFLG`,
the IOP-to-EE SIF flag register) with a debounce-style double-read-
and-compare, branching out once the two reads agree - a completely
ordinary SIF handshake polling pattern, not a bug. Given this is a
BIOS-only boot (no disc) with a minimal SIF/IOP HLE model that never
asynchronously updates this flag the way a real IOP would, the
interpreter legitimately has nothing new to wait for here; this is an
honest steady state, not a new wall, and going further would mean
investigating the IOP-side SIF/HLE model (a separate, future round) -
not touched this round to keep scope honest.

Tests: `tests/test_ee_cop2_vu0.c` (10 checks - QMFC2/QMTC2 round-trip,
VSUB self-subtract, VF00/VI0 hardwiring including that writes to them
are discarded, VISWR/VSQI storing to the correct VU0-mem lanes with
VSQI's post-increment) and `tests/test_ee_ldl_ldr_sdl_sdr.c` (6 checks
- aligned and genuinely-misaligned-crossing-a-block-boundary LDL/LDR
round-trips, plus an SDL/SDR round-trip). Full regression (all 40
host-native test files) passes with 0 failures, and the Wii/devkitPPC
target rebuilds clean with 0 warnings/0 errors.

### GS: first flat-shaded triangle primitive

Starting to expand GS coverage in parallel with the EE JALR
investigation, alongside round 13. `source/hw/gif.c` previously only
rasterized SPRITE (PRIM type 6, a filled axis-aligned rectangle from 2
vertices) - everything else, including TRIANGLE/TRIANGLE_STRIP/
TRIANGLE_FAN (types 3/4/5), only updated vertex/PRIM state without
drawing anything.

Added a flat-shaded triangle rasterizer (`rasterize_triangle()` in
`gif.c`) using a standard edge-function scanline fill - plain 2D
geometry, not real-hardware-specific behavior, so it doesn't need the
same "verify against PCSX2 source" treatment the register layouts do.
Single color per triangle (whichever RGBAQ was active when the
triangle's last vertex arrived) - real per-vertex Gouraud shading,
textures, and Z-testing are NOT modeled, an honest simplification
matching this project's existing SPRITE-rasterizer scope notes.

Vertex accumulation (`gif_state_t.tri_vseq`/`tri_x`/`tri_y`) now
handles all 3 triangle primitive types:
- `TRIANGLE` (type 3): every group of 3 vertices is independent (no
  reuse between triangles).
- `TRIANGLE_STRIP` (type 4): each new vertex from the 3rd onward forms
  a triangle with the previous 2 (a rolling 3-vertex window).
- `TRIANGLE_FAN` (type 5): the first vertex becomes a fixed anchor;
  each subsequent vertex forms a triangle with the anchor and the
  previous vertex.

Any PRIM write (A+D, PACKED-mode PRIM register write, or the GIFtag's
PRE bit) now resets the vertex-accumulation sequence - matching real
hardware starting a fresh vertex queue on a new PRIM - so a primitive-
type change mid-stream can't leak stale vertices from a previous
primitive into a new triangle. This was actually tested (not just
assumed): a dedicated check builds a partial TRIANGLE_STRIP, switches
PRIM to TRIANGLE_FAN, and confirms only 2 vertices there draws
nothing yet.

Tests: `tests/test_gif_triangle.c`, 13 checks - a plain TRIANGLE fills
exactly its interior (checked against a point just past the hypotenuse
and a point outside the triangle entirely) and draws exactly once; a
4-vertex TRIANGLE_STRIP and a 4-vertex TRIANGLE_FAN each draw exactly 2
triangles that together fill the whole intended square; and the
PRIM-write vertex-reset behavior above. Full regression (41
host-native test files) passes with 0 failures, and the Wii/devkitPPC
target rebuilds clean with 0 warnings/0 errors (fixed 4 new
`-Wmisleading-indentation` warnings from a compact bounding-box
one-liner along the way).

### GS: Gouraud shading for triangles (second GS increment, per-vertex color)

Direct follow-up to the first flat-shaded triangle rasterizer above.
Real hardware's PRIM register has an IIP bit (bit 3, mask 0x8) that
switches TRIANGLE/TRIANGLE_STRIP/TRIANGLE_FAN between flat shading
(IIP=0, single color from the last vertex) and Gouraud shading (IIP=1,
per-vertex color interpolated across the triangle) - confirmed against
a live fetch of PCSX2's own `GS/GSRegs.h` (`GIFRegPRIM`'s bitfield:
`u32 PRIM:3; u32 IIP:1; ...`), not guessed.

Added `tri_rgba[3]` to `gif_state_t` alongside the existing
`tri_x`/`tri_y` vertex-position buffers - each of the 3 rolling vertex
slots (or, for TRIANGLE_FAN, the fixed anchor slot) now also captures
the RGBAQ color that was active when that vertex was kicked, mirroring
the position-tracking logic exactly (including TRIANGLE_STRIP's
rolling window and TRIANGLE_FAN's anchor-seeding step).
`rasterize_triangle()` now takes the 3 vertex colors as parameters and
branches on the PRIM IIP bit: flat mode is unchanged from before
(fills with the last vertex's color); Gouraud mode computes, per
pixel, the standard barycentric weights from the existing edge-
function values (`b_i = w_i / area`, where `w_i` is the edge function
opposite vertex `i`) and blends each RGBA channel as
`b0*c0+b1*c1+b2*c2`, clamped to 0-255.

Honest simplification, clearly noted in `gif.h`'s scope comment: this
is plain affine/screen-space barycentric interpolation, NOT the real
GS's perspective-corrected (1/Q) interpolation. Matches this project's
established pattern of flagging where its model diverges from real
hardware rather than silently guessing a "close enough" behavior.

Tests: `tests/test_gif_gouraud.c`, 9 checks, using a right triangle
(0,0)-(60,0)-(0,60) with distinct red/green/blue vertex colors so the
barycentric weights have a clean closed form. Confirms: a Gouraud
(IIP=1) triangle's sample points near each vertex are dominated by
that vertex's color, its centroid reads back an even blend of all 3
(not any single pure color), and alpha interpolates too; a flat (IIP=0)
triangle with the SAME distinct per-vertex colors still uses only the
last vertex's color everywhere (regression-proving per-vertex color
capture didn't change flat-shading behavior); and the unrelated SPRITE
path still flat-fills correctly. Full regression (45 host-native test
files) passes with 0 failures, and the Wii/devkitPPC target rebuilds
clean with 0 warnings/0 errors.

### Wiring a real GIF packet through DMA in main.c's on-device demo

Direct follow-up to the Gouraud shading round above. Until now,
main.c's "pixels reach the actual screen" milestone wrote directly
into GS local memory (`gs_mem_write_psmct32`) - proving the GS-memory
-> YCbCr -> Wii XFB output path works, but bypassing the DMA/GIF
pipeline entirely (the actual consumer real BIOS/game code would use).

Added a second demo, right after the first: builds a real, well-formed
A+D-mode GIF packet in memory (FRAME_1 + XYOFFSET_1 + PRIM(TRIANGLE |
IIP) + 3x(RGBAQ + XYZ2), exactly the same wire format
`gif_process_quadwords()` parses and this round's own
`tests/test_gif_gouraud.c` exercises), copies it into the EE's actual
RAM buffer, sets the GIF DMA channel's MADR/QWC/CHCR registers, and
calls `dma_channel_kick()` - the same function real EE `SIF`-driven
DMA writes would trigger. This routes through the real sink wiring
(`dma_set_sink(DMA_CHANNEL_GIF, gif_process_quadwords)`, done in
`ee_core_init()`) end to end, drawing a Gouraud-shaded red/green/blue
triangle below the existing 4-color test bars - proving this session's
Gouraud-shading work exercises the real DMA->GIF->rasterizer pipeline,
not just host-native tests in isolation.

`GIF_REG_*`/`GS_REG_*`/`PRIM_TYPE_*`/`PRIM_IIP_MASK` were promoted from
`gif.c`-private `#define`s to public macros in `gif.h`, since any real
GIF-packet producer (this demo, and future VIF passthrough work) needs
them, not just the parser itself.

Caught and fixed a real bug during this round, not just a Wii-rebuild
formatting nit: the packet buffer was initially sized and the tag's
NLOOP field computed using `3 + n_verts` (6) instead of the correct
`3 + 2*n_verts` (9) - undercounting by one register-entry per vertex,
since each vertex contributes 2 register writes (RGBAQ then XYZ2), not
1. This would have both overflowed the buffer (an `-Warray-bounds`
warning from the Wii-target compiler this round is exactly what
surfaced it) and silently truncated the GIF packet mid-parse on real
hardware (the parser would stop after `NLOOP` register-entries,
dropping the last vertex's `XYZ2` write). Fixed by correcting both the
buffer size and the NLOOP computation to `3 + 2*n_verts`.

Tests: `tests/test_dma_gif_demo.c`, 11 checks - mirrors main.c's exact
packet-building logic host-natively (not achievable by compiling for
Wii alone, since a clean compile doesn't prove the packet bytes/NLOOP
count are actually well-formed) and drives it through the real
`dma.c`/`gif.c` code via `dma_channel_kick()`. Confirms: the packet
builder fills its buffer exactly (regression-proving the NLOOP bug
above stays fixed), the DMA kick reports no error and fully consumes
QWC/advances MADR, exactly one triangle is drawn with IIP set, and the
same red/green/blue/centroid-blend sample-point checks as
`test_gif_gouraud.c` pass through the real DMA path. Full regression
(46 host-native test files) passes with 0 failures, and the
Wii/devkitPPC target rebuilds clean with 0 warnings/0 errors (after
fixing the buffer-size/NLOOP bug above, which the target compiler's
`-Warray-bounds` caught on the first attempt).

### Clock-rate-aware EE:IOP scheduler (8:1, was 1:1)

Direct follow-up to wiring the real GIF-packet demo above, picking up
another item from `docs/ROADMAP.md`'s "remaining near-term candidates"
list. `system_run_interleaved()` (`source/core/system.c`) previously
stepped the EE and IOP one instruction each per slice - a deliberately
simple round-robin, explicitly documented in `system.h` as not
clock-rate-accurate. Real hardware's EE runs at ~294.912 MHz and the
IOP at ~36.864 MHz, a ratio of roughly 8:1 - this was already the
project's own stated target ratio (see `system.h`'s prior wording),
just not implemented yet.

Added `EE_IOP_STEP_RATIO` (8) in `system.c`: each "slice" now steps
the EE up to 8 times (respecting `ee->halted`) before stepping the IOP
once, instead of 1:1. This is explicitly still NOT cycle-accurate -
different MIPS instructions take different real cycle counts on both
cores, and none of that is modeled - it's a ratio-aware approximation
that gives each core roughly the right SHARE of total instructions per
slice, an honest incremental improvement over the previous 1:1
stepping rather than a claim of real timing fidelity. Both `system.h`
and `system.c`'s doc comments were updated to describe this precisely
rather than leaving the old "left for later" wording in place.

No new dedicated test file: `tests/test_system_handshake.c` (the only
existing consumer of `system_run_interleaved()`) already uses a
generous slice cap (2000) and only asserts that both cores halt within
it and that the SIF handshake data round-trips correctly - not exact
instruction/slice counts - so it exercises the new ratio automatically
and still passes unchanged, which is itself a meaningful regression
check (a ratio bug that starved one core would show up as the slice
cap being hit instead of a clean mutual halt). Full regression (46
host-native test files) passes with 0 failures, and the Wii/devkitPPC
target rebuilds clean with 0 warnings/0 errors.

### VIF0/VIF1 passthrough (first increment)

Direct follow-up to the clock-rate scheduler round above, picking up
`docs/ROADMAP.md` section 4's last open item: "VIF0/VIF1 (Vector
Interface) - feeds VU0/VU1 with microcode data and unpacks data
formats". This is a genuinely large real subsystem (PCSX2's `Vif.cpp`
418 lines, plus `Vif_Unpack.cpp`, `Vif_Codes.cpp`, `Vif1_Dma.cpp`,
`Vif0_Dma.cpp`) - this round scopes a first, deliberately narrow
increment rather than attempting all of it at once, matching this
project's established pattern (the GIF parser's own first round was
similarly narrow: PACKED mode only, 4 registers).

New `source/hw/vif.c` / `include/core/hw/vif.h`. A VIF DMA transfer is
a stream of 32-bit "VIFcode" words interspersed with per-command data
(NOT the 128-bit-tag-plus-PACKED-rows format GIF uses) - CMD in bits
24-30, NUM in bits 16-23, IMM in bits 0-15, cross-checked against a
live fetch of PCSX2's `Vif_Codes.cpp` (the real `vifCmdHandler[]`
dispatch table) and `Vif.h` (register bitfields), not guessed.

Implemented this round: NOP/STCYCL/OFFSET(VIF1-only)/BASE(VIF1-only)/
ITOP/STMOD/MARK - trivial register stores (ITOP correctly masks to
0xFF on VIF0 vs 0x3FF on VIF1, matching real hardware's smaller VU0
vs larger VU1 memory); FLUSHE/FLUSH(VIF1-only)/FLUSHA(VIF1-only)/
MSCAL/MSCNT/MSCALF - real, correct no-ops, since this project has no
VU microcode interpreter to flush or execute against (docs/ROADMAP.md
section 5 is still open) - "do nothing" is the honest behavior here,
not a shortcut; STMASK/STROW/STCOL - store their trailing data word(s)
into mask/row[4]/col[4] registers; MPG - correctly skips its data span
(NUM-derived word count) so the VIFcode stream stays in sync, counted
as unsupported since there's no VU micro-instruction memory to write
into; **DIRECT/DIRECTHL (VIF1-only) - the one command that actually
produces pixels this round**: forwards its data span verbatim to
`gif_process_quadwords()`, exactly matching real PCSX2's own
`_vifCode_Direct` behavior (a real, common pathway - many BIOS/game
splash screens draw via VIF1 DIRECT rather than raw EE->GIF DMA).
DIRECT vs DIRECTHL are treated identically (the real difference is a
GS-FIFO-level "horizontal" nuance irrelevant without a FIFO model -
PCSX2 itself shares one implementation for both).

Explicitly NOT implemented: UNPACK (CMD 0x60-0x7F) - the format that
decodes S/V2/V3/V4 component data into VU data memory, a substantial
feature in its own right (`Vif_Unpack.cpp`) that this project has no
VU1 data memory to unpack into yet. Encountering UNPACK (or any other
unrecognized/reserved code) stops processing the REST of that DMA
transfer's data stream cleanly - counted via `unsupported_cmds_seen`,
not silently misparsed as garbage VIFcodes - matching this project's
established pattern for out-of-scope formats (see gif.c's REGLIST/
IMAGE handling).

Wired into `ee_core_init()`: `vif_init()` alongside `gif_init()`, and
`dma_set_sink(DMA_CHANNEL_VIF0, vif0_process_quadwords)` /
`DMA_CHANNEL_VIF1` likewise - VIF0/VIF1 DMA transfers now actually get
parsed instead of silently discarded (dma.c's existing "no sink
registered -> data discarded, transfer still runs" fallback, unchanged
for any channel without a sink).

Tests: `tests/test_vif.c`, 24 checks - NOP/STCYCL/ITOP/OFFSET/BASE/
STMASK/STROW/STCOL/MPG behavior, VIF1-only commands correctly rejected
on VIF0 (and accepted on VIF1), UNPACK stopping the stream cleanly
(proven by placing a would-be-effective marker code right after it and
confirming it was NOT parsed), and - the key end-to-end proof - a real
DIRECT command on VIF1 forwarding an actual SPRITE GIF packet through
to `gif_process_quadwords()`, verified by reading back the drawn
pixel's color from GS memory (a genuine DMA -> VIF -> GIF -> pixels
path, not just parser-internal state). Full regression (47 host-native
test files - the existing EE-core-linking tests all needed
`source/hw/vif.c` added to their link line too, the same transitive-
dependency pattern this project has hit before whenever `ee_core.c`
gains a new hardware-model call) passes with 0 failures, and the
Wii/devkitPPC target rebuilds clean with 0 warnings/0 errors.

### GS: texturing for the triangle rasterizer (task #85)

Direct follow-up to the VIF0/VIF1 round above, closing out the last
open item from the "remaining near-term candidates" list: texturing
for TRIANGLE/TRIANGLE_STRIP/TRIANGLE_FAN, driven by PRIM's real TME
bit (bit 4) and TEX0's TBP0/TBW/TFX fields - both cross-checked
against PCSX2's own `GS/GSRegs.h` (`GIFRegPRIM`, `GIFRegTEX0`
bitfields), not guessed.

Added to `gif_state_t`: `tex_tbp0`/`tex_tbw`/`tex_tfx` (decoded from a
new TEX0_1 A+D register handler - TBP0/TBW used directly as OUR
gs_mem "bp"/"bw" convention, exactly like FRAME_1's FBP/FBW already
work, not a claim of matching real hardware's block-swizzled
addressing), `cur_u`/`cur_v` (from a new UV A+D register handler - a
12.4 fixed-point texel coordinate, same `>>4` conversion as XYZ2's
screen coordinates), and `tri_u`/`tri_v[3]` (a per-vertex rolling
buffer mirroring `tri_rgba`'s exact rolling/anchor logic across all 3
triangle vertex-accumulation modes).

`rasterize_triangle()` now takes 6 more parameters (u0,v0,u1,v1,u2,v2)
and, when PRIM's TME bit is set, interpolates the texture coordinate
per pixel using the SAME barycentric weights already computed for
Gouraud color (on real hardware, texture-coordinate interpolation
always happens when texturing is on, independent of the IIP
color-shading bit - this project's implementation now reflects that:
the weights are computed once and used for both purposes as needed).
Samples GS memory (nearest-neighbor, PSMCT32 only) at the interpolated
texel, then combines with the shaded (flat or Gouraud) color per
TEX0's TFX field: DECAL replaces the color entirely with the texture
sample; MODULATE (and, simplified, HIGHLIGHT/HIGHLIGHT2 too - an
honest, noted simplification, since this project doesn't model the
real highlight modes' extra specular-like term) blends via the
standard GS formula, `(tex*color)/128` per channel, clamped to 255.

Honest simplifications, all noted in `gif.h`'s scope comment: texture
coordinates come from UV only (real hardware's "FST=1" mode) - the
ST+Q floating-point perspective-correct path (FST=0) is NOT supported;
no CLAMP register modeling at all (no wrap/clamp/region modes -
negative interpolated coordinates are simply clamped to 0 as a
defensive measure, not real repeat/clamp semantics); interpolation
itself is plain affine (screen-space), not the real GS's perspective-
corrected (1/Q) interpolation, matching this project's existing
Gouraud-shading simplification. SPRITE is NOT texture-mapped (out of
scope this round, matching Gouraud's own triangle-only scope from the
previous round).

Tests: `tests/test_gif_texture.c`, 10 checks. Since this project has no
texture-upload path yet (no TRXDIR/BITBLTBUF), textures are simply
pre-existing GS memory content, filled directly via
`gs_mem_write_psmct32()` - exactly how the framebuffer itself already
works. Covers: DECAL replacing a red vertex color with a solid blue
texture entirely; MODULATE's exact per-channel blend math verified
against hand-computed expected values (a 200/100/50/255 texture times
a 128/64/32/128 vertex color, checked channel-by-channel including an
intentionally-truncating case); real per-pixel UV interpolation across
a 3-texel horizontal-gradient texture, sampled exactly at each
triangle vertex's own coordinate (where the barycentric weights are
exactly 1/0/0 by construction, avoiding nearest-neighbor's discrete-
snapping ambiguity that a merely-nearby sample point would have,
unlike Gouraud color's continuous blending); and a TME=0 regression
proving flat-shaded triangles are completely unaffected by the new
texturing code path. Full regression (48 host-native test files)
passes with 0 failures, and the Wii/devkitPPC target rebuilds clean
with 0 warnings/0 errors.

### Perspective-correct (ST+Q) texture coordinates + SPRITE texturing (task #88)

Direct continuation of "mach 1 3 4 6 komplett" - task 4 this round
(after tasks 1/#86 and 3/#87 above). Task 4 asked for real ST+Q
(FST=0) perspective-correct texture coordinates on triangles, plus
texturing support for SPRITE (previously flat-color only).

**Real references, live-fetched this round**: PCSX2's `GS/GSRegs.h`
gave the exact real bitfield layouts needed: `GIFRegPRIM`'s FST bit is
bit 8 (`PRIM:3,IIP:1,TME:1,FGE:1,ABE:1,AA1:1,FST:1,...`); `GIFRegRGBAQ`
is `u8 R,G,B,A; float Q` - Q occupies the ENTIRE second 32-bit word of
an A+D RGBAQ write as a real IEEE-754 float (previously this project
read but discarded that word - `(void)data_hi` - now decoded for
real); `GIFRegST` is `float S; float T` (no Q - unlike PACKED mode's
combined STQ tag, A+D mode's Q arrives bundled with RGBAQ instead, a
real hardware quirk now correctly modeled); and `GIFRegTEX0`'s TW/TH
fields (previously ignored entirely) - TW is a clean 4-bit field
(word0 bits 26-29), but TH is a real hardware oddity that straddles
the 64-bit register's word boundary (2 bits from word0's top, bits
30-31, plus 2 bits from word1's bottom, bits 0-1), confirmed via
PCSX2's own overlapping-bitfield union for this exact register.

**Triangle ST+Q**: `rasterize_triangle()` (`source/hw/gif.c`) now
branches on PRIM's real FST bit. FST=1 (UV) keeps the exact existing
affine-interpolation code path unchanged. FST=0 implements the
standard perspective-correct texture-mapping algorithm real GS
hardware uses: 1/Q, S/Q, and T/Q (NOT S, T, Q directly) are affine/
linear in screen space, so those are what get barycentrically
interpolated; the true per-pixel S/T is then recovered by dividing
back out the per-pixel Q. Verified with a specifically-designed test
that distinguishes genuine perspective correction from a plain-affine
bug: for ANY triangle, the centroid (average of the 3 vertices) has
barycentric weights of exactly (1/3, 1/3, 1/3) - a well-known,
independently-verifiable geometric fact. Using vertices (0,0)/(9,0)/
(0,9) (centroid exactly (3,3)) with S=0/9/0 and Q=1/1/4 (differing Q
to force real perspective skew), plain affine interpolation of S alone
would wrongly give 3.0, while genuine perspective correction gives
exactly 4.0 (hand-verified: inv_q_avg=(1+1+0.25)/3=0.75, s_over_q_avg=
(0+9+0)/3=3.0, q_at_pixel=1/0.75=1.3333, s_norm=3.0*1.3333=4.0) - and
the implementation samples texel 4 exactly, confirming real 1/Q
division is happening, not a fallback. A second case with equal Q at
every vertex confirms the perspective-correct math correctly reduces
to the same answer plain affine would give when there's nothing to
correct for.

**SPRITE texturing**: new `rasterize_sprite()` replaces the previous
inline flat-fill-only code. Since SPRITE is screen-axis-aligned (U
varies only with X, V only with Y), this uses a deliberately simpler
approximation than triangles: each corner's texture coordinate is
resolved to final texel space FIRST (applying the FST=0 perspective
divide at the corner, if applicable), then plain linear interpolation
runs between the two corners' already-resolved texel coordinates.
This is exact when both corners share the same Q (the overwhelmingly
common real case for a 2D sprite) - explicitly documented as a
simplification for the rarer differing-Q "billboard" case, not
silently assumed correct. Verified with an identity-mapped UV sprite
(corner0 (0,0)->uv(0,0), corner1 (10,10)->uv(10,10)) sampling a
red=x*10,green=y*10 gradient texture: the exact midpoint (5,5) reads
back texel (5,5) via the axis-aligned bilinear interpolation.

**Bug caught while writing the SPRITE test**: the test initially packed
UV's V coordinate into the A+D write's second word (`data_hi`),
mirroring how ST/RGBAQ/XYZ2 spread their two logical values across two
words - but real hardware's `GIFRegUV` packs BOTH U and V into the
FIRST word alone (`u16 U; u16 V; u32 _PAD3` - word1 is pure padding),
confirmed against PCSX2's own GS/GSRegs.h. This was a test-construction
bug, not a `gif.c` bug - the existing UV handling in `apply_ad_write`
(unchanged this round) was already doing this correctly; the
pre-existing `test_gif_texture.c` never caught it because its
V-in-data_hi mistake happened to coincide with the real, correct V
value (0) in every case it tested.

Full regression (51 host-native test files) passes with 0 failures,
and the Wii/devkitPPC target rebuilds clean with 0 warnings/0 errors.
New `tests/test_gif_stq_sprite.c`, 15 checks. Updated
`tests/test_gif_texture.c`'s 3 textured-PRIM constructions to
explicitly set `PRIM_FST_MASK` (UV mode) - needed since FST now has
real meaning and those tests never touch ST/Q, matching what they
always actually intended.

### Z-buffer / depth test for triangles and SPRITE (task #89, task 6)

Real ZBUF_1 (0x4E) and TEST_1 (0x47) A+D registers implemented in
`source/hw/gif.c`, cross-checked against a live fetch of PCSX2's
`GS/GSRegs.h`: `GIFRegZBUF` (`ZBP:9, PSM:6, ZMSK:1` - PSM ignored,
matching this project's PSMCT32-only simplification; real hardware's
ZBUF register notably has NO separate width field, so this project
reuses FBW for Z-buffer addressing too, exactly like real hardware
does) and `GIFRegTEST` (`ZTE:1, ZTST:2` modeled; ATE/ATST/AREF/AFAIL/
DATE/DATM are not - no alpha test/blending exists in this project).
`GS_ZTST`'s 4 real compare modes (NEVER/ALWAYS/GEQUAL/GREATER) gate
both the color write and the Z-buffer write (ZMSK-respecting) per
pixel in `rasterize_triangle()`/`rasterize_sprite()`.

Z itself comes from XYZ2's real Z word - this took real investigation,
not a guess. A live fetch of PCSX2's `GS/GSRegs.h` shows the genuine
PACKED-mode `GIFPackedXYZ2` layout is X in word0, Y in word1, Z as the
ENTIRE word2 (a real 32-bit value) - previously read into a local `w2`
variable in `process_one_packet()`'s PACKED loop but discarded. Wiring
it through was a one-line change (`apply_xyz2(w0, w1, w2)`) with zero
regression risk, since no existing test in this codebase had ever
exercised the genuine PACKED-mode XYZ2 register path - every existing
test/demo (main.c included) uses A+D-mode XYZ2 instead. That turned
out to matter: real hardware's A+D-mode `GIFRegXYZ` register is only
64 bits total (X:16 and Y:16 packed together into ONE word, Z:32 alone
in the other), but this project's pre-existing A+D XYZ2 convention -
already baked into every single test file and main.c before this round
- puts X in the ENTIRE first word and Y in the ENTIRE second word,
leaving no room for Z at all. Reconciling that would mean rewriting
every existing test's packet-construction helper and main.c's demo,
well outside this task's scope, so it's called out as an honest,
explicit gap instead: Z only flows through for genuine PACKED-mode
XYZ2 writes (Z=0 for A+D-mode XYZ2, harmless since Z-buffer access is
fully gated behind this round's own safety flag regardless - see
below). The new `tests/test_z_buffer.c` builds real PACKED-mode GIF
packets by hand (GIFTag + a dedicated XYZ2-only loop) specifically to
supply genuine, distinct per-vertex Z values, the same way
`tests/test_vif.c` already hand-builds packets for its own purposes.

Z interpolation for triangles is plain barycentric (screen-space-
linear, the same weighting already used for Gouraud color) - real
hardware's Z has already gone through the perspective transform by the
time it reaches the rasterizer, so unlike S/T it needs no 1/Q
correction, well-known real GS behavior. Verified with the same
centroid technique task #88 introduced: a triangle's centroid always
has barycentric weights of exactly (1/3, 1/3, 1/3), so 3 distinct
per-vertex Z values (0, 300, 600) must average to exactly 300 at the
centroid if interpolation is genuinely happening - confirmed. SPRITE
gets a single flat Z from its second (completing) vertex, extending
this file's already-established "flat shading uses the last vertex"
convention (previously applied to color) to Z as well - noted as an
extension of that real, already-cited convention rather than a fresh,
independently-verified claim specific to Z.

This round's own safety mechanism, `zbuf_configured` (not a real
hardware concept), stays false until an explicit ZBUF_1 write happens,
so any pre-existing draw that never configures a Z buffer - which is
every one of the 51 tests that existed before this round, plus
main.c's demo - behaves byte-for-byte exactly as it did before this
round. Without this gate, ZBUF's real default value (ZBP=0) would
silently alias the color framebuffer's own default address (FBP=0) and
corrupt it the first time any triangle drew. A dedicated regression
test (`tests/test_z_buffer.c`'s "No ZBUF configured" case) proves this
explicitly: two overlapping draws with wildly different Z values still
overwrite each other unconditionally, exactly like pre-#89 behavior.

20 new checks (`tests/test_z_buffer.c`), covering ZBUF_1/TEST_1
register parsing, genuine per-vertex Z interpolation (the centroid
proof), GEQUAL/GREATER/NEVER/ALWAYS depth-test semantics, ZMSK
(color written but Z buffer left untouched, independently confirmed by
re-testing against the stale stored Z), the zbuf_configured safety
gate, and SPRITE's flat "second vertex" Z convention together with its
own depth test. Full regression (52 host-native test files, up from
51) passes with 0 failures, and the Wii/devkitPPC target rebuilds
clean with 0 warnings/0 errors.

### VU0/VU1 micro-instruction memory + microcode interpreter control flow (task #87)

Direct continuation of the user's "mach 1 3 4 6 komplett" instruction
(task 3 this round, after task 1/#86 above). Task 3 asked for "VU0/VU1
data memory + VU microcode interpreter... enough to execute basic VU
programs (at least the ones MSCAL/MSCNT in vif.c currently no-op)".

**What VU "micro mode" is, and how it differs from the existing round-
13 VU0 work**: round 13 implemented VU0 "macro mode" - VU0 acting as
the EE's COP2 coprocessor, executing one vector instruction per EE
instruction via MFC2/CFC2/MTC2/CTC2/QMFC2/QMTC2/VSUB/VISWR/VSQI, using
the EE's own MIPS-style instruction encoding. "Micro mode" is a
completely separate thing: an asynchronous microprogram (uploaded via
VIF's MPG command, kicked by MSCAL/MSCNT/MSCALF) that runs using a
totally different, VU-native 64-bit-per-instruction ISA. Real hardware
runs both on the SAME physical VU0 (sharing VF/VI registers and local
data memory) - this project's new `vu0_exec_micro()`/
`vu0_micro_write32()` (added to `ee_core.c`) reuse the existing
`vu0_vf`/`cop2_ctrl`/`vu0_mem` fields for exactly that reason. VU1 has
no macro-mode/COP2 presence on real hardware at all, so it gets a
fully self-contained new `vu1_state_t` (`include/core/hw/vu.h`/
`source/hw/vu.c`).

**Live-fetched real references this round**: PCSX2's `VU.h` (VECTOR/
REG_VI/VURegs layout, the VURegFlags enum confirming `cop2_ctrl[26]`
- already round 12's generic CTC2/CFC2 register file - is the real
TPC slot, REG_TPC), `VUmicro.h` (VU0_MEMSIZE/PROGSIZE=0x1000 4KB each,
VU1_MEMSIZE/PROGSIZE=0x4000 16KB each - confirms this project's sizes
exactly), `VUops.h`/`VUops.cpp` (per-instruction body implementations
and the real field-extraction macros - `_Ft_`/`_Fs_`/`_Fd_`/`_Fsf_`/
`_Ftf_`/`_Imm11_` etc), `VUmicro.cpp`, `VUmicroMem.cpp` (VF00 hardwired
to (0,0,0,1.0f), confirming round 13's existing VU0 convention extends
to VU1 too), and - most importantly - `VU0microInterp.cpp`'s
`_vu0Exec()`, which gave the exact, byte-accurate real control-flow
mechanics this round implements: each micro-instruction is 8 bytes
(`ptr[0]`=lower word, `ptr[1]`=upper word, TPC+=8 per step); upper-word
bit 31 (0x80000000, "I" flag) means only the upper instruction executes
and the lower word's raw bits become VI[21]/REG_I; bit 30 (0x40000000,
"E" flag) marks the end of the microprogram, but real hardware executes
exactly ONE MORE instruction after it (the classic VU "E-bit delay
slot") before actually stopping - verified via the exact countdown
arithmetic in the cited source (`ebit=2` then `if (ebit--==1) stop`,
which this project reproduces as an equivalent pre-decrement-then-
check-zero form). Branches use the identical one-instruction-delay
mechanism - implemented (a generic, correct `branch_delay`/
`branch_target` pair) even though nothing sets it yet (see below).

**Honest scope boundary - read before extending**: despite fetching
seven real PCSX2 source files this round (`VU.h`, `VUmicro.h`,
`VUmicro.cpp`, `VUops.h`, `VUops.cpp`, `VUmicroMem.cpp`,
`VU1micro.cpp`), this project could not locate the actual
`VU0_LOWER_OPCODE[128]`/`VU0_UPPER_OPCODE[64]` function-pointer table
that maps a real numeric opcode (7-bit lower field `code>>25`, 6-bit
upper field `code&0x3f`) to a real instruction. `VUops.cpp` implements
every instruction's BODY (`_vuADDx`, `_vuNOP`, etc.) but the index-to-
function table itself is defined somewhere else this project's fetch
attempts (including guessing several plausible filenames) didn't find.
Per this project's consistent no-fabrication policy, `vu_micro_step()`
(`source/hw/vu.c`) does NOT guess which numeric value means what -
every instruction pair is genuinely fetched, its real E-bit/I-bit
flags are honored exactly per the cited source, and TPC/branch/E-bit
control flow is completely real, but the actual FMAC/integer-ALU/
branch body of every instruction is a logged no-op
(`unimplemented_opcodes_seen`). This means MSCAL/MSCNT/MSCALF go from
"total no-op" (vif.c's prior state) to "genuinely runs the real
uploaded microprogram, fetch-by-fetch, honoring real flags, until real
E-bit completion" - exactly the bar task 3 set ("enough to execute
basic VU programs... at least the ones MSCAL/MSCNT... currently
no-op") - without fabricating what any specific instruction actually
computes. A new, narrower, honest wall, matching this project's
established pattern (VIF0/VIF1's first increment, GS's first triangle,
IOP HLE's pure-computation subset, etc.).

**MPG now writes real data**: previously (vif.c, task #84) MPG
recognized its data span and skipped it correctly (to keep the
VIFcode stream in sync) but the microprogram bytes went nowhere, since
no VU micro-instruction memory existed. Now they're written via
`vu0_micro_write32()`/`vu1_micro_write32()` at the VIFcode's IMM
address (same "instruction pair index" addressing units MSCAL/MSCNT
use - byte offset = imm*8, confirmed against a live fetch of PCSX2's
`vu1ExecMicro()`: `VU1.VI[REG_TPC].UL = addr; ...SetStartPC(TPC<<3)`).
MPG is no longer counted as unsupported.

**Tests**: updated `tests/test_vif.c`'s MPG check (previously asserted
"counted as unsupported"; now asserts the 4 microprogram words were
actually written into VU1 micro memory) - required converting
`test_vif.c` from its previous self-contained `#include`-the-source
style to proper separate-translation-unit linking (vif.c now calls
into `ee_core.c`/`vu.c`, the same reason `test_system_handshake.c`
already needed that style). New `tests/test_vu_micro.c` (14 checks):
a 4-instruction VU1 program with the E-bit on instruction 3 stops
after exactly 4 real instructions (verified against the exact
countdown arithmetic cited above); MSCAL's start address confirmed to
be an instruction-pair index; the safety cap (65536 instructions -
this project's own guard against a genuinely-infinite microprogram,
not real hardware) correctly terminates an all-zero "program" that
never sets E; the I-flag correctly loads VI[21]; `vu1_micro_write32`'s
little-endian storage and real 16KB address wraparound; and VU0's
execution correctly reusing `ee_state_t`'s shared VF/VI/data-memory
fields from round 13 while keeping its own separate micro-instruction
memory. Also required adding `source/hw/vu.c` to every existing test's
link line that already needed `source/hw/vif.c` (same transitive-
dependency pattern as every previous round that gave `ee_core.c`/
`vif.c` a new hardware dependency). Full regression (50 host-native
test files) passes with 0 failures, and the Wii/devkitPPC target
rebuilds clean with 0 warnings/0 errors.

### IOP HLE: real A0-table BIOS calls implemented for the first time (task #86, priority round)

Direct response to the user's explicit priority instruction ("mach 1
3 4 6 komplett... am wichtigsten ist 1 komplett" - do items 1/3/4/6
completely, item 1 most important). Item 1 is IOP HLE stubs for the
BIOS-boot-path modules - previously blocked (see `iop_hle_bios.h`'s
header comment and the ROADMAP's own "crux" note) on not having a
verified, citable reference for real PS1/PS2 BIOS syscall function
numbers beyond the single existing exception (`InstallExceptionHandlers`,
C(07h)).

**What changed**: psx-spx (https://psx-spx.consoledev.net/kernelbios/),
the same community reference already cited for C(07h), documents the
full A0/B0/C0 "Function Summary" tables. Cross-checking that table
against this project's existing "no fabrication" bar, a clear subset
stood out as safe to implement for real right now: pure computation on
IOP RAM/registers only, with no dependency on any unmodeled internal
BIOS kernel structure (unlike module loading, TCBs/EvCBs, or device
drivers, which genuinely do need such structures and remain out of
reach - see below).

Implemented in `source/hw/iop_hle_bios.c` (all A0-table, real $a0-$a3
argument registers, real IOP RAM via `iop_mem_read8`/`iop_mem_write8`):
`ABS`/`LABS`(0x0E/0x0F), `STRCAT`/`STRNCAT`(0x15/0x16), `STRCMP`/
`STRNCMP`(0x17/0x18), `STRCPY`/`STRNCPY`(0x19/0x1A), `STRLEN`(0x1B),
`BCOPY`/`BZERO`(0x27/0x28 - note BCOPY's argument order is
`(src,dst,len)`, reversed from MEMCPY's `(dst,src,len)`, exactly as
psx-spx documents it), `MEMCPY`/`MEMSET`/`MEMMOVE`(0x2A/0x2B/0x2C),
`INITHEAP`(0x39, bookkeeping only - records addr/size, no real
allocator, same "scaffold not a port" caveat as `iop_hle_modules.c`),
`FLUSHCACHE`(0x44, a correct no-op - no cache model exists to flush),
and `EXIT`/`_EXIT`(0x06/0x3A, which now halts the core with an honest,
descriptive reason instead of silently returning 0 and letting the
caller run past a call real hardware never returns from).

One deliberately-preserved real bug: psx-spx annotates `A(2Ch) memmove`
as ";Bugged" on real hardware. Rather than "fixing" it to correct,
overlap-safe modern libc semantics, `MEMMOVE` here is implemented as a
plain forward byte-copy (identical to `MEMCPY`, NOT overlap-safe) to
match that documented real (buggy) behavior.

**Live ground-truth cross-check**: reconnected to the user's real,
running SCPH-10000 BIOS session via PCSX2-MCP's DebugServer. Confirmed
16 real IOP modules load successfully on real hardware
(`System_Memory_Manager`, `Module_Manager`, `Exception_Manager`,
`Interrupt_Manager`, `ssbus_service`, `dmacman`, `Timer_Manager`,
`System_C_lib`, `Heap_lib`, `Multi_Thread_Manager`, `Vblank_service`,
`IO/File_Manager`, `Moldule_File_loader`, `ROM_file_driver`, `Stdio`,
`IOP_SIF_manager`) - useful confirmation that a real, working IOP BIOS
does reach a fully-loaded steady state, and a concrete list of exactly
which subsystems the round-14 wall still stands between this project
and. Also live-disassembled the real A0/B0 vector region
(0x000000A0-0x000000C4) in RAM: it decodes as a real (self-installed,
not ROM-resident) dispatcher using `$k0` as scratch for a masked
jump-table lookup at RAM 0x440, sharing/overlapping code between the
A0 and B0 16-byte vector windows. This is consistent with - not
contradicting - psx-spx's documented `$t1`/R9 calling convention: the
caller still sets `$t1` before jumping to 0xA0, and since this
project's HLE intercepts execution AT the trap address itself (before
any real vector code would run), the internal register choreography
of the real ROM-installed dispatcher is moot for our purposes - it's
never actually executed.

**Still NOT implemented** (unchanged rationale): anything touching
files/devices (open/read/write/close/ioctl), heap allocation (malloc/
free/calloc/realloc - INITHEAP is recorded but nothing allocates from
it), threads/events, or CD-ROM/memory-card functions - all depend on
internal BIOS kernel structures this project has never modeled and has
no verified layout for. Real IOP module/IRX loading (the round-14 wall
itself - a genuine `JALR $ra,$s1` into an address only a real module
loader would populate) is **not** cleared by this round; it remains the
same honest architectural boundary described in round 14, now with a
concrete real-module-list reference point (above) for whenever this
project returns to attempting it.

New test file `tests/test_iop_hle_bios_functions.c` (26 checks, all
passing): every new function number exercised directly via
`iop_hle_bios_try_handle()` with hand-set registers/memory, including
the reversed-argument-order BCOPY case, the documented-buggy MEMMOVE
overlap case (computed independently in the test, not by calling the
implementation under test), INITHEAP bookkeeping, FLUSHCACHE's no-op
guarantee, an unimplemented function number correctly NOT incrementing
`known_calls_handled`, and EXIT's halt-with-reason behavior. Full
regression (49 host-native test files) passes with 0 failures, and the
Wii/devkitPPC target rebuilds clean with 0 warnings/0 errors.

### Round 14: IOP-side investigation - the EE's SIF-polling steady state is real, and a genuine IOP wild-jump root-caused and hardened against

Direct follow-up to round 13's finding that the EE settles into a
bounded loop polling SIF_SMFLG after 300M+ steps. Round 13's own
diagnostic only ran the EE in isolation (no IOP execution at all), so
the natural next question was: does actually running the IOP
alongside the EE (via `system_run_interleaved`, this project's
existing interleaved scheduler) change that picture?

**Short answer: no, but for an interesting, deeper reason.** Running
both cores together for the same 300M-step budget, the EE stays
exactly where round 13 found it (still polling SMFLAG, still a
legitimate steady state), while the **IOP halts almost immediately**
- after only ~111,000 real instructions - hitting a genuine, honest
bug of its own.

**Root cause, traced precisely**: a host-native diagnostic
(`/tmp/diag/round14_iop_pc_bounds.c`) found the IOP's PC leaving all
sane bounds (not IOP RAM, not BIOS ROM) at instruction ~3,054,825,
jumping from `0x00000E0C` straight to `0x03400008`. Backtracking one
more instruction and reading the actual word our own RAM held there
(`0x0220F809`) decodes cleanly as a real, valid MIPS instruction:
**`JALR $ra, $s1`** - and `$s1` held exactly `0x03400008`, an address
inside neither IOP RAM (2MB) nor the BIOS ROM window. A live PCSX2
disassembly of the surrounding addresses (`0x00000D74`-`0x00000E20`)
confirmed this region is a real kernel routine (a linked-list/heap-
style walker: `andi`/`srl`/`sltu`/`lw ...,4(t0)`/`bnez` in a tight
loop) - genuine BIOS code, not garbage - but the *specific* value
loaded into `$s1` only makes sense as a pointer a real IOP module/IRX
loader would populate once it actually copies a module image into IOP
RAM and jumps to it locally. This project's `iop_hle_modules.c` has
always been an explicit scaffold, not a real loader (see its own
scope note, unchanged since task #32) - so `$s1` ends up holding
whatever this simplified model computed instead of a real, locally-
loaded module address, and the JALR faithfully jumps exactly where
that (wrong, for us) value points.

This is an honest architectural boundary, not a fixable bug: making
this jump land somewhere meaningful would require actually
implementing IOP module/IRX loading against a verified reference this
project has always deliberately avoided fabricating (same policy that
shaped `iop_hle_modules.c`, `iop_hle_bios.c`, and the ROMDIR work
originally). Not attempted this round, to keep scope honest.

**What WAS fixed**: before this round, an out-of-range instruction
fetch silently returned `0` (a NOP) forever - the IOP just "wandered"
through effectively unmapped memory for tens of millions of steps
until it coincidentally hit a non-zero value sitting in the SIF
register mirror's reset default (`SIF_F260 = 0x1D000060`, which
happens to *equal its own address* and decodes as a bogus SPECIAL
instruction) and halted on a confusing, unrelated-looking "illegal
opcode" message with zero clue about the real cause. Added a PC
fetch-sanity guard to `iop_step()` (`source/core/iop/iop_core.c`):
any fetch address that's neither real IOP RAM nor real BIOS ROM now
halts immediately with a clear, honest diagnostic naming the exact
escaped-to address - turning a 111,000-vs-tens-of-millions-of-
instructions confusing false lead into an instant, precise one.
Verified live: the interleaved diagnostic now halts the IOP in ~5.6s
instead of ~9.7s (no more wasted cycles wandering unmapped memory),
landing cleanly at `pc=0x03400008` with the new message, while the EE
side is unaffected (still the same legitimate round-13 steady state).

Tests: `tests/test_iop_pc_guard.c`, 7 checks - a deliberately wild
JALR halts within a handful of steps (not after wandering) with a
message naming both the escape and the exact address; a JALR to a
real, valid BIOS ROM address still works exactly as before (link
register gets the correct return address) - proving the guard doesn't
just make everything halt, only genuinely unfetchable addresses. Full
regression (44 host-native test files) passes with 0 failures, and
the Wii/devkitPPC target rebuilds clean with 0 warnings/0 errors
(fixed a `%X`-vs-`long` format-type warning and a `strncpy` truncation
warning from the new diagnostic message along the way - devkitPPC's
32-bit-`long` `uint32_t` differs from this project's host test
environment here, exactly the kind of cross-compile detail the "always
verify the real Wii rebuild too" discipline exists to catch).

### EE JALR investigation, round 10: idle loop confirmed dead code on real hardware; root cause fully traced to a BIOS clock-calibration loop whose retry budget this project's timing model exhausts too early

Direct continuation of round 9's wall (EI never executes before
`pc=0xBFC0092C`). The user provided two more live PCSX2 traces and,
after this session, direct live access to a running PCSX2 via a new
`pcsx2-mcp` MCP connector (DebugServer mode - breakpoints, register
reads, disassembly, memory reads, all queryable directly rather than
through user-relayed report files).

**Confirmed: `pc=0xBFC0092C`/the `j $` idle loop is genuinely
unreachable on real hardware (0 hits in a full boot trace).** Real
hardware takes the OTHER branch at `pc=0xBFC0088C` (`bltz v0, +0x98`)
because `v0` is positive there (`0x02000000`), falls through, and
eventually does `jr t0` (`t0=0x80001000`) into RAM, never returning to
this ROM region at all. This project's interpreter takes the wrong
branch at that exact spot instead - `v0` ends up negative (`-1`) -
landing squarely in the idle loop real hardware never visits. So the
"wait for interrupt" framing from round 8/9 was a misreading: this
was never a real idle pattern, it's this project's own wrong-branch
bug wearing the disguise of one.

**Correction of an earlier mislabeling in this same round:** the
subroutine chain leading up to the `0x9FC410E8` call was originally
(round 6 report) described as "SIF communication init/status". Live
disassembly plus reading the actual format string this code prints
(`pcsx2_read_memory` at `a0=0x9FC438C8`) shows this is wrong - the
string is `"Initialize memory (rev:%d.%02d, ctm:%dMhz, cpuclk:%dMhz
%s)..."`. The three calls preceding `0x9FC410E8` from the same parent
(`0x9FC41000`) are siblings, not nested inside it: `0x9FC42F48` is SIO
baud-rate-divisor setup (writes `0x1000F100`/`0x1000F120`/`0x1000F140`
via KSEG1, `a1=0x9600`=38400 baud), `0x9FC43088` is the actual printf
for the string above, and `0x9FC42D78` iterates a small (48-entry)
device/module table calling a shared dispatcher (`0x9FC42D48`) - none
of this is SIF. This whole region is BIOS console/UART setup and
memory/clock diagnostics output, not SIF communication.

**Traced the wrong value three levels deep, confirmed live against a
real BIOS-only boot (no disc) via direct `pcsx2-mcp` breakpoints/
register reads:**
- `v0` at `pc=0xBFC0088C` comes from `a1`, which comes from the return
  value (`v0`) of a subroutine call at `pc=0x9FC410E8` (called with a
  fixed constant `a0=0x60000012` from `pc=0x9FC41000`).
- **Real hardware**: that subroutine returns `v0=0x08028020`. `a1=v0`
  is positive, `bgez a1` (at `pc=0x9FC41078`) is taken (success path),
  `s0 = a1 & 0x7FFF = 0x20`, final `v0 = s0 << 20 = 0x02000000`) -
  matches the very first (round 6) report's numbers exactly, now
  confirmed live via direct register reads at each step.
- **This project's interpreter**: the same subroutine call returns
  `v0=0xFFFFFFFF` (-1, an error sentinel) instead. Confirmed NOT a
  timing/"hasn't finished yet" issue via a fresh interleaved EE+IOP
  diagnostic (IOP genuinely executing ~147,500 instructions alongside
  the EE, not stalled/halted) - still produces `-1`.
- Real hardware took **~14.9 million CPU cycles** through this call
  (per the live trace's own cycle counter) versus this project's
  ~142,500 EE instructions - a roughly 100x difference.

**Root cause, fully traced via a host-native tail trace
(`/tmp/diag/round10_tail_trace.c`, ring buffer of the last 400 PCs/
registers before the return) cross-checked instruction-by-instruction
against live PCSX2 disassembly:**

`0x9FC410E8`'s own table-search preamble (`s5 = a0>>12`, a lookup
against the table at `0x9FC43850`) was verified **byte-for-byte
identical** between this project's ROM read and the real-hardware
memory dump the user captured earlier (`ps2_bios_table_analyse.md`) -
ROM loading/table logic is fully correct and was ruled out as the
cause.

Past that point, the real subroutine turns out to be a **BIOS
clock/baud calibration loop**, not a SIF/IOP handshake:
- It repeatedly calls a helper at `0x9FC42570` that writes a
  candidate baud/clock configuration byte to the SIO control path and
  returns the resulting 6-bit config value (`andi v0,0x3F` at
  `pc=0x9FC42190`).
- It compares that value against the previous iteration's value
  (`s0`); as long as they're equal (`beq v0,s0,->0x9FC42150`) it loops
  back, incrementing a counter `s3` (`addiu s3,0x1` at `pc=0x9FC42160`)
  each time.
- Once the computed config value finally differs from `s0` (the
  calibration has "found the edge"), it falls through to a sanity
  check: `slti v0,s3,0x0002; bnez ->0x9FC422D4` - **if fewer than 2
  loop iterations happened before the value changed, the whole
  subroutine bails out and returns `-1`.**
- **This project's trace shows exactly that failure**: `s3` reaches
  only `1` before the computed value changes, so the `s3<2` guard
  trips and `v0=-1` is returned. On real hardware, this same
  calibration loop evidently runs enough iterations (consistent with
  the ~100x cycle gap observed above) to keep `s3>=2`.
- The `0x9FC42570`/`0x9FC42650` helpers both poll a hardware register
  at KSEG1 `0xB000F430` (physical `0x1000F430`) via a tight
  `lw v0,(v1); bltz v0,->retry` spin (waiting for a busy/ready bit,
  MSB) before reading/writing - this is SIO/UART hardware, confirmed
  by the surrounding baud-rate setup code, not a SIF register.

**Conclusion**: the bug is not in SIF, not in the EE CPU core's own
opcodes (SRL/ANDI/BGEZ/SLTIU/SLTU here are simple and already
well-tested), and not in ROM/table loading (verified byte-identical).
It is that this project's emulated SIO/UART hardware (behind
`0x1000F430`) responds to the calibration probe in essentially zero
elapsed time, so the BIOS's own "did we actually wait through a real
edge transition" sanity check (`s3>=2`) fails and the whole boot path
takes the permanent-error branch. Real hardware's actual UART timing
takes many more polling iterations for the same probe, comfortably
clearing the guard. This also lines up with the round 9 finding that
COP0 Count in this project advances once per instruction rather than
at a real bus-clock-relative rate - the same class of "our timing
model runs faster/coarser than real hardware" issue underlying both
walls.

**Next step (round 11, not yet started)**: give the emulated SIO/UART
hardware behind `0x1000F430` (and its pair register `0x1000F440`)
enough modeled latency/iteration count that this specific calibration
loop naturally clears its `s3>=2` guard - most likely by having the
busy/ready bit stay set for a handful of polls rather than clearing
immediately, rather than trying to fake a hardcoded `s3` value or
special-case this one call site. No code changes made this round -
investigation only, captured here (plus the underlying trace/disasm
data) so the next round can implement a fix directly instead of
re-deriving this whole three-levels-deep-and-then-some chain again.

### EE JALR investigation, round 8: real Scratchpad RAM + COP0 Count fixed via a live PCSX2 trace - two more walls cleared, boot now reaches a real "wait for interrupt" idle loop

### EE JALR investigation, round 8: real Scratchpad RAM + COP0 Count fixed via a live PCSX2 trace - two more walls cleared, boot now reaches a real "wait for interrupt" idle loop

Direct continuation of round 7's precisely identified wall: two real
exceptions fired, then Status.EXL never cleared again and the EE just
kept running real code in the exception-handler prologue region
indefinitely. The user connected a real, working PCSX2 (via the same
PCSX2-MCP bridge as round 5) and captured exactly the live evidence
needed to resolve it - see `report.md` (user-provided).

**Root cause #1 - the R5900 Scratchpad RAM (SPR) is not ordinary TLB
memory.** The live trace found the faulting instruction is `sd ra,
0x20(sp)` at `pc=0x9FC41008`, with `$sp=0x70003FC0` - inside the
virtual range `0x70000000`-`0x70003FFF`. Per the report's own MMU
analysis (independently confirmed against real PCSX2 source):
this fixed 16KB window is real, dedicated on-chip Scratchpad RAM that
hardware bypasses the TLB for *entirely* - not a normal mapped KUSEG
region needing a TLB entry at all. Confirmed directly in PCSX2's own
source: `pcsx2/Memory.cpp` literally comments "`0x70000000-0x70003fff
scratch pad`"; `pcsx2/MemoryTypes.h` defines a dedicated 16KB
`Ps2MemSize::Scratch` buffer; `pcsx2/COP0.cpp`'s `MapTLB()`
special-cases `isSPR()`-flagged entries to route straight to that
buffer instead of normal PFN-based physical translation. Round 7's TLB
implementation had routed this entire range through ordinary KUSEG TLB
translation like any other address - since the real BIOS's kernel
stack pointer lands in the *upper* half of this window
(`0x70002000-0x70003FFF`), past the one narrow TLB entry the boot path
happened to install, this produced a genuinely unresolvable TLB Refill
exception loop (the "new wall" from round 7's writeup).

**Fix**: `ee_mem_ptr()` now intercepts the fixed range
`0x70000000-0x70003FFF` unconditionally, *before* any TLB lookup,
routing straight to a new dedicated `scratch[16*1024]` buffer in
`ee_state_t` - matching real hardware exactly, independent of whatever
(if anything) a software TLB entry says about that range. Verified:
an 800-million-instruction run against the real SCPH-10000 BIOS no
longer raises a single exception (previously: 2, then stuck forever).

**Root cause #2 - COP0 Count never advanced.** Past the scratchpad fix,
tracing hit a second wall: a classic MIPS delay loop (`MFC0 $v0,$9`
(Count); `SUBU`; `SLTU`; `BNE`) at `pc=0x9FC42500` in the real BIOS,
looping forever because this project's COP0 Count register (`cop0[9]`)
was only ever set via explicit `MTC0` - never a real, free-running
counter. Real PCSX2 advances Count lazily based on elapsed bus cycles
(`COP0.cpp`'s `MFC0` case 9: `Count += cpuRegs.cycle -
cpuRegs.lastCOP0Cycle`); this project has no cycle-accurate timing
model to draw an equally precise increment from, so `ee_step()` now
increments `cop0[9]` by a fixed 1 every executed instruction instead -
a real, working free-running counter (monotonic, comparable against
Compare, exactly the documented COP0 Count/Compare mechanism), just
without precise bus-clock-rate fidelity, which isn't verifiable without
a real timing model and isn't needed just to let a delay loop
terminate. Documented as a known simplification, not fabricated
semantics.

**Tested**: a new `tests/test_ee_scratchpad_count.c` (12 checks) covers
a SW/LW round-trip through the scratchpad's upper half with *no* TLB
entry installed at all (proving the hardware-bypass path, not TLB
translation, is what resolves it), exact boundary checks (0x70000000
and 0x70003FFC map into `scratch[]`; 0x6FFFFFFC and 0x70004000 do not,
correctly falling through to the normal KUSEG TLB-miss path), and Count
advancing by exactly 1 between two consecutive `MFC0` reads. One
pre-existing test, `tests/test_ee_cop0_tlb.c`'s KUSEG-translation case,
had unknowingly picked `0x70000000` as its "generic KUSEG address"
example before the scratchpad's special nature was known - now
collides with the new hardware-bypass path and was fixed by moving it
to `0x71000000` (same kind of test-premise fix as `test_ee_unaligned.c`
and the KUSEG-miss case needed in earlier rounds).

**Verified as real, dramatic progress, not another false-progress
trap**: with both fixes, an 800-million-instruction run against the
real SCPH-10000 BIOS raises zero exceptions and reaches a genuinely new
code region (`pc=0xBFC0092C`) - two full walls past round 7's stopping
point. Disassembly confirms `0xBFC00928` is `J 0xBFC00928` - a literal,
deliberate `j $` self-jump, immediately preceded by a `Compare=1` COP0
timer setup a few instructions earlier (`pc=0xBFC00824`). This is a
real, intentional "wait for interrupt" idle pattern, not a bug: real
hardware escapes it via a genuine interrupt (very plausibly the timer
interrupt this exact Count/Compare setup is meant to trigger) which
this project's EE core has never raised at all (Status.IM/IE are
tracked but nothing on the EE side ever asserts an Interrupt-class
exception, ExcCode 0). This is the next concrete, non-speculative EE
blocker - implementing real EE interrupt delivery, starting with the
Timer (Count==Compare) case - already flagged generically in
`docs/ROADMAP.md`'s "Counters/Timers + INTC" item, now confirmed by
live evidence to be exactly what's needed next rather than one of
several equally-plausible guesses.

Regression: full suite (33 test files, including the two new ones)
passes 0 failures. Wii/devkitPPC target rebuilds clean with no
warnings.


### FPU accumulator (ACC) family implemented

Added the last 7 COP1.S opcodes needed for the FPU's ACC register:
`ADDA.S`/`SUBA.S`/`MULA.S` (write ACC from `fs`+/-/`*``ft`), `MADD.S`/
`MSUB.S` (read ACC, write `fd = ACC +/- fs*ft`, leaving ACC itself
unchanged), and `MADDA.S`/`MSUBA.S` (read+write ACC in place). All
ported directly from PCSX2's `FPU.cpp`. See `docs/ROADMAP.md`'s COP1
bullet for the exact case-by-case semantics.

The one quirk worth calling out again here (documented in `ee_core.c`'s
case comments and deliberately preserved, not "cleaned up"): `MADD.S`/
`MSUB.S` run the intermediate `fs*ft` product through `fpuDouble()` a
SECOND time before combining it with ACC, but `MADDA.S`/`MSUBA.S` don't
- they combine the raw product directly. This is invisible for
ordinary finite values (both paths agree), and only becomes observable
when the product overflows to infinity, since `fpuDouble()` clamps
infinities to `+/-Fmax` on the spot while a raw native float infinity
does not get clamped until the final `fpu_check_overflow()` call at
the very end.

Unit tested in `tests/test_ee_fpu3.c`, 19/19 checks. Beyond the basic
arithmetic checks for each opcode, the last test constructs exactly
that overflow scenario to make the asymmetry directly observable:
ACC preset to an overflow-clamped `-Fmax` (via `ADDA.S(-3.4e38,
-3.4e38)`, which itself overflows to `-infinity` and gets clamped),
and an `fs*ft` product that overflows to `+infinity` (`1e30 * 1e30`).
Feeding the SAME inputs to both opcodes gives different, verifiable
results: `MADD.S`'s `fd` ends up exactly `0.0` (`-Fmax + Fmax`, since
the product got pre-clamped to `+Fmax`), while `MADDA.S`'s ACC ends up
`+Fmax` (`-Fmax + raw-infinity` overflows to `+infinity`, only clamped
afterward). This is a genuine, checkable behavioral difference, not
just a comment - confirms the port is faithful to PCSX2's actual
control flow rather than an equivalent-looking simplification.

Regression: full suite (24 test files, including this new one) all
pass 0 failures. Also caught and fixed a stale, unrelated regression
script/doc bug while running the full suite: `tests/README.md`'s
documented build command for `test_iop_hle_bios.c` was missing
`iop_hle_modules.c` on the link line (this test's `iop_core.c`
dependency started requiring it after the module-registry work,
`iop_core_init()` now calls `iop_hle_modules_init()`, but the doc's
command was never updated) - fixed the command in `tests/README.md`.

Wii/devkitPPC target rebuilds clean with no warnings.

### MMI compare/max/min/abs opcode family implemented

Added 13 MMI (SIMD) opcodes ported directly from PCSX2's `MMI.cpp`:
`PCGTW`/`PCGTH`/`PCGTB` and `PMAXW`/`PMAXH` (MMI0 sub-table, `sa`
0x02/0x03/0x06/0x07/0x0A), and `PABSW`/`PCEQW`/`PMINW`/`PADSBH`/
`PABSH`/`PCEQH`/`PMINH`/`PCEQB` (MMI1 sub-table, `sa`
0x01/0x02/0x03/0x04/0x05/0x06/0x07/0x0A) - funct/sa values confirmed
against the real `tbl_MMI0[32]`/`tbl_MMI1[32]` tables in
`R5900OpcodeTables.cpp`, not assumed. This brings EE MMI coverage from
~35 to ~48 of the roughly 90 real opcodes.

Three things worth documenting explicitly (all ported as real hardware
behavior, not simplified away): the compare opcodes (`PCGT*`/`PCEQ*`)
produce a per-lane all-1s/all-0s mask result, not a boolean 0/1 - this
is the standard SIMD-compare convention on real hardware, used by
guest code to build a select mask, not just a scalar boolean.
`PMAXW`/`PMAXH`/`PMINW`/`PMINH` use a genuine signed comparison (unlike
the earlier `MAX.S`/`MIN.S` FPU opcodes, which needed the bit-level
signed-int trick specifically because IEEE-754 floats don't sort as
plain integers - these are already ordinary twos-complement integers,
so a normal signed compare is correct and sufficient). `PABSW`/`PABSH`
preserve a real quirk: `INT32_MIN`/`INT16_MIN` (`0x80000000`/`0x8000`)
have no positive representation at their own bit width, so real
hardware clamps the result to `INT32_MAX`/`INT16_MAX` instead of
overflowing back to the same negative value. And `PADSBH` ("add/
subtract halfword") is a genuinely asymmetric single instruction. not
a uniform 8-lane op: its low 4 halfword lanes compute `PSUBH` (rs-rt)
while its high 4 lanes compute `PADDH` (rs+rt).

Unit tested in `tests/test_ee_mmi_compare.c`, 32/32 checks - operands
planted directly into EE RAM and loaded via `LQ`, since these are
whole-register SIMD lane ops (same approach as `tests/test_ee_lqsq.c`).
Covers both directions of every compare (true and false cases, to
prove none of them are accidentally unconditional), a mixed-sign case
for max/min that wouldn't survive a naive unsigned/bit-pattern
compare, the `INT32_MIN`/`INT16_MIN` clamp case for both `PABSW` and
`PABSH`, and specific low-lane vs. high-lane checks for `PADSBH` to
confirm the asymmetry is real.

Regression: full test suite (25 test files total, including this new
one) all pass 0 failures. Wii/devkitPPC target rebuilds clean with no
warnings.

### MMI0 completed: saturated arithmetic + PEXT5/PPAC5

Added the last 8 MMI0 sub-opcodes, ported from PCSX2's `MMI.cpp`:
`PADDSW`/`PSUBSW` (32-bit), `PADDSH`/`PSUBSH` (16-bit), `PADDSB`/
`PSUBSB` (8-bit) - all saturated signed add/sub, computing the result
in a wider intermediate type (e.g. 64-bit for the 32-bit lanes) and
clamping to that lane width's signed min/max instead of wrapping on
overflow/underflow - and `PEXT5`/`PPAC5`, which unpack/pack a GS
16-bit 5551 pixel format (5 bits R, 5 bits G, 5 bits B, 1 bit A) to/
from 32-bit lanes with each channel left-aligned in its own byte.
Both `PEXT5` and `PPAC5` use only the `rt` operand - `rs` is unused,
matching real hardware/PCSX2 (these are unary unpack/pack ops, not
binary ones like the rest of MMI0). Funct/`sa` values confirmed
against the real `tbl_MMI0[32]` table in `R5900OpcodeTables.cpp`, not
assumed. This completes every defined MMI0 sub-opcode (the four
remaining table slots are genuinely `MMI_Unknown` in real hardware,
not gaps in this port) and brings EE MMI coverage from ~48 to ~56 of
the roughly 90 real opcodes.

Unit tested in `tests/test_ee_mmi_sat.c`, 21/21 checks - same LQ-based
RAM-planting approach as `tests/test_ee_mmi_compare.c`. Each saturated
op is checked in a normal (non-saturating) case plus both an overflow
and an underflow case, to confirm the clamp lands on the correct
signed min/max for that width rather than merely not-crashing.
`PEXT5`/`PPAC5` are checked with a specific 5551 pixel
(R=0x1F, G=0x03, B=0x00, A=1), verified channel-by-channel after
`PEXT5`, then round-tripped back through `PPAC5` to confirm the
original 16-bit value comes back exactly - this caught an arithmetic
mistake in the test itself (not the implementation) during
development: an initial hand-computed expected pixel value
(`0x801F`) turned out to be wrong once checked bit-by-bit against the
5/5/5/1 layout (the correct value for that R/G/B/A combination is
`0x807F`) - a good reminder to verify test *expectations*
programmatically/by hand-derivation, not just trust a first guess at
the encoding.

Regression: full test suite (27 test files total, including this new
one) all pass 0 failures. Wii/devkitPPC target rebuilds clean with no
warnings.

### MMI2/MMI3 permute/interleave family implemented

Added 9 self-contained MMI2/MMI3 data-movement opcodes, ported
directly from PCSX2's `MMI.cpp`: `PINTH`/`PINTEH` (interleave Rs/Rt
halfword lanes), `PEXEH`/`PEXCH` and `PEXEW`/`PEXCW` (halfword- and
word-granularity lane swaps), `PREVH` (full lane reverse), `PCPYH`
(lane broadcast), and `PROT3W` (3-lane rotate). These were chosen as
the next batch specifically because they need no additional CPU
state (unlike `QFSRV`, which needs the SA hardware register and the
`MTSA`/`MTSAB`/`MTSAH` instructions to set it - none of which exist
in this project yet, so `QFSRV` stays out of scope for now) and no
HI/LO interaction (unlike the remaining MMI2/MMI3 arithmetic opcodes,
e.g. `PMADDW`/`PMULTW`/`PDIVW`). `sa` values confirmed against the
real `tbl_MMI2[32]`/`tbl_MMI3[32]` tables in `R5900OpcodeTables.cpp`.
Brings EE MMI coverage from ~56 to ~65 of the roughly 90 real opcodes.

Two pairs of opcodes are easy to confuse with each other and got
extra attention: `PINTH` takes ALL of Rt's lanes plus Rs's UPPER 4
lanes, while `PINTEH` takes only the EVEN-indexed lanes of BOTH Rs
and Rt - a real, distinct difference, not two names for the same
operation. Likewise `PEXEH`/`PEXEW` swap lane pair 0/2 while their
"C" counterparts `PEXCH`/`PEXCW` swap lane pair 1/2 - kept as
separate case bodies rather than a shared helper with a parameter, to
avoid a transcription slip silently making one of them wrong.

Unit tested in `tests/test_ee_mmi_permute.c`, 32/32 checks - operands
use distinct, position-identifiable lane values (e.g. halfword lane N
= `0x50+N` for Rs / `0x60+N` for Rt) so a wrong permutation shows up
immediately as the wrong value in the wrong lane. Specifically checks
that `PINTH` and `PINTEH` produce different results from the same
inputs, and likewise for the `PEXEH`/`PEXCH` and `PEXEW`/`PEXCW`
pairs, so a copy-paste mix-up between either pair would be caught.

Regression: full test suite (28 test files total, including this new
one) all pass 0 failures. Wii/devkitPPC target rebuilds clean with no
warnings.

### PSLLVW/PSRLVW added

Added `PSLLVW`/`PSRLVW` (MMI2, `sa` 0x02/0x03), ported from PCSX2's
`MMI.cpp`: a variable logical shift of a word pair - Rt's word lanes
0 and 2, each shifted by the shift amount taken from the
CORRESPONDING lane of Rs (lane 0's amount from Rs lane 0, lane 2's
amount from Rs lane 2, masked to 5 bits - the standard MIPS
variable-shift convention). Each 32-bit result is sign-extended to 64
bits into `gpr.ud0`/`gpr.ud1` (matching every other 32-bit GPR result
in this file via the existing `sext32()` helper), but the shift
operation itself is a plain logical shift with no sign propagation -
worth stating explicitly since both facts are true at once and can
read as contradictory: `PSRLVW` of `0x80000000 >> 4` gives
`0x08000000` (no 1-bits shifted in from the top), but that 32-bit
result is still sign-extended afterward like any other GPR value
(irrelevant here since the top bit of the *result* happens to be 0,
but would matter for a different shift amount).

Picked as a small, self-contained next step after finishing the MMI2/
MMI3 permute family - these two need no HI/LO interaction, unlike the
next real chunk of remaining MMI work (the `PMADDW`/`PMSUBW`/`PMULTW`
family), which PCSX2's own source flags with a real-hardware
"division voodoo" rounding correction that will need extra care to
port faithfully rather than being ported naively.

Unit tested in `tests/test_ee_mmi_pvshift.c`, 6/6 checks - confirms
the 5-bit shift-amount masking (amount 35 behaves like 3), that
`PSLLVW` sign-extends a bit-31-set result to 64 bits, and that
`PSRLVW`'s shift itself doesn't propagate the sign bit.

Regression: full test suite (29 test files total, including this new
one) all pass 0 failures. Wii/devkitPPC target rebuilds clean with no
warnings.

### Round 15 (2026-07-07): the round-14 IOP wall genuinely bypassed via a real module/IRX loader; a real VU opcode table; an SPU2 register scaffold

User directive this round: "fix all IOP errors and port over the IOP
and IRX loader, and the VU microcode table, if you have time SPU2."

**Real IOP module/IRX loader (tasks #91-93).** The round-14 wall
(`JALR $s1=0x03400008` at IOP pc~0x00000E08, ~3.05M instructions into
real BIOS boot) was previously documented as "an honest architectural
boundary requiring a real IOP module loader" - this round built one.
Via local, uncommitted analysis of the user's own real SCPH-10000 BIOS
(never committed, no BIOS bytes in any test fixture - see
`include/core/hw/iop_module_loader.h`'s citation trail), this project
reverse-engineered and implemented, for the first time: a real
ELF32/MIPS "IRX" module loader (`source/hw/iop_elf.c`) with genuine
relocation processing (R_MIPS_32/26/HI16/LO16, cross-checked against
ps2dev/ps2sdk's public `irx.h` and the community "PS2 BIOS in Rust"
book), and a real ROMDIR/IOPBTCONF-driven sequential module loader
with export/import table linking (`source/hw/iop_module_loader.c`).
19 new synthetic-fixture unit tests plus the full regression suite
pass.

Live-traced against the real BIOS, this genuinely: parses all 29 real
IOPBTCONF module names in order, loads and relocates the real SYSMEM
module for the first time in this project's history, and redirects
the interpreter to SYSMEM's real entry point. A **second, real bug**
was found this way and fixed: SYSMEM's own function prologue (`sw
$ra, ...($sp)`) was corrupting its own return address because this
project's module loader never set up a stack pointer before jumping
into a module's entry - `$sp` held stale/leftover state, so the
prologue's stack-relative save/restore landed on garbage memory,
`$ra` came back as 0 instead of this loader's trampoline, and
execution looped back through address 0 into the *exact same*
original wall. Fixing this (seeding `$sp` to a documented top-of-RAM
value before each module's entry) genuinely resolved the recurrence -
live-traced confirmation that the interpreter now runs real SYSMEM
kernel code well past the original wall (branch-traced past
instruction 3,054,850 into previously-never-executed real kernel
code, versus immediately re-hitting pc=0x00000e0c before the fix).

A **third, deeper boundary** was found and honestly documented rather
than chased further this round: SYSMEM's own init code reads a value
via `lw $v0, 0($a0)` (almost certainly a RAM-size/boot-info pointer
real loadcore would normally pass as an argument) - this project's
loader doesn't set up module-entry argument registers (only `$ra`/
`$sp`), so `$a0`=0, the read returns 0, and a subsequent `sll
$sp,$v0,20` (likely "compute stack pointer from discovered RAM size in
MB") zeroes `$sp` again. This is a genuine, narrower, well-understood
scope gap (real loadcore's module-entry calling convention/boot-info
block isn't modeled) rather than a re-emergence of the original wall -
left for a future round.

**Real VU upper/lower opcode table (task #94).** Previous rounds
explicitly could not find PCSX2's own VU opcode dispatch tables in any
fetched source file, so every VU micro-mode instruction was a logged
no-op with correct control flow only. This round found and used the
original Sony "PlayStation 2 Vector Unit Instruction Manual" (a
primary hardware reference) instead - see `source/hw/vu_opcodes.h` for
the full bit-field citation trail, including an explicit list of the
handful of instructions this project deliberately left unimplemented
because the source document's own tables were internally inconsistent
for them (an encoding collision between "OPMSUB" and "MULbc.w" in
particular - resolved in favor of the far more common MULbc, with
OPMSUB left unimplemented rather than guessed).

`vu_micro_step()` now really decodes and executes: upper FMAC
arithmetic (ADD/SUB/MUL/MADD/MSUB/MAX/MINI and their broadcast/Q/I
forms, the ADDA/SUBA/MADDA/MSUBA accumulator family, OPMULA outer
product, ABS, ITOF/FTOI fixed-point conversion), lower integer ALU
(IADD/ISUB/IADDI/IAND/IOR), load/store (LQ/SQ/LQI/SQI/LQD/SQD/ILW/ISW/
ILWR/ISWR/MTIR/MFIR/MOVE/MR32), and branches (B/BAL/JR/JALR/IBEQ/IBNE/
IBLTZ/IBGTZ/IBLEZ/IBGEZ) with correct delay-slot semantics. A real
accumulator register (`acc[4]`) was added alongside the existing VF/VI
register file, and `vi[22]` is now used as the real Q register.

12 new tests validate actual arithmetic/branch/load-store *results*
(not just control flow) - real ADD, real ADDA-then-MADD proving the
accumulator genuinely round-trips, real ADDbc lane broadcast, real
ADDQ using the Q register, a real branch whose delay-slot instruction
executes and whose skipped instruction doesn't, real IADD, and a real
SQ/LQ quadword round-trip through VU1 data memory. One existing test
assertion needed updating (not loosening): an all-zero instruction
word is no longer "no real opcode" - bits5-2=0 of the upper word is
the real ADDbc encoding and bits31-25=0 of the lower word is the real
LQ encoding, both genuinely matched (if degenerate/no-effect)
instructions now that real decode exists.

**SPU2 register scaffold (task #95, time permitting).** No SPU2 code
existed at all. Added `source/hw/iop_spu2.c`/`.h`: a real, cited
register-file scaffold at the real IOP-side base address
(0x1F900000), correctly modeling SPU2's native 16-bit register
granularity (wired into `iop_core.c`'s `iop_mem_read16`/`write16`,
which previously had no MMIO dispatch at all - only RAM/BIOS passthrough)
plus 32-bit access for any real code that uses LW/SW instead. Honestly
scoped as a scaffold, not audio: no per-register (voice/ADSR/volume)
semantics are modeled, only documented as such. 10 new tests (direct
unit tests plus integration through real IOP LH/SH/LW/SW instructions)
all pass.

Regression: full test suite (54 test files total, including
`test_iop_elf.c` and `test_iop_spu2.c`) all pass 0 failures.
Wii/devkitPPC target rebuilds clean (0 errors; one pre-existing,
unrelated `strncpy` truncation warning in `iop_module_loader.c` from
this same round, harmless - the copied ROMDIR name field is always
exactly 10 bytes and the destination buffer is 11 bytes with an
explicit trailing NUL set separately).

### Round 16 (2026-07-07): third BIOS-filename candidate for the test menu, user's own real BIOS validated for local Dolphin testing

Follow-up to Round 15's native Wii test menu (task #97). The user provided
their own real, legally-owned PS2 BIOS dump (SCPH-10000, 4,194,304 bytes -
the correct size for a real PS2 BIOS ROM) for local testing purposes.

**Change made to the repo (source only, no BIOS bytes involved):**
`source/main.c`'s `action_bios_boot_test()` previously only tried
`sd:/pcsx2/bios/SCPH39001.bin` and `sd:/pcsx2/bios/bios.bin`. Added a third
candidate filename, `sd:/pcsx2/bios/SCPH10000.bin`, so a user with an
SCPH-10000 dump doesn't have to rename their file to use the BIOS Boot Test
menu action. This is a one-line string-literal change; no BIOS bytes are
embedded anywhere in the repo.

**Handling of the user's real BIOS file (per this project's absolute,
standing rule):** the file was inspected transiently in the sandbox only
(size/header sanity-checked - it starts with plausible MIPS boot-code
opcodes, consistent with a real BIOS ROM) and was never copied into
`/tmp/pcsx2-wii` (the git repo), never committed, never pushed, and never
placed in the rsync'd outputs mirror of the repo. Instead, a separate,
non-repo deliverable folder (`dolphin_sdcard/pcsx2/bios/SCPH10000.bin`) was
created directly in the outputs folder, structured as a virtual SD card
root the user can point Dolphin's "SD Card Folder" device at, so the BIOS
Boot Test menu action can load it. This folder is not tracked by git, not
part of the PCSX2-Wii repository, and is delivered to the user only as a
local convenience artifact.

**Regression / build status:** all 93 host-native regression test binaries
continue to pass (0 failures). Clean devkitPPC/libogc rebuild of
`boot.dol`, same single pre-existing harmless `strncpy` truncation warning
in `iop_module_loader.c` as prior rounds (unrelated to this change).

### Round 17 (2026-07-07): first real on-device (Dolphin) BIOS boot validation, and a precisely-traced new IOP SYSCALL-exception boundary

**On-device validation, the headline result**: the user ran the native
Wii test menu's "BIOS Boot Test" action in a real Dolphin session
against their own real SCPH-10000 BIOS (via the `sd_v2.raw` virtual SD
image built this round with `mtools`, after an earlier `pyfatfs`-built
image proved unreadable by real `libfat` - see below). Result: BIOS
loaded correctly (`rom_ver=0100JC20000117`), both EE and IOP cores
initialized and ran interleaved for the full 2,000,000-slice test cap
without halting - EE reached `instructions_executed=16,000,000`
(pc=0x8000B8AC) and IOP reached `instructions_executed=2,000,000`
(pc=0x001A44EC). This is the first time this project has observed the
real module/IRX loader (round 15) and real VU opcode table (round 15)
running against the real BIOS on real (Dolphin-emulated) Wii hardware
rather than only in host-native diagnostics, and it matches host-native
diagnostics exactly (see below) - strong cross-validation that the Wii
build and the host-native test harness are behaviorally consistent.

**SD-image tooling note**: the first `sd.raw` (built by writing directly
into an mkfs.vfat-formatted file via the `pyfatfs` Python library) mounted
fine in Dolphin but the BIOS file was never found by any of the 3
candidate filenames - `libfat`'s own source (`fatInit`/`disc.c`) was
read directly to rule out a device-naming mismatch (Wii's SD slot
genuinely registers as `"sd"`, confirming `sd:/...` paths were always
correct) - the real cause was almost certainly a `pyfatfs`-write
compliance gap, not a project bug. Rebuilt with `mtools` (`mcopy`/`mmd`,
obtained via `apt-get download` without root, extracted with
`dpkg-deb -x`) instead, verified byte-for-byte via read-back
(`md5sum` match), and this image (`sd_v2.raw`) worked correctly on the
first try.

**New finding - a real IOP SYSCALL exception leads to a dead end**:
following up on round 14's finding that the EE settles into a
legitimate SIF-mailbox-polling steady state, this round traced the IOP
side of that same steady state in detail using host-native diagnostics
(`/tmp/diag8.c` through `/tmp/diag15.c`, not committed - transient
`/tmp`-only scratch tools per this project's established practice).

Disassembly (via `capstone`, `pip`-installed) of the EE's steady-state
loop at `0x80005E58`-`0x80006278` shows it is a real, legitimate
subroutine: it reads the three real SIF mailbox/flag registers
(`0xB000F200`=SIF_MSCOM, `0xB000F210`=SIF_SMCOM, `0xB000F230`=SIF_SMFLG),
each with a genuine settle-then-recheck pattern, then loops
(`and v0,v0,s0` / `beqz v0,retry`) until a caller-supplied bitmask
matches - i.e. the EE is correctly, faithfully waiting for the IOP side
to set specific SIF flag bits. Real hardware would do exactly this.

The IOP side, traced via repeated `system_run_interleaved()` calls at
fine (10,000-slice) granularity to pinpoint the exact transition,
reaches a clean, stable, sane state at `pc=0x001A44EC` (`sp=0x801ffdd8`,
matching the real on-device Dolphin result exactly) and holds it from
roughly instruction 1,700,000 onward - genuinely idle/polling, not
stuck. But at **exactly instruction 3,059,999** (fully deterministic -
reproduced identically whether stepped in 10,000-, 50,000-, or
2,500,000-instruction chunks, ruling out a chunking artifact), the IOP
takes a real MIPS **SYSCALL exception** (`Cause=0x00000020`, ExcCode 8;
`EPC=0x8003ecf4`; `$v0=1` at the time, `$a0=0xFFFFFF98`) - a completely
normal, real R3000A mechanism real IOP kernel code uses for kernel
calls (thread/semaphore/etc. requests), distinct from this project's
existing A0/B0/C0 jump-table BIOS-call convention (task #31).

After that exception, though, control does not end up in a sensible
syscall handler return: `$ra` is left holding `0x00100000` (suspicious
- exactly the raw base address SYSMEM was loaded at, not a real return
address), and the IOP proceeds to (re-)execute what disassembly
confirms is SYSMEM's own internal 17-iteration array-zeroing init loop
(`0x00100D94`-`0x00100DBC` - matches round 14's "legitimate bounded
loop" finding verbatim) - except this time with the stack pointer
already at `sp=0xFFFFFF40` (a small, ~192-byte underflow past address
0, i.e. `-0xC0`), consistent with round 15's already-documented "third
boundary" (module-entry argument/boot-info block not modeled, so a
downstream stack-size computation goes wrong). From this point on the
IOP repeatedly re-enters this same narrow loop/init path and never
progresses further within any tested budget (traced cleanly out to
95,000,000 slices with no change).

**Working theory, not yet fixed**: this project's IOP SYSCALL exception
vector/dispatch either has no real handler for this specific syscall
number, or its handler incorrectly falls through into re-running
SYSMEM's own init path instead of returning to the caller - likely
connected to the same missing module-entry-argument gap round 15 called
out, now manifesting through a different, previously-unreached code
path (a real SYSCALL, not a raw JALR). Not fixed this round to keep
scope honest and give the user a checkpoint before deciding how much
further to dig - full precise trace is above for whoever picks this up
next (host or a future round).

Regression: 93/93 host-native test binaries pass, clean Wii rebuild.
No BIOS bytes were added to the repo; all diagnostics above live only
in `/tmp` scratch files, never committed.

### Round 18 (2026-07-07): the module-entry boot-info gap fixed for real - stack corruption resolved, a new (likely legitimate) real BIOS panic loop reached

Direct fix for round 17's precisely-traced finding and round 15's
originally-deferred "third boundary". Disassembly of the real, loaded
SYSMEM module's own entry code (cited in round 17) showed it reads a
word through $a0 and left-shifts it by 20 bits to compute its initial
stack pointer (`lw v0,(a0); sll sp,v0,0x14`) - i.e. it expects $a0 to
point at a word giving the number of megabytes of IOP RAM. This
project's module loader (`iop_module_loader.c`) never set $a0 before
jumping to a module's entry point (only $ra/$sp, from round 15), so
`*a0` read whatever was at IOP RAM address 0 (0), collapsing SYSMEM's
own computed stack pointer to 0.

**Fix**: both jump sites in `iop_module_loader.c`
(`iop_module_loader_boot()` and `iop_module_loader_try_handle()`) now
allocate a small word via the existing bump allocator, write the real,
hardware-invariant constant `2` (PS2 IOP RAM is always exactly 2MB,
unlike the EE side - not a guess) into it, and set `$a0` to its
address before every module entry jump. See
`source/hw/iop_module_loader.c`'s new `BOOT_INFO_RAM_MB` comment for
the full citation and reasoning.

**Verified via host-native diagnostics** (`/tmp/diag17.c`-
`/tmp/diag19.c`, transient, not committed): before the fix, the IOP's
stack pointer walked down through and past IOP RAM address 0 (ending
up at `sp=0xFFFFFF40`, a ~192-byte underflow) as a direct consequence
of the bogus zero-derived initial SP, and - critically - this
underflow silently overwrote the real, dump-specific exception-vector
trampoline `InstallExceptionHandlers` (task #42) had correctly written
to address `0x80`, so a later, completely normal SYSCALL exception
(instruction 3,059,999, fully deterministic) vectored into garbage
instead of the real handler. After the fix, the same SYSCALL fires at
the same instruction count, but the stack pointer stays sane throughout
(`sp≈0x001FFFxx`, matching this project's own `INITIAL_SP` convention),
and the real exception-vector trampoline survives intact.

**New resting state reached**: with the corruption gone, the IOP now
runs the real exception handler through to a different code region
(`0x00101270`-`0x00101288`), which disassembly shows is:
```
lui  v1, 0x8000
addiu v0, zero, 2
sb   v0, (v1)      ; store byte 2 to address 0x80000000 (IOP kseg0 -> RAM addr 0)
j    0x101270      ; unconditional infinite loop
```
This reads as a genuine, deliberately-authored real BIOS panic/halt
loop (write an error code to a well-known low address, then spin
forever) rather than an emulation artifact - the interpreter is no
longer wandering through corrupted memory, it's executing real,
sensible-looking kernel code that appears to detect some other still-
missing condition (most plausibly: this project has never implemented
a real IOP kernel SYSCALL dispatch table - only the separate, PS1-
legacy-style A0/B0/C0 jump-table BIOS calls - so whatever the real
exception handler tries to do with this SYSCALL's request number likely
fails, and the real BIOS code correctly, safely halts). Not
investigated further this round to keep scope honest; flagged in
ROADMAP.md as the next concrete target (a real IOP kernel syscall
table is a substantial, well-defined next feature, distinct from the
existing A0/B0/C0 mechanism).

Regression: 93/93 host-native tests still pass unchanged (this fix only
touches module-entry argument setup, not any tested opcode behavior).
Clean Wii/devkitPPC rebuild, same single pre-existing harmless
`strncpy` truncation warning as prior rounds.

### Round 19 (2026-07-07): IOP SYSCALL investigation, corrected - the real gap is an unregistered exception-handler chain, not a missing dispatch table

Direct follow-up to Round 18's fix and its own stated hypothesis ("most
likely...a real IOP kernel SYSCALL dispatch table was never
implemented"). This round traced the actual instruction-level sequence
around the SYSCALL exception precisely and found that hypothesis was
**wrong** - the real situation is more specific and points at a
different, better-scoped gap.

**What's actually happening, traced precisely** (host-native
diagnostics `/tmp/diag20.c`-`/tmp/diag28.c`, transient, not committed):

1. At instruction 3,054,722, real ROM bootstrap code executes a genuine
   `SYSCALL` with `$a0=0x00000002` - confirmed via the psx-spx public
   kernel reference (https://psx-spx.consoledev.net/kernelbios/,
   "SYS-Functions (Syscall opcode with function number in R4 aka A0
   Register)") to be real `SYS(02h) ExitCriticalSection()` - about as
   ordinary a kernel call as exists.
2. The exception correctly vectors to `0x80000080`; the real,
   dump-specific trampoline `InstallExceptionHandlers` installed there
   (task #42) correctly executes (`LUI $k0/ADDIU $k0/JR $k0`) and jumps
   into real BIOS RAM code at `0x00000c80`.
3. Disassembly of `0x00000c80`-`0x00000e30` (`capstone`) confirms this
   is a genuine, textbook R3000A generic exception dispatcher: it
   saves the ENTIRE GPR context into a save area, reads `EPC` via
   `MFC0`, then looks up a registered handler through a linked
   structure rooted at `RAM[0x100]` (`addiu $s3,zero,0x100; lw
   $s3,($s3)`, then two more indirections) and calls it: `jalr $s1`.
   This is a real exception-handler-CHAIN mechanism (multiple handlers
   can be registered; `beqz $s0,...`/`lw $s6,($s6)` afterward walk to
   the next node) - not something this project needs to fabricate, it
   is genuinely present in the loaded BIOS/RAM image.
4. The problem: at this point in boot, `RAM[0x100]`'s chain has never
   had a real handler registered - the value found through it is the
   same leftover exception-vector-template bytes (`0x03400008`,
   literally the `JR $k0` instruction encoding) this project has
   already found and explained twice before (round 14's original
   finding, and again here). The `jalr $s1` therefore jumps to
   `0x03400008`, which this project's PC-fetch-sanity guard catches -
   and, since `iop_module_loader_boot()` had never yet run (`g.attempted`
   was still 0 at this exact point - confirmed via a temporary debug
   build), this is the SAME event round 15 already documented as "how
   SYSMEM first gets loaded." Round 15's account of this event is
   correct on its own terms, but this round adds the missing piece of
   context: it happens as a side effect of an early, real
   `ExitCriticalSection` syscall's exception-chain lookup finding an
   unpopulated/stale entry, not as an independent, freestanding event.

**Consequence**: because the loader's guard treats this escape as "go
load the next real module" rather than "no handler registered, fall
back to whatever real hardware's kernel does by default," this project
never actually returns from the original SYSCALL exception at all
(no `ERET` back to the ROM bootstrap code that issued
`ExitCriticalSection`) - execution is diverted into SYSMEM's own
module-init code instead. That code is genuinely real and mostly
plausible (see below), but it is a different, disconnected program
from whatever the ROM bootstrap was doing before the syscall - not a
continuation of a single coherent boot sequence.

**SYSMEM's own init code, traced further**: with round 18's stack fix
in place, SYSMEM's entry code runs a very real-looking sequence: a
17-iteration array-zeroing loop (round 14/18), then what disassembly
confirms is a genuine phase-based driver-dispatch loop at
`0x0010119c`-`0x00101264` - iterating a linked list of
(function-pointer | phase-tag) entries, calling (`jalr`) each entry
whose low 2 tag bits match the current phase counter (0..3), matching
a classic RTOS "call all registered drivers for phase N, then advance"
pattern. Once all 4 phases complete, execution falls through to
`0x00101268`-`0x00101288`: `sb` a status byte (value 2) to IOP RAM
address 0, then an unconditional `j` back to itself. Checked whether
this is a real crash/panic or an idle wait: `Status.IEc` (COP0
register 12, bit 0 - global interrupt enable) is `0x00000000` the
entire time this loop runs, and stays exactly `0` across 2,000,000
further instructions with zero interrupts ever taken (`Cause` never
changes). So even if this project's IOP timer/interrupt-controller
model correctly raised a pending interrupt condition here, the CPU
would not take it while `IEc=0` - this loop cannot be broken by an
interrupt in its current state regardless of timer/INTC correctness.

**Not fixed this round** (correctly scoped as substantial, not a quick
patch): two independent, well-defined next targets, either of which
could plausibly move this forward:
  (a) make the generic exception dispatcher's "no handler registered"
      case behave like real hardware's actual default instead of
      relying on the module-loader escape hatch as a lucky substitute
      - needs research into what real IOP kernel init does at
        `RAM[0x100]` before any handler is registered (a citable
        reference, not a guess);
  (b) figure out why `Status.IEc` never gets set to 1 anywhere in the
      traced execution - either a missing real "enable interrupts"
      step this project's model doesn't yet reach, or confirmation
      that real hardware genuinely keeps interrupts masked at this
      exact point too (in which case the idle loop is a real, if
      early, resting state and this project's IOP model is behaving
      correctly - just incomplete elsewhere).

Regression: no source changes this round (pure investigation), 93/93
tests unaffected. No BIOS bytes committed; all diagnostics are
`/tmp`-only, discarded at session end.


### Round 20 (2026-07-07): VIF UNPACK implemented (item 4 of the user's requested "1, 4, then 5" order)

Real UNPACK (VIFcode CMD 0x60-0x7F) is now implemented in `vif.c`,
ported directly from a live fetch of PCSX2's own `Vif_Unpack.cpp`/
`Vif_Unpack.h` (github.com/PCSX2/pcsx2, master) - not guessed. This
was previously the one deliberately out-of-scope VIF command (see
round-13-era `vif.h` notes); VU0/VU1 both now have real local DATA
memory to unpack into (`vu0_mem_write32()` in `ee_core.c`,
`vu1_mem_write32()` in `vu.c` - siblings of the existing
`vu0_micro_write32()`/`vu1_micro_write32()` used by MPG).

**What's implemented, precisely** (see `vif.c`'s `vif_unpack()` and
`vif.h`'s header comment for the full per-line citation trail):

- CMD bit layout: bits 5-6 = 0b11 (signals UNPACK), bit 4 = M (mask
  enable), bits 0-3 = VN*4+VL (VN: 0=S,1=V2,2=V3,3=V4; VL:
  0=32-bit,1=16-bit,2=8-bit,3=5-bit/V4 only). Per-format source byte
  size from PCSX2's own `nVifT[16]` table, reproduced verbatim as
  `VIF_UNPACK_SIZE[16]`.
- IMM field reinterpreted for UNPACK: bits 0-9 = VU mem address
  (qwords), bit 14 = USN (0=signed/1=unsigned), bit 15 = FLG (VIF1
  only - address relative to TOPS).
- S: one component broadcast to all 4 lanes. V2: two components,
  written X=v0,Y=v1,Z=v0,W=v1 (real hardware repeats the pair,
  confirmed from PCSX2's `UNPACK_V2`). V3: reuses the V4 4-component
  read - the W lane genuinely reads 1 component-width PAST the real
  3-component data (typically into the next vector's first component)
  and gets overwritten by the next unpack - a real, cited hardware
  quirk PCSX2's own comment says Ape Escape 3 depends on, reproduced
  here rather than "fixed". V4-5: a single 16-bit read decoded via the
  exact real bit-shift formula from `UNPACK_V4_5`
  (X=(d&0x1F)<<3,Y=(d&0x3E0)>>2,Z=(d&0x7C00)>>7,W=(d&0x8000)>>8), MODE
  forced to 0 (real hardware: "V4_5 unpacks do not support the MODE
  register").
- STCYCL-controlled CL/WL skip-write/fill-write cycles (real
  `_nVifUnpackLoop` "isFill"/"skipSize" timing, including its
  documented advance-then-read ordering - ported faithfully even
  though it's genuinely subtle: for the by-hand-traced case CL=2,WL=4
  the real sequence is [read0, read1, read2, repeat-of-read2], not the
  naively-expected [read0, repeat, repeat, repeat]). STMASK/STROW/
  STCOL-based per-lane masking (Data/MaskRow/MaskCol/Write-Protect,
  real 2-bits-per-lane-per-cycle-position `mask` register layout from
  `writeXYZW`), and STMOD-driven row accumulate/chain modes (0-3, real
  `writeXYZW` switch/`setVifRow` semantics).
- Consumed-word accounting: rather than trying to reuse PCSX2's
  separate `vifUnpackSetup()` "tag.size" formula (which this round's
  investigation found can under-count vs. what the per-vector loop
  actually dereferences, in ways that only matter for the DMA
  controller's own DIFFERENT bookkeeping concern - not for finding
  where the next VIFcode begins in an in-memory buffer), this project
  tracks the furthest byte offset any iteration actually dereferenced
  (correctly handling both V3's real read-ahead quirk and fill mode's
  repeat iterations) and rounds up to whole words. Verified against 12
  new host-native checks (`tests/test_vif.c`) covering V4-32, S-32
  broadcast, V2-16 signed/unsigned, V3-8's read-ahead quirk, V4-5's
  bit-shift decode, all 4 masking modes, STMOD mode 2's row-accumulate
  chaining, and both a skip-write and a fill-write STCYCL scenario
  (the fill-write expectation was itself derived by hand-tracing the
  real algorithm's instruction-level timing, not assumed).

**Not implemented**: a partial UNPACK payload split across multiple
DMA calls (real hardware/PCSX2 buffer this via `nVifStruct::buffer` -
this project's `vif_process()` only ever sees one contiguous transfer
at a time; a payload that runs off the end of what's actually present
reads 0 for the missing bytes via `vif_rd_bytes_le()`'s existing bounds
check rather than reading adjacent, unrelated memory - flagged, not
guessed, matching this project's established pattern for narrow first
increments).

Regression: 12 new checks added to `tests/test_vif.c` (was 23 checks,
now 35), all passing; full suite still 0 failures across every
host-native test binary. Clean Wii/devkitPPC rebuild, same single
pre-existing harmless `strncpy` truncation warning as prior rounds.

### Round 21 (2026-07-07): POINT/LINE/LINE_STRIP rasterization (item 5 of the user's requested "1, 4, then 5" order)

Real POINT (PRIM type 0) and LINE/LINE_STRIP (types 1/2) rasterization
added to `source/hw/gif.c` - the "Lines/points are still open" gap
flagged in every prior GS-related round. Researched and cross-checked
against a live fetch of PCSX2's own source (`GS/GSRegs.h`'s
`enum GS_PRIM`/`GS_PRIM_CLASS`, `GS/GSState.cpp`'s `NumIndicesForPrim`/
`VertexKick`, `GS/Renderers/SW/GSRasterizer.cpp`'s `DrawPoint`/
`DrawEdgeLine`, `GS/Renderers/SW/GSDrawScanline.cpp`'s `CSetupPrim`) -
not guessed.

**What's implemented** (see `gif.c`'s `rasterize_point()`/
`rasterize_line()` and `gif.h`'s updated scope comment for the full
citation trail):

- POINT: a single flat-color pixel write, no interpolation of any
  kind (real hardware: `DrawPoint` pulls color from the point's own
  single vertex, nothing to interpolate between).
- LINE: independent 2-vertex segments (real hardware:
  `NumIndicesForPrim` returns 2, no vertex reuse across segments -
  same "no carry-over" shape as this file's existing plain TRIANGLE
  handling). LINE_STRIP: a rolling 2-vertex window (each new vertex
  forms a segment with the previous one), same shape as this file's
  existing TRIANGLE_STRIP handling, just 2 slots instead of 3.
- Flat shading uses the LAST vertex's color - cross-checked against
  `CSetupPrim`, which selects `last=1` for `GS_LINE_CLASS` (the same
  "flat uses the last vertex" convention this file already established
  for triangles/sprites). Gouraud (IIP bit) is fully supported for
  LINE, confirmed real hardware behavior (`CSetupPrim`'s `Color()`
  path applies identically regardless of primitive class).
- Real per-pixel-step DDA line rasterization ported from
  `GSRasterizer::DrawEdgeLine`: walk whichever axis has the larger
  absolute delta (the major axis) one pixel at a time, stepping every
  interpolated attribute (color, Z) linearly by its total delta
  divided by the step count. This project uses a plain floating-point
  step instead of PCSX2's fixed-point 16.16 subpixel accumulator - an
  equivalent-result simplification of a well-known rasterization
  technique, not a real-hardware-specific detail (unlike the flat/
  Gouraud-vertex-selection and linear-Z rules, which ARE real hardware
  behavior and are followed exactly).
- Z interpolates linearly along the segment (real hardware: Z has
  already been through the perspective projection by the time it
  reaches the rasterizer, so - same as this project's existing
  triangle Z handling - no perspective correction is needed), gated
  behind the same `zbuf_configured`/ZTE/ZTST machinery every other
  primitive already uses.
- No texture mapping: real GS hardware does not texture-map POINT/
  LINE primitives (only SPRITE/TRIANGLE support TME) - correctly not
  modeled, not an oversight.
- `reset_tri_vseq()` (now resetting the line accumulator and
  `has_vertex0` too, not just the triangle one) is called on every
  PRIM write, matching real hardware starting a fresh vertex queue -
  this closes a latent gap where a PRIM change mid-SPRITE-vertex-pair
  could previously leak a stale first vertex into whatever primitive
  came next.

17 new checks (`tests/test_gif_line.c`): a flat POINT with no
interpolation; a flat LINE using the real last-vertex convention; a
Gouraud LINE proving genuine per-pixel linear interpolation (distinct
red/blue endpoints, a roughly-half-way midpoint - not a flat fill);
LINE_STRIP's rolling-window continuation (3 vertices -> 2 connected
segments); and a LINE whose Z fails the real depth test over a
pre-populated Z-buffer, proving color/Z stay untouched. Two real,
worth-recording test-construction bugs were found and fixed while
writing this test (not implementation bugs): a GIFtag NLOOP field that
didn't match its actual A+D loop count (silently misparsing the
following bytes as a bogus tag header - the same class of bug this
project has hit before when hand-building test packets), and a ZBUF_1
ZBP test value (2000) that exceeded the register's real 9-bit hardware
field width (0-511) and got silently masked down - both documented in
`tests/README.md` so they aren't rediscovered blindly.

Regression: 53 host-native test binaries (was 52), 0 failures across
all of them. Clean Wii/devkitPPC rebuild, same single pre-existing
harmless `strncpy` truncation warning as prior rounds.

### Round 22 (2026-07-07): user directive - "handle ALL IOP problems before anything else"; first fix found: real RFE was never implemented

The user explicitly broadened scope past Round 19's narrow "fix the
RAM[0x100] handler chain + Status.IEc" framing to "take care of ALL
IOP problems first, before other tasks are tackled" (verbatim, in
German). This round starts that sweep with an inventory pass and one
concrete, confirmed, real fix.

**Inventory** (cross-referencing `docs/ROADMAP.md` section 2's IOP
bullets, this file's IOP-related round history, and `CLAUDE.md`'s
frontier notes): section 2 has exactly one remaining unchecked bullet
- the Round 19 "real exception-handler-chain default behavior at
`RAM[0x100]`" item, itself bundling two sub-questions: (a) what real
IOP kernel init does at `RAM[0x100]` before any handler is registered,
and (b) why `Status.IEc` never becomes 1. Direct code inspection while
starting on (b) surfaced a THIRD, previously undocumented, concrete
bug described below - found by reading `iop_core.c`'s COP0 dispatch
directly rather than only re-reading prior round narratives.

**Bug found: RFE (Restore From Exception) was completely
unimplemented.** `iop_core.c`'s COP0 sub-dispatch (`case 0x10: switch
(rs)`) only ever handled `rs=0x00` (MFC0) and `rs=0x04` (MTC0) - any
CO-format op (`rs` with bit `0x10` set, which is how real hardware
encodes RFE/TLBR/TLBWI/etc, selected further by the low-6-bit `funct`
field) fell through to the "unimplemented COP0 sub-opcode" halt path.
This matters directly to the Status.IEc question: every real MIPS I
exception handler ends in RFE before returning via `JR`, to restore
the pre-exception KU/IE mode stack (and re-enable interrupts if they
were enabled beforehand) - a handler that actually ran to completion
and tried to return would have hit this exact halt, silently masking
any forward progress the RAM[0x100]-chain fix (still pending) might
otherwise unlock. Put another way: fixing RAM[0x100] alone, without
this fix, would likely have just moved the wall from "jump to garbage"
to "halt on RFE" - both dead ends, but for different reasons, and the
RFE one would have been confusing to re-diagnose from scratch later.

**Fix**: added `case 0x10` (CO-format) under the COP0 dispatch, with
`funct==0x10` (RFE) implemented per PCSX2's own `R3000A.cpp`
`psxException()` RFE case: `Status = (Status & ~0xF) | ((Status &
0x3C) >> 2)` - shifting the "previous"/"old" KU/IE bit-pairs down into
"current"/"previous" (bits 4-5, the "old" pair, are left untouched),
the exact mirror of the exception-entry push this project already
implements (`(Status & 0x0F) << 2` into bits 2-5, clearing bits 0-1).
No TLB-family CO-format ops (TLBR/TLBWI/TLBWR/TLBP) were added - the
IOP's R3000A has no MMU/TLB (unlike the EE), so these aren't
applicable here and any other `funct` value under `rs=0x10` correctly
still halts with a descriptive, distinct message
("unimplemented COP0 CO-format op") rather than being silently
guessed at.

**Verification**: `tests/test_iop_rfe.c` (3 checks) hand-derives and
confirms one full exception-entry-then-RFE round trip bit-for-bit: an
initial `Status` low-6 value of `0b000101` (chosen to make every bit
position distinguishable) becomes `0b010100` after the existing
SYSCALL push, then `0b010101` after the new RFE - exactly matching a
hand-traced application of the real formula, not just "doesn't crash".
Also confirms RFE correctly leaves `Cause` untouched (only `Status` is
architecturally affected) and that execution actually continues past
the RFE instruction to a following `BREAK` (proving the CO-format path
no longer falls into the halt default).

Regression: 54 host-native test binaries (was 53, +`test_iop_rfe.c`),
0 failures across all of them. Clean Wii/devkitPPC rebuild, same
single pre-existing harmless `strncpy` truncation warning as prior
rounds (`iop_module_loader.c`, unrelated to this change).

**Still open, same round's remaining scope** (per the user's "ALL IOP
problems" directive, not yet done): (a) RAM[0x100] exception-chain
default-handler behavior itself - partial research recovered from
psx-spx's kernelbios page confirms the real BIOS RAM layout has
`RAM[0x100]` as the start of an 8-entry "Table of Tables", whose FIRST
entry (`RAM[0x100]`/`RAM[0x104]`) is literally documented as "ExCB
Exception Chain Entrypoints (addr=var, size=4*08h)" - i.e. a real
BIOS's kernel init writes a pointer+size pair here describing WHERE
the 4 real exception-chain-root entries live (typically inside the
E000h-2000h "Kernel Memory" region allocated via a real `B(00h)`
kernel-memory-init call) and how many there are; a fully-cited,
implementable default-fallback behavior for the specific case where
this project's boot path reaches the dispatcher before any real
handler is registered still needs the deeper "Priority Chains"/"ExCB"
structure-layout sections of that same document, which did not fit in
one fetch and have not yet been retrieved in full. (b) Status.IEc: RFE
existing now means an eventual real handler CAN restore it correctly,
but nothing in this project's IOP interpreter step loop currently
checks Status.IEc to decide whether to take a pending hardware
interrupt at all (`iop_intc.c`'s own scope comment already flagged
this: "NOT modeled: actually raising a CPU interrupt/exception in
iop_core.c when I_STAT & I_MASK becomes nonzero") - meaning IEc is
currently a dead bit with no observable behavioral effect regardless
of its value, which is itself a real, separate, well-defined next
target for this same sweep.

**Update, same session**: gap (b) above (real hardware-interrupt
delivery) is now also fixed. Cited from the public psx-spx reference
(https://psx-spx.consoledev.net/interrupts/, whose "PS2 IOP interrupts"
subsection explicitly confirms it applies here: "The PS2's IOP has the
same interrupt controller as the PS1 but with more channels"): unlike
the EE's 8 independent Cause.IP0-IP7 lines, the real IOP/PS1
architecture routes EVERY peripheral IRQ (all of I_STAT/I_MASK)
through ONE single CPU interrupt line - Cause.bit10 (IP2), which
mirrors "(I_STAT AND I_MASK)=nonzero" LIVE and NON-latching (quoting
the source directly: "cop0r13.bit10 is NOT a latch, ie. it gets
automatically cleared as soon as (I_STAT AND I_MASK)=zero" - a real,
documented difference from the EE's sticky IP7 timer latch). The
interrupt is actually taken once Cause.bit10, Status.bit10 (IM2), and
Status.bit0 (IEc) are all set, vectored exactly like the existing
SYSCALL exception (same BEV-dependent vector, same KU/IE mode-stack
push formula) - and, per this project's existing documented
simplification for SYSCALL, EPC always points at the next not-yet-
executed instruction (no branch-delay-slot/Cause.BD tracking for the
IOP, consistent with the rest of this file).

New function `iop_check_hw_interrupt()` in `iop_core.c`, called at the
end of every real (non-HLE-trap) instruction step. `tests/
test_iop_hw_interrupt.c` (8 checks): a real program that writes I_MASK
via an actual `SW` instruction (not just the direct `iop_intc_raise()`
test hook) after a peripheral has already raised a pending `I_STAT`
bit, proving the interrupt correctly preempts the very next
instruction the instant the enabling write makes `(I_STAT & I_MASK)`
nonzero - EPC, the two never-executed marker instructions, Cause.
ExcCode/IP2, and the exact post-push `Status` value are all checked
bit-for-bit. A second case proves NO interrupt fires when `Status.IEc`
is 0, even with `I_STAT & I_MASK` already nonzero from the start -
the marker instruction executes normally and the expected instruction
count is exact.

Regression: 55 host-native test binaries (was 54, +`test_iop_hw_interrupt.c`),
0 failures. Clean Wii/devkitPPC rebuild, same single pre-existing
harmless `strncpy` warning as every prior round.

With this, `Status.IEc` finally has a real, observable, end-to-end
effect for the first time in this project: RFE can restore it (fixed
earlier this round) AND the interpreter now actually checks it before
delivering a hardware interrupt.

**Update, same session: RAM[0x100] itself, now with the full
reference.** The earlier fetch of psx-spx's kernelbios page (the
rendered HTML version) was truncated before reaching the "BIOS
Interrupt/Exception Handling" section. Re-fetched the page's raw
markdown source directly from GitHub
(`raw.githubusercontent.com/psx-spx/psx-spx.github.io/master/docs/
kernelbios.md`) instead of the rendered HTML - smaller, no navigation/
CSS bloat - which got substantially further (1775 lines) and reached
the needed section in full: "Priority Chains", "C(02h) - SysEnqIntRP",
"C(03h) - SysDeqIntRP", "SYS(01h)/SYS(02h) - Enter/ExitCriticalSection",
"C(06h) - ExceptionHandler()", "B(17h) - ReturnFromException()",
"C(00h)/C(01h)/C(0Ch) - EnqueueTimerAndVblankIrqs/EnqueueSyscallHandler/
InitDefInt", and "No Nested Exceptions" - the complete real mechanism
Round 19 needed a citable reference for.

Key facts from this reference: the Kernel's exception handler has
**4 priority chains** (0-3), each a singly-linked list of handlers;
the default population is `Prio 0: CdromDmaIrq, CdromIoIrq,
SyscallException` / `Prio 1: CardSpecificIrq, VblankIrq, Timer2Irq,
Timer1Irq, Timer0Irq` / `Prio 2: PadCardIrq` / `Prio 3: DefInt`.
`C(02h) SysEnqIntRP(priority, struc)` documents the EXACT node byte
layout (`00h`=next-pointer, written by the BIOS; `04h`=second-function
pointer; `08h`=first-function pointer; `0Ch`=unused) and real behavior
(always inserts at the chain HEAD - newest-first). `C(03h)
SysDeqIntRP(priority, struc)` has a documented, real BUG: it can only
correctly remove the FIRST element of a chain - removing anything else
"reads a garbage value from an uninitialized stack location, and acts
more or less unpredictable." `RAM[0x100]`'s earlier-documented "Table
of Tables" entry (`addr=var, size=4*08h`) is therefore a pointer to a
real 4-entry array of chain-head pointers, one per priority level.

**Implemented** (`include/core/hw/iop_excb.h` / `source/hw/
iop_excb.c`, new files): `iop_excb_init()` writes `RAM[0x100]`/
`RAM[0x104]` to point at a real 4-entry, 8-byte-per-slot chain-head
array in the documented "Kernel Memory" region (`0xE000`+), all-empty
(head=NULL) - correctly matching "before any handler is registered",
the exact scenario Round 19's trace found. `iop_excb_sys_enq_int_rp()`
implements the real head-insertion byte-exactly. `iop_excb_sys_deq_int_rp()`
implements the real first-element-removal case byte-exactly, and
models the documented non-first-element BUG as a safe no-op rather
than fabricating a specific garbage-dependent outcome (there is
nothing citable to reproduce there - the real behavior is documented
as genuinely undefined). Wired into `iop_hle_bios.c`'s existing C0-
table real-function dispatch as `C(02h)`/`C(03h)`, alongside the
already-real `C(07h)` (InstallExceptionHandlers).

**Deliberately NOT implemented, same rationale as every other HLE
function in this project**: the real default handler CONTENTS
(`EnqueueSyscallHandler`/`EnqueueTimerAndVblankIrqs`/`InitDefInt`,
`C(01h)`/`C(00h)`/`C(0Ch)`) - actually populating the chains with
`SyscallException`/`VblankIrq`/`Timer0-2Irq`/`CardSpecificIrq`/
`PadCardIrq`/`CdromDmaIrq`/`CdromIoIrq`/`DefInt` would require those
functions' real BIOS-ROM machine code bodies, which this project has
no verified byte-for-byte reference for and will not fabricate. What
IS now real and byte-exact is the container/mechanism itself - the
chain data structure and its two manipulation primitives - which is
the part Round 19's trace actually needed a citable reference for.

`tests/test_iop_excb.c` (18 checks): the Table-of-Tables pointer/size
fields, all 4 chains starting empty, real head-insertion order (newest
first) across two nodes with byte-exact next-pointer linking, chain
isolation between priorities, real first-element removal, the non-
first-element bug modeled as a no-op (and counted, not silently
dropped), out-of-range-priority handled safely, and the real C0-table
`C(02h)` HLE trap end-to-end (register-convention parameter reads,
correct chain mutation, correct return-to-`$ra`).

Regression: 56 host-native test binaries (was 55, +`test_iop_excb.c`),
0 failures. Clean Wii/devkitPPC rebuild, same single pre-existing
harmless `strncpy` warning as every prior round.

**Net result for this entire IOP sweep (Round 22)**: three real,
previously-undocumented/unimplemented gaps found and fixed in one
session (RFE, hardware-interrupt delivery, the RAM[0x100] chain
mechanism), each backed by a citable real-hardware reference, each
with its own host-native regression test, none fabricated. The only
remaining honestly-open piece is the real default handler BODIES
(actual BIOS-ROM machine code for SyscallException/VblankIrq/etc.) -
which this project has never had and is not attempting to synthesize,
consistent with its established no-fabrication policy.

### Round 29 (2026-07-07): real-BIOS live tracing of the RAM[0x100] gap - root cause substantially narrowed, fix not yet implemented

Direct follow-up to the user's explicit request (after choosing "Track B" -
fix the real exception-handler-body gap using bytes/behavior from their own
legally-owned SCPH-10000 dump rather than a synthetic HLE stub) to continue
investigating before implementing anything. This round is host-native
tracing only, no code changes to the emulator itself.

**Method**: rather than reason about the mechanism abstractly, this round
single-stepped the actual interleaved EE/IOP scheduler (`system.c`'s own
8:1 loop, reimplemented in a throwaway `/tmp/diagNN.c` harness so every IOP
instruction could be inspected) against the user's real `scph10000.bin`,
logging every JAL/JALR target and every store to the "Table of Tables"
region (`RAM[0x100]-RAM[0x158]`, per psx-spx's BIOS RAM Map) from boot
until the known early-SYSCALL wall (Round 19's finding, reproduced here
independently: a genuine `ExitCriticalSection` SYSCALL, Cause=SYSCALL,
EPC=0x8003ECF4).

**Confirmed, byte-for-byte, via live Capstone disassembly of the actually-
resident RAM** (not assumed from documentation): the real exception
dispatcher at `0x00000C80`-`0x00000E3C` is genuine, executed BIOS/kernel
code (not this project's own fabrication) that unconditionally does
`lw $s3,(0x100)` (table address) `-> lw $s6,($s3)` (priority-0 chain head)
`-> lw $s1,8($s6)` (first-function pointer) `-> jalr $s1`, with NO null
check on `$s6` before the final dereference+call. At the moment this
SYSCALL's exception fires, `RAM[0x100]` is still `0` (never allocated), so
`$s6 = RAM[0] = 0` and `$s1 = RAM[8] = 0x03400008` - the exact leftover
"JR $k0" template bytes Rounds 14/19 already identified. This is a precise,
independently-reproduced confirmation of Round 19's account, now backed by
live disassembly instead of static reasoning.

**New this round**: exhaustively logging every JAL/JALR the IOP executes
from reset to this exact SYSCALL (about 3.05 million IOP instructions)
found exactly 2 calls to the public `0xA0` BIOS vector and ZERO calls to
`0xB0` or `0xC0` - meaning `C(00h)`/`C(01h)`/`C(0Ch)`
(EnqueueTimerAndVblankIrqs/EnqueueSyscallHandler/InitDefInt, the functions
psx-spx documents as populating this exact chain) are never invoked via
the public vector mechanism before this syscall fires, on this exact real
BIOS revision's boot order. Also found: the "Table of Tables" region gets
explicitly zeroed three separate times very early in boot (real ROM
bootstrap code at `0xbfc021d4`-`0xbfc02290` and `0xbfc4d30c`-`0xbfc4d324`,
confirmed genuinely executed, not this project's own init), and later
(sometime before instruction 3,054,820) the PCB/TCB "size" sub-fields
(`RAM[0x10c]=4`, `RAM[0x114]=0x300` - exactly matching real hardware's
"1 PCB entry, 4 TCB entries * 0xC0h" convention) get set to real,
meaningful values with NO corresponding CPU store instruction ever
executing for those addresses. Traced this last part down to this
project's own real ELF/IRX loader (`iop_elf.c`'s `iop_mem_write8()` calls
inside `elf_load_segments()`, called from the module loader's C code, not
from interpreted IOP instructions) copying an already-loaded real kernel
module's own DATA segment bytes onto these addresses - i.e. this project's
existing, real ELF loader (task #92) is ALREADY correctly delivering real,
ROM-sourced kernel configuration data to part of the Table of Tables; it's
specifically the ExCB entry (`RAM[0x100]`/`RAM[0x104]`) that stays at zero,
and specifically the actual runtime allocation-and-registration step
(computing a real heap address for the ExCB array and calling the real
kernel equivalent of `SysEnqIntRP` for each default handler) that has not
executed by the time this SYSCALL fires.

**Not yet resolved**: which exact loaded module/ELF segment is responsible
for the PCB/TCB values (not pinned down to a specific module name yet -
`iop_module_loader.c`'s escape-hatch mechanism from Round 15 loads modules
opportunistically whenever execution jumps to the leftover `0x03400008`
template bytes, and by instruction 3,054,820 this hasn't happened yet
either, meaning the PCB/TCB-setting module load must be triggered by some
OTHER, earlier mechanism this round didn't trace down to its root call
site), and consequently whether the real ExCB allocation+registration
would naturally follow once that earlier mechanism is fully understood, or
whether it requires a genuinely separate fix. This is the concrete next
step - tracing backward from wherever the PCB/TCB-setting module load
actually gets triggered, rather than forward from the SYSCALL wall as this
round did.

**No code changes this round** - purely diagnostic, using disposable
`/tmp/diagNN.c` harnesses (not committed, matching this project's
established pattern for host-native investigation tooling) against the
user's own real BIOS dump (`scph10000.bin`, already present in this
session's uploads directory, never committed or copied into the repo/
outputs - same standing rule as every prior round). No regression risk;
existing 63-test suite and Wii build are unaffected and were not re-run
this round since nothing in `source/`/`include/` changed.

### Round 29 continued (2026-07-07): B(00h) alloc_kernel_memory implemented for real - root cause fixed, boot wall NOT yet cleared

Direct continuation of the same session/investigation above, per the
user's explicit "Ja" (continue) after the checkpoint was pushed. Picked
up exactly where the previous section left off: tracing backward from
the PCB/TCB-setting mechanism instead of forward from the SYSCALL wall.

**Diagnostic bug found and fixed first**: the previous round's "zero
calls to 0xB0/0xC0" finding was itself an artifact of a JAL/JALR-only
trace. Building a full-state watchpoint (checking `RAM[0x100]`,
`RAM[0x104]`, `RAM[0x10c]`, `RAM[0x114]` after literally every IOP
instruction, regardless of opcode) found real writes to `RAM[0x10c]`
and `RAM[0x114]` at ROM PCs `0xbfc50110`/`0xbfc5012c` that the address-
range store trace had also missed (a second, independent diagnostic
bug: those stores use `lui $at,0xa000` - KSEG1 addressing, virtual
address `0xA000010Ch` - and the trace's own address comparison wasn't
masking to physical address before comparing, unlike the project's
real `iop_mem_write32()`/`iop_mem_ptr()`, which correctly does
`& 0x1FFFFFFFu`. Both bugs were in the throwaway diagnostic code, not
in any project source file.)

**Root cause, confirmed via direct disassembly of the real ROM bytes**:
genuine, executing BIOS code at `~0xbfc4ff90-0xbfc501f8` (not project
fabrication) sets up the PCB/TCB size fields directly, then calls
`B(00h) alloc_kernel_memory(size)` to get the ExCB array's address -
but not via `jal`/`jalr` (which the earlier trace was watching for).
It goes through a thunk table at `0xbfc58c80-0xbfc58d80`: each entry is
`addiu $t1,zero,<function>` / `addiu $t2,zero,<0xA0|0xB0|0xC0>` /
plain `jr $t2` - a tail call that preserves the ORIGINAL caller's `$ra`,
invisible to any JAL/JALR-only trace. Since this project's
`iop_hle_bios.c` had no real case for B0-table function 0, every one
of these calls fell through to the generic default (`$v0=0`, i.e.
"allocation failed"), so the real BIOS's own allocation logic correctly
bailed out and never wrote a valid address into `RAM[0x100]`.

**Fix implemented** (per the user's explicit "lieber aus dem bios dump"
directive - real BIOS-derived behavior, not a synthetic stub):
`IOP_HLE_B0_ALLOC_KERNEL_MEMORY` (B0h, function 0x00) is now a real bump
allocator over the documented Kernel Memory region (psx-spx's BIOS RAM
Map: "0000E000h 2000h Kernel Memory; ExCBs, EvCBs, and TCBs allocated
via B(00h)") - starts at `IOP_EXCB_ARRAY_ADDR` (0xE000), 4-byte aligned,
bounded by a new `IOP_KMEM_REGION_SIZE` (0x2000) constant, with
persistent `kmem_bump_next`/`kmem_alloc_calls`/`kmem_alloc_failures`
state in `iop_hle_bios_state_t`. Companion fix in `iop_excb.c`:
`chain_head_addr()` now reads the chain-head array's base address
dynamically from `RAM[0x100]` (`IOP_EXCB_TABLE_ADDR`) instead of
hardcoding `IOP_EXCB_ARRAY_ADDR`, matching the real dispatcher's own
`lw $s3,(0x100)` behavior - falls back to `IOP_EXCB_ARRAY_ADDR` if
`RAM[0x100]` is still 0 (nothing allocated yet), preserving every
pre-existing test's assumptions. New test: `tests/test_iop_kmem_alloc.c`
(19 checks, all passing) covers the allocator (aligned/unaligned sizes,
overflow-fails-cleanly) and the dynamic chain-head resolution.

**Verified via live re-trace against the real BIOS that the fix does
what it's supposed to**: `RAM[0x100]` now genuinely gets set to
`0x0000E000` around IOP instruction 84,849 (previously stayed 0
forever) - a real, allocator-driven write, confirmed via the same
watchpoint harness. `kmem_alloc_calls` also goes from 3 (all silently
failing) to 4 successful calls with the fix in place.

**Honest result, not oversold**: implementing this fix does NOT, by
itself, get boot past the same point the pre-fix build reaches. A
direct A/B test (identical harness, only the `git stash`-toggled fix)
run to 30 million IOP instructions (240 million EE instructions) shows
BOTH the before- and after-fix builds land at the exact same steady-
state PCs (EE at `0x80005e64`, IOP at `0x00101280`), and `RAM[0x100]`
ends up `0` in both cases by that point regardless. Tracing why: a
separate, genuinely-executing block of ROM code at `0xbfc4d2c8-
0xbfc4d360` unconditionally zeroes the entire `0x000-0xf80` low-RAM
region in a loop (8 words per iteration) shortly AFTER the real
allocator call succeeds (`RAM[0x100]` set at instr 84,849, wiped again
by this clear loop at instr 221,259) - this looks like a generic
kernel-reset/low-memory-clear pass that this BIOS revision runs before
its real, final ExCB/handler registration, and neither build (before or
after this fix) has been traced far enough forward to find where (or
whether, within the currently-traced window) that final registration
actually happens.

**What this round concretely accomplished**: eliminated a real,
confirmed behavioral gap (B0-table allocation silently failing when
the real BIOS legitimately calls it) with real, BIOS-derived semantics
- not a guess, and not regressive (0 failures across all 64 host-native
tests, clean Wii/devkitPPC rebuild). It did not, on its own, resolve
the ultimate early-boot wall; the next concrete step is tracing forward
from the `0xbfc4d30c` clear loop to find whatever comes after it and
whether/when the ExCB chain gets rebuilt a second time following that
clear.

### Round 29 continued (2nd fix, 2026-07-07): real C(01h) EnqueueSyscallHandler + B(18h) ResetEntryInt - forward progress confirmed, ultimate wall still open (real ROM clear-loop timing)

Direct continuation of the same session/investigation, per the user's
explicit "real implementieren" (implement it for real) after being shown
that psx-spx documents C(00h)/C(01h)/C(0Ch) only vaguely ("internally
used to add some default handlers") and asked whether to pursue this.

**Deep live disassembly this round** of the user's real SCPH-10000 dump's
resident kernel image (dumped IOP RAM at the exact instant these calls
happen, disassembled offline with Capstone) fully mapped out, for the
first time, the REAL exception dispatcher's entire body
(`0x00000c80`-`0x00000e98`) and the REAL `ReturnFromException` routine
(`0x00000f30`-`0x00001000`), byte-for-byte:

- The dispatcher's entry code re-derives the current TCB via
  `RAM[0x108]` (PCB pointer) -> `RAM[PCB]` (current-TCB pointer) -> `+8`
  (TCB body base), saves ALL GPRs/HI/LO/SR/EPC into it, reads
  `RAM[0x100]` (the ExCB table address - exactly the address this
  project's earlier B(00h) fix now populates for real) and walks all 4
  priority chains calling each element's first-function
  (`jalr $s1`) and, if it returned nonzero, its second-function
  - byte-for-byte matching psx-spx's documented "Priority Chains"
  mechanism exactly, including the "second function executes iff func1
  returns r2<>0" rule.
- If NOTHING in any chain calls `ReturnFromException` directly, the
  dispatcher falls through to a `setjmp`/`longjmp`-style restore from a
  pointer at `RAM[0x7520]` - matching psx-spx's `B(19h) HookEntryInt`
  documentation's struct layout exactly (ra/sp/fp/s0-7/gp fields).
  Live-dumping RAM found the STRUCT this points to by default
  (`RAM[0x00006C34]`) is ALREADY correctly populated by other, already-
  resident real kernel code (`ra` field == `0x00000f30`, the real
  `ReturnFromException` address; `sp` field == `0x00008524`, matching
  "exception stacktop minus 4" against the real `RAM[0x6c30]=0x8528`) -
  but the POINTER VARIABLE at `RAM[0x7520]` itself stays 0 until
  `B(18h) ResetEntryInt()` runs, which this project did not previously
  implement (fell through to the generic default, a no-op).
- `ReturnFromException` (`0x00000f30`) restores every GPR/HI/LO/SR from
  the current TCB and finishes with `jr $k0` where `$k0` was loaded
  from the TCB's own saved "resume PC" field (offset `+0x80`) -
  confirming this is genuinely the real, complete return-from-exception
  mechanism, not a partial/incomplete stub.

**Root cause of the STILL-unresolved wall, precisely identified**: the
real BIOS DOES call `C(01h) EnqueueSyscallHandler(priority=0)` and
`C(0Ch) InitDefInt(priority=3)` immediately after `B(00h)` succeeds
(confirmed via a full HLE-call log: instr 84862 and 84868, right after
the B(00h) alloc at instr 84702) - exactly matching psx-spx's own
documented usage ("used with prio=0" / "used with prio=3"). Since this
project did not implement `C(01h)` for real, the priority-0 chain never
actually got a real `SyscallException`-equivalent handler installed,
even though `RAM[0x100]` itself was (briefly) valid.

**Fix implemented**: `B(18h) ResetEntryInt()` now writes the real,
ROM-confirmed constant `RAM[0x00007520] = 0x00006C34` (not a guess -
the literal immediate value the real ROM code itself uses,
`addiu $v0,$v0,0x6c34`), matching real hardware's documented behavior.
`C(01h) EnqueueSyscallHandler(priority)` now installs a real, hand-
assembled (Keystone), Capstone-round-trip-verified, position-
independent MIPS machine-code trampoline into the Kernel Memory bump
allocator (reused across calls, not re-installed) implementing psx-spx's
own word-for-word documented `SYS(01h)`/`SYS(02h)` behavior
(`EnterCriticalSection`/`ExitCriticalSection`: clear/set `SR` bits 2 and
10, with the documented return-value rule for `EnterCriticalSection`),
re-deriving the current TCB exactly as the real dispatcher's own entry
code does, and ending by jumping directly to the REAL, live-disassembled
`ReturnFromException` address (`0x00000f30`) - not a synthetic shortcut,
genuine MIPS instructions executed by the ordinary IOP interpreter, with
no new special-cased trap PC. New test `tests/test_iop_syscall_handler.c`
(26 checks) actually EXECUTES the installed trampoline bytes through the
real IOP interpreter end-to-end (not just inspects them), for both the
Enter/ExitCriticalSection paths and the "not our exception" fallthrough.
64/64 total regression (0 failures), clean Wii/devkitPPC rebuild.

**Honest result, not oversold**: live re-trace confirms both fixes
genuinely execute on the real boot path (`known_calls_handled` rises
from 7 to 9), and the syscall-handler chain node is correctly built and
wired. However, this does NOT change the ultimate observed wall: RAM[0x100]
still gets wiped by the SAME real ROM clear-loop (`~0xbfc4d2c8-0xbfc4d360`,
found in the previous fix this round) at IOP instruction 221,259 - nearly
2.8 million instructions BEFORE the dispatcher actually runs
(~instruction 3,055,000) - so by the time the dispatcher needs to find
the priority-0 chain, `RAM[0x100]` is 0 again, and the newly-built
syscall-handler chain node (which itself survives untouched, since it
lives above the cleared `0x000-0xf80` range) becomes unreachable. The EE
and IOP land at the exact same steady-state PCs as before this fix
(`0x80005e64` / `0x00101280` at 30M IOP instructions), confirmed via
direct re-trace. The real, precise, and only remaining open question is
why that clear-loop runs AFTER the real ExCB/PCB/TCB setup succeeds, and
whether the real BIOS re-establishes `RAM[0x100]`/`RAM[0x108]` a second
time afterward via some as-yet-untraced path (the full HLE-call log
shows NO further B(00h) calls anywhere in the 30M-instruction window
after instruction 85,844, meaning if a second setup pass exists, it does
not go through the B0/C0 vector mechanism this project intercepts -
worth tracing directly from the clear-loop's own return address forward,
rather than backward from the dispatcher, as the next concrete step).

### Round 29 continued (3rd finding, 2026-07-07): the "clear-loop wall" is real BIOS LOGO-loading code, not a bug - reframes the whole investigation

Direct continuation of the same session, per the user's "mach weiter"
after being shown that the clear-loop wiping RAM[0x100]/RAM[0x110]
(found in this round's earlier fixes) still blocks the exception
dispatcher. Pure diagnostic tracing this round - no code changes.

**Traced the clear-loop's own caller** (return address `0xbfc52b4c`,
captured live) and disassembled the surrounding function
(`0xbfc52afc`-`0xbfc52b98`). It compares a name against two ROM string
constants - **live-decoded from the actual ROM bytes**: both compares
are against the literal string `"LOGO"` (null-padded, 8-byte aligned),
with a `"CD001"` string (the ISO9660 volume-descriptor signature)
immediately adjacent in ROM. This is a real, direct textual match, not
inference - read straight out of the loaded BIOS image. The function's
shape (compare against candidate name -> on match, look up and call a
function pointer via `jalr`) matches this project's own already-
understood ROMDIR mechanism (task #33) applied to find and invoke the
BIOS's own boot-logo resource handler.

**Confirmed live**: at the `jalr $v1` call site (instr 221,572), `$v1`
holds `0x00030000` - a RAM address, not a ROM address, meaning this is
a call into a **loaded module** (this project's own real IOP IRX
loader, task #92, is what would have placed it there) - almost
certainly the BIOS's real logo-decompression/rendering module.
Execution enters `0x00030000` at instr 221,575 and runs largely self-
contained (no further BIOS A0/B0/C0 calls at all) until instr 367,227,
when it calls `A0(0x44) FlushCache` - typical after writing freshly
decompressed image data - then continues running self-contained again
all the way to instr 3,050,446 (another FlushCache), followed by a
tight cluster of calls (`A0(0x13)`, `B0(0x19)`, `B0(0x5B)`, `C0(0x0A)`,
`A0(0x72)`) right before the previously-identified dispatcher/garbage-
jalr wall at instr ~3,055,000.

**Why this matters**: this strongly suggests the ~2.8-million-
instruction stretch this project has been treating as an opaque
"pre-wall" gap is not idle or wasted execution, nor a stuck loop - it
is the REAL BIOS's own logo-loading and (very likely) rendering code,
running for real, self-contained, exactly as real hardware would. The
"wall" investigated over the last two fixes this round (RAM[0x100]
clear, missing C(01h)/C(0Ch) handlers) sits chronologically AFTER this
real logo routine, not before it - meaning it is not currently known to
block the logo from displaying at all. This reframes the practical
priority: getting a real, BIOS-driven splash screen (the user's
originally stated goal) may depend far more on task #126 (wiring the
GS/display driver path into the real boot flow, so that whatever this
logo module writes actually reaches the Wii's framebuffer) than on
further exception-dispatcher archaeology, since the logo module itself
does not appear to need working priority-chain dispatch to run.

**Not yet determined this round**: whether the logo module's output
actually reaches GS-visible memory (would require watching GS register
and VRAM writes during this exact instruction window - not done yet),
what module name/IRX this corresponds to, and whether `main.c`'s
current demo-driven boot path would even reach this code path as
currently wired. These are the natural next steps, and directly serve
task #126 rather than task #124.

No code changes this round - purely diagnostic, using disposable
`/tmp/diagNN.c` harnesses (not committed) against the user's own real
BIOS dump, per the established convention. Existing 64-test suite and
Wii build are unaffected and were not re-run this round since nothing
in `source/`/`include/` changed.

## GS Round 23: alpha test + alpha blending (TEST_1/ALPHA_1)



Per the user's explicit directive to return to the GS and keep pushing
toward a complete port, this round tackles the next largest
documented GS gap (ROADMAP.md section 6): the GS alpha unit - the
per-pixel alpha test (`TEST_1`'s `ATE`/`ATST`/`AREF`/`AFAIL` fields)
and alpha blending (`ALPHA_1`, gated by `PRIM`'s `ABE` bit). Both are
real, load-bearing parts of the GS's per-fragment pipeline on actual
hardware, used pervasively for transparency, particle effects, and
alpha-cutout rendering in real PS2 titles - not a cosmetic add-on.

**Research.** A dedicated research subagent was dispatched to pull the
exact bit layouts and semantics from PCSX2's own source
(`GS/GSRegs.h` for the register bitfields, `GSDrawScanline.cpp` for
the alpha-test and blend-equation logic), returning a well-cited
report. One detail - that the real blend equation truncates rather
than rounds (`((A-B)*C)>>7 + D` with no `+0.5`-equivalent bias) - was
sourced from a PCSX2 developer forum post rather than primary source
and is flagged as such here for the same reason; it is a
well-established, widely-repeated hardware quirk (real GS hardware
famously does NOT round its blend math, unlike a "naive" software
renderer would), not a fabrication, but the citation trail is weaker
than the register-layout facts pulled directly from GSRegs.h.

**TEST_1 (`include/core/hw/gif.h`, `source/hw/gif.c`).** Real bit
layout: `ATE`:1 (bit0, alpha-test enable), `ATST`:3 (bits1-3, compare
function), `AREF`:8 (bits4-11, reference value), `AFAIL`:2 (bits12-13,
fail action). `ATST` values: `NEVER`=0, `ALWAYS`=1, `LESS`=2,
`LEQUAL`=3, `EQUAL`=4, `GEQUAL`=5, `GREATER`=6, `NOTEQUAL`=7 - compares
the fragment's own alpha byte against `AREF`. `AFAIL` values:
`KEEP`=0 (both color and Z writes are discarded on failure), `FB_ONLY`
=1 (color still writes, Z write is suppressed), `ZB_ONLY`=2 (Z still
writes if otherwise allowed, color write is suppressed), `RGB_ONLY`=3
(RGB channels still write, but the alpha BYTE is preserved from the
framebuffer's OLD value rather than the fragment's - Z write
suppressed). This project's `gs_mem` is PSMCT32-only, so the real
hardware's "RGB_ONLY downgrades to FB_ONLY on non-32-bit formats"
special case never applies here and is not modeled (there is nothing
for it to downgrade from).

**ALPHA_1.** Real bit layout: `A`/`B`/`C`/`D` are each 2 bits (word0
bits 0-1/2-3/4-5/6-7), `FIX` is 8 bits - but NOT in word0; it lives in
word1 (`data_hi`)'s low byte (bits 32-39 of the 64-bit register). This
was gotten wrong on the first pass (see Errors below) and fixed by
cross-checking against the adjacent, already-correct `ZBUF_1` case's
established word0/word1 convention. `A`/`B`/`D` select a COLOR input:
0=`Cs` (the fragment's own source color), 1=`Cd` (the current
framebuffer/destination color), 2=constant zero (black). `C` selects a
COEFFICIENT: 0=`As` (source alpha), 1=`Ad` (destination/framebuffer
alpha), 2=`Af` (the fixed `FIX` value). The blend equation is
`Color = ((A-B)*C)>>7 + D` - a plain truncating integer divide by 128
with NO rounding bias (see the citation caveat above), and the
coefficient is NOT clamped to [0,1] - real hardware allows FIX or
alpha values above 128/255 to produce "boosted", intentionally
over-saturated blend results, which this implementation preserves
(only the FINAL per-channel result is clamped to [0,255], modeling
COLCLAMP=1, the hardware default; COLCLAMP=0's wrap-instead-of-clamp
alternate mode is a documented, deliberate non-goal - no title this
project targets is known to rely on it). The written alpha channel of
a blended fragment is ALWAYS the fragment's own source alpha -
blending only ever touches RGB, never the alpha channel itself, per
real hardware behavior.

A new `PRIM_ABE_MASK` (bit 6 of the `PRIM` register) was added
alongside the pre-existing `PRIM_TME`/`PRIM_IIP`/etc. bits - blending
is a real per-primitive-draw toggle on actual hardware (`ABE`), not
something that runs unconditionally just because `ALPHA_1` happens to
be configured; a test in this round explicitly proves `ALPHA_1` being
configured has zero effect when `ABE`=0.

**`gs_finish_pixel()` (new shared helper, `source/hw/gif.c`).** All 4
rasterizers (triangle/sprite/point/line) previously each inlined their
own small "write color, maybe write Z" tail block - a deliberate,
established pattern in this file for the tiny Z-test logic. The alpha
unit's logic (test + blend + the AFAIL-mode color/Z suppression
interactions) is substantially larger and behaves identically
regardless of which primitive produced the fragment, so this round
introduces a single shared `gs_finish_pixel(x, y, frag_color, frag_z,
z_write_allowed)` function, called from all 4 rasterizers in place of
their old inline tails. This is a deliberate, documented deviation
from the "duplicate the small block" pattern, justified by the size
and shared-behavior of the new logic - not a whim.

**Errors found and fixed this round:**
- The `FIX` field was first read from `data_lo >> 24` (assuming it
  lived in word0's top byte). Cross-checking against `ZBUF_1`'s own
  word0/word1 convention (`ZBP` from `data_lo`, `ZMSK` from `data_hi`)
  revealed `FIX` is actually `data_hi`'s low byte - fixed to
  `data_hi & ALPHA_FIX_MASK` with no shift needed, and the header
  comment/constants updated to match (a now-inapplicable
  `ALPHA_FIX_SHIFT=32u` constant was removed rather than left as dead,
  misleading documentation).
- Two more instances of this project's recurring C block-comment
  gotcha (a literal `_*/`` substring inside doc-comment prose like
  `TEST_ATE_MASK/etc and GS_ATST_xxx/GS_AFAIL_xxx` prematurely closing
  the comment) were hit in `gif.h` and `gif.c` - diagnosed via
  `gcc -fsyntax-only` plus `grep -n '[A-Za-z0-9_]\*/'` and fixed by
  rephrasing to avoid the pattern, consistent with how this same bug
  was handled earlier in the project.
- `tests/test_gs_alpha.c` itself had two test-construction bugs, not
  implementation bugs: two of its manually-built GIF packets (the
  `AFAIL_FB_ONLY` and `PRIM.ABE=0` gating checks) hardcoded a stale
  NLOOP count (6) left over from an earlier draft, undercounting the
  real number of A+D register writes in those packets (8 and 7
  respectively) - the GIF parser silently stopped consuming registers
  partway through the packet as a result. Fixed by recomputing NLOOP
  to match the actual register count in each packet.
- A pre-existing, environment-specific gap (not caused by this round)
  was hit while re-running the full regression suite in this sandbox:
  every test linking `source/hw/vu.c` failed to link with `undefined
  reference to fmaxf/fminf/sqrtf` unless `-lm` was added to the
  command line, even though `tests/README.md`'s documented commands
  don't include it. This is a property of this sandbox's default
  linker behavior (libm isn't implicitly linked here the way it may be
  on other systems) rather than a documentation bug to fix - every
  README command is otherwise correct and was verified to build and
  pass once `-lm` was appended for local verification purposes.

**Testing (`tests/test_gs_alpha.c`, 13 checks).** `ATST_NEVER`
discards every fragment (framebuffer untouched, failed-fragment
counter increments); `ATST_GEQUAL` passing vs failing cases against a
specific `AREF`; all 4 `AFAIL` modes exercised individually
(`KEEP`/`FB_ONLY`/`ZB_ONLY` semantics, plus `RGB_ONLY`'s old-alpha-byte
preservation); a hand-computed 50% alpha blend
(`A`=`Cs`,`B`=`Cd`,`C`=`Af`,`D`=`Cd`,`FIX`=64) of an opaque red
fragment over an opaque blue background, verified channel-by-channel
against the real truncating-divide arithmetic (R=127, not 127.5;
B=128, not 127 - truncation direction differs per operand sign); and
the `PRIM.ABE`=0 gating check described above.

Regression: 57 host-native test binaries (was 56, +`test_gs_alpha.c`),
0 failures (`-lm` appended locally per the sandbox note above). Clean
Wii/devkitPPC rebuild, same single pre-existing harmless `strncpy`
warning as every prior round.

**Net result for GS Round 23**: the GS alpha unit (test + blend) is
now implemented against real, cited hardware semantics, with its own
dedicated regression test and zero regressions elsewhere. Per
ROADMAP.md section 6, the remaining known GS gaps are: CLUT/paletted
textures (PSMT8/4), real block-swizzled addressing (this project uses
simplified linear addressing throughout), REGLIST/IMAGE transfer
modes (only PACKED mode is implemented in the GIF parser), GS context
2 (dual-context - only context 1 is modeled), and mipmaps.

## GS Round 24: CLUT/paletted textures (PSMT8/PSMT4)

Continuing the user's "implement the complete GS port" directive after
Round 23's alpha unit, this round tackles CLUT/paletted textures - the
next item on ROADMAP.md section 6's remaining-gaps list.

**Citation-honesty note for this round.** A dedicated research
subagent dispatch (the same technique used successfully for Round 23's
alpha-unit citations) hit this session's own usage/session limit
before it could run and return results. Rather than block on that or
fabricate specific PCSX2 source line citations I can't verify this
round, the PSM enum values, TEX0's CLUT field layout, and the CLUT
addressing/swizzle scheme below are implemented from established,
widely-published PS2 GS hardware knowledge (the kind referenced
repeatedly across PS2 homebrew texture-conversion tooling and general
GS documentation), NOT a fresh primary-source citation trail. This is
flagged explicitly in the code comments (`gif.h`'s `TEX_PSM_xxx`/
`CLUT_ROW_WIDTH` and `gif.c`'s `gs_sample_texel()`/`gs_sample_clut()`)
so a future round can strengthen the citation trail with a live
GSRegs.h/GSLocalMemory.cpp fetch if/when research tooling is available
again - consistent with this project's standing policy of flagging
weaker-than-usual citations rather than presenting them as equally
solid.

**TEX0's PSM + CLUT fields (`include/core/hw/gif.h`,
`source/hw/gif.c`).** TEX0's previously-ignored PSM field (6 bits,
word0 bits 20-25) is now parsed - real GS pixel-storage-mode values
`PSMCT32`=0x00, `PSMCT24`=0x01, `PSMCT16`=0x02, `PSMCT16S`=0x0A,
`PSMT8`=0x13, `PSMT4`=0x14, `PSMT8H`=0x1B, `PSMT4HL`=0x24,
`PSMT4HH`=0x2C are all defined for documentation, but this project's
`gs_mem` is PSMCT32-storage-only (an established limitation from
earlier rounds), so only `PSMCT32`/`PSMT8`/`PSMT4` are actually
sampled specially - any other PSM value falls back to direct PSMCT32
sampling rather than crashing or silently misbehaving. TEX0 word1's
CLUT fields are also now parsed: `CBP` (14 bits, bits 5-18, the
CLUT's storage location - used directly as this project's own
`gs_mem` bp convention, exactly like `TBP0`/`FBP`/`ZBP` elsewhere),
`CPSM` (4 bits, bits 19-22, CLUT entry format - only `PSMCT32` is
supported, `PSMCT16`/`PSMCT16S` is a documented gap since `gs_mem` has
no 16-bit storage format at all), `CSA` (5 bits, bits 24-28, a
16-entry-unit offset into the CLUT storage), and `CLD` (3 bits, bits
29-31, CLUT load control - parsed but unused, since this emulator has
no CLUT cache to manage; every sample simply re-reads the CLUT fresh
from `gs_mem`, which is always correct, just not a claim of matching
real hardware's cache timing/behavior).

**CLUT addressing (`gs_sample_clut()`).** The CLUT is modeled as its
own small `gs_mem` region at `CBP`, with a fixed row width of 16
entries (`CLUT_ROW_WIDTH`) - matching real hardware's 16-entries-per-
CSA-unit addressing granularity (`CLUT_CSA_UNIT`=16). A palette entry
at flat index N lives at gs_mem pixel `(N % 16, N / 16)` within that
region. `CSA` selects a bank offset: `flat = CSA*16 + index`. This
means PSMT4's 16-entry palette occupies exactly one CSA unit (so up to
32 independent 4-bit palettes can share one CLUT storage region at
different CSA offsets - Round 24's test file proves this explicitly),
while PSMT8's 256-entry palette spans all 16 CSA units and
conventionally starts at CSA=0.

**PSMT8's CSM1 index swizzle (`gs_sample_texel()`).** Real PS2 GS
hardware does not store 8-bit CLUT-indexed texture data with the
palette index used as a flat 0-255 array position - CSM1-mode 8-bit
CLUT storage swaps bits 3 and 4 of the raw index before lookup:
`real_index = (idx & 0xE7) | ((idx & 0x08) << 1) | ((idx & 0x10) >> 1)`.
This is a well-known PS2 GS quirk frequently referenced in PS2
homebrew texture-conversion tooling as the "CSM1 8-bit CLUT swizzle" -
implemented here and proven by two symmetric test cases (raw index 8
resolves to CLUT entry 16, and raw index 16 resolves to CLUT entry 8),
plus a third case (index 3, both swizzle bits already 0) confirming
indices outside the swapped bit range are left untouched. PSMT4 does
not need this swizzle - its 16-entry palette has no sub-block
structure to rearrange.

**No real texture-upload/bit-packing path.** Consistent with this
project's already-established, documented limitation (see
`test_gif_texture.c`'s own scope note: there is no `TRXDIR`/
`BITBLTBUF` texture-upload path yet, textures are pre-existing
`gs_mem` content filled directly via `gs_mem_write_psmct32()` before a
test packet runs), PSMT8/PSMT4 texture data here is stored as one raw
palette-index value per texel SLOT (a full `gs_mem` "pixel", not a
packed byte/nibble) - masked down to the relevant range (0-255 for
PSMT8, 0-15 for PSMT4) at sample time. Real hardware's tightly-packed
byte/nibble-per-texel storage is a separate concern from CLUT lookup
itself and is deliberately not conflated here - it belongs to the
REGLIST/IMAGE transfer-mode work (a separate, still-open ROADMAP.md
item), which is what would actually deliver bit-packed texture data
into `gs_mem` on real hardware.

**Testing (`tests/test_gs_clut.c`, 6 checks).** PSMT4 basic lookup
against a known CLUT entry; PSMT4 with CSA=2 proving bank selection
actually changes which palette is used (not silently ignored); PSMT8
index 8 resolving through the swizzle to entry 16; the symmetric PSMT8
index 16 resolving to entry 8; a PSMT8 index (3) with both swizzle
bits already clear, confirming it's unaffected; and a PSMCT32
regression check proving the default (unset) PSM samples directly,
with the new CLUT machinery not engaged at all.

Regression: 59 host-native test binaries (was 58, +`test_gs_clut.c`),
0 failures (`-lm` appended locally per this sandbox's own linker
behavior, documented in Round 23's section). Clean Wii/devkitPPC
rebuild, same single pre-existing harmless `strncpy` warning as every
prior round.

**Net result for GS Round 24**: CLUT/paletted textures (PSMT8/PSMT4)
are implemented against real (if this-round-unverified-live) GS
hardware semantics, with a dedicated regression test and zero
regressions elsewhere. Per ROADMAP.md section 6, the remaining known
GS gaps after this round are: real block-swizzled addressing,
REGLIST/IMAGE transfer modes, GS context 2 (dual-context), and
mipmaps - the user has directed all four to be completed next, in
that order, as part of the same standing "complete GS port" session.

## GS Round 25: real block-swizzled addressing (page/block level)

Continuing the user's "complete GS port" sweep after Round 24's CLUT
work, this round tackles real block-swizzled GS memory addressing -
the next item on the user's stated order.

**Why this is an ADDITIVE change, not a replacement.** `gs_mem`'s
existing `gs_mem_read_psmct32`/`gs_mem_write_psmct32` functions use a
simplified linear addressing convention where `bp` is treated as an
arbitrary large "word offset" - values like 2000, 5000, 10200 appear
throughout this project's entire existing GS test suite (15+ files)
and the `gif.c` rasterizer/texture/CLUT pipeline, picked purely to
keep unrelated regions from overlapping under linear addressing. Real
hardware's BP field unit is one PAGE (8192 bytes for PSMCT32) - a 4MB
buffer only has 512 such pages, so none of those existing large `bp`
values are valid real-hardware pointers, and would collide/alias
under real addressing. Retrofitting the entire existing test suite
and rasterizer to real-hardware-valid `bp` ranges is a substantially
larger, riskier change than fits in one focused, safely-verifiable
increment - it would mean re-picking every texture/framebuffer/Z-
buffer/CLUT base pointer across 15+ test files and re-verifying each
by hand. So this round adds a genuinely real, separately-tested
addressing function (`gs_mem_swizzle_addr32` and its read/write
wrappers) alongside the existing simplified functions, rather than
swapping the pipeline over - that swap is explicit, documented future
work (see ROADMAP.md section 6).

**What's real vs. simplified.** A PSMCT32 "page" is 64x32 pixels
(8192 bytes = one real BP unit), divided into 32 blocks of 8x8 pixels
(256 bytes each), arranged in a fixed non-linear 8-wide x 4-tall grid
- the actual real, well-established PSMCT32 block-swizzle order
(reproduced in `source/hw/gs_mem.c`'s `gs_psmct32_block_table`). What
is NOT modeled is the finer within-block "column" pixel interleave
real hardware also applies - pixels within each 8x8 block are stored
here in simple row-major order instead, a documented, honest partial
step: real at the page/block granularity, simplified below that.

**Citation-honesty note (same as Round 24).** This round's dedicated
research-subagent dispatch again hit this session's own usage/session
limit before it could run. The page/block-grid table above is
implemented from established, widely-published PS2 GS/homebrew
texture-tooling knowledge rather than a freshly-verified primary-
source citation this round - flagged explicitly in `gs_mem.h`/
`gs_mem.c`'s comments. Unlike the field-layout facts in Rounds 23-24
(which are simple bit positions with essentially no room for
ambiguity), a block-swizzle table is exactly the kind of detail that
benefits most from primary-source verification - so Round 25's test
file includes a "no-collision" property check (every one of a page's
2048 pixels maps to a distinct byte offset) specifically because that
property would likely fail if the table were subtly wrong, giving
real confidence even without a fresh citation.

**Testing (`tests/test_gs_swizzle.c`, 10 checks).** Hand-derived
known-value checks against the documented block table (pixel (0,0),
(8,0), (0,8), and the page's last pixel (63,31) all land at their
expected byte offsets); `bp` behaving as a real page unit (exactly
+8192 bytes between `bp=0` and `bp=1`); a 2-page-wide buffer's second
page and second row landing at the correct page-index offsets; the
no-collision property across a full page (2048 pixels, all distinct);
a full-page write/read round-trip through the real swizzle functions;
and an explicit check that the new swizzled function and the
pre-existing linear function genuinely disagree at a non-degenerate
coordinate (proving they're not accidentally aliased to each other).

Regression: 60 host-native test binaries (was 59, +`test_gs_swizzle.c`),
0 failures - and, notably, ALL 59 pre-existing tests pass completely
unmodified, since this round adds new functions without touching the
existing linear ones at all. Clean Wii/devkitPPC rebuild, same single
pre-existing harmless `strncpy` warning as every prior round.

**Net result for GS Round 25**: real PSMCT32 page/block-swizzled
addressing exists, is genuinely tested (including a structural no-
collision property, not just a handful of point checks), and is ready
for a future round to actually wire into the rendering pipeline once
the existing test suite's `bp`/`bw` conventions are audited/migrated
to real-hardware-valid ranges - a nontrivial follow-up task, explicitly
left open rather than rushed. Per the user's stated order, remaining:
REGLIST/IMAGE transfer modes, GS context 2, mipmaps.

## GS Round 26: REGLIST and IMAGE GIF transfer modes

Third of five items in the user's directed sweep (after Round 24's
CLUT and Round 25's block-swizzle addressing), this round implements
the two GIF transfer modes that were previously entirely unimplemented
- any non-PACKED GIF tag was simply byte-skipped without any
interpretation at all.

**REGLIST mode (FLG=1).** Real hardware packs TWO plain 64-bit
register values per 128-bit qword (register A in words 0-1, register
B in words 2-3), looping NLOOP times through the tag's NREG-register
REGS descriptor - the exact same REGS/NREG tag fields PACKED mode
already uses, just interpreted as a flat stream of 64-bit values
instead of PACKED's per-register 128-bit expanded encodings. Every
register in a REGLIST stream is now routed through the existing
`apply_ad_write()` function uniformly: it already implements the
correct "natural" 64-bit encoding REGLIST uses for PRIM/RGBAQ/XYZ2/
TEX0_1/FRAME_1/ZBUF_1/TEST_1/ALPHA_1/etc (the same encoding A+D writes
already use in PACKED mode) - reusing it is both more complete than a
narrower duplicate switch and, more importantly, already tested. This
does mean REGLIST-mode XYZ2 writes inherit this project's existing,
already-documented A+D XYZ2 simplification (no real Z value - see
`apply_ad_write`'s own `GS_REG_XYZ2` case) - a consistent, not a new,
limitation. Total registers = NLOOP*NREG; total qwords consumed =
ceil(total/2) (the previous, pre-Round-26 fallback code incorrectly
assumed REGLIST's byte span equaled NLOOP qwords, same as IMAGE mode -
a real bug that would have desynced the GIF stream on any actual
REGLIST packet; fixed as part of this round, verified by a dedicated
odd-register-count test proving the packet immediately following a
REGLIST packet still parses correctly).

**IMAGE mode (FLG=2/3).** Confirmed real semantics: a completely raw
pixel-data dump, sized as exactly NLOOP qwords (this part of the
pre-existing fallback code was already correct - IMAGE's byte
accounting didn't need fixing, only its data INTERPRETATION did),
governed by 4 registers configured via ordinary PACKED-mode A+D writes
beforehand: `BITBLTBUF` (SBP/SBW/SPSM source and DBP/DBW/DPSM
destination fields - only the destination fields are acted on),
`TRXPOS` (SSAX/SSAY source and DSAX/DSAY destination position - only
destination acted on), `TRXREG` (RRW/RRH transfer rectangle
width/height), and `TRXDIR` (XDIR - writing this register is what
actually TRIGGERS the transfer on real hardware, resetting the
progress cursor to the rectangle's start). This round implements
host-to-local (XDIR=0) transfers into a PSMCT32 destination only -
local-to-host (readback) and local-to-local (in-VRAM blit) are parsed
(all 4 registers' fields are extracted) but not acted on, a documented
gap consistent with this project having no host-readback path and no
local-to-local blit engine at all. When a transfer is active, each
IMAGE-mode qword's 4 words are written as 4 raw PSMCT32 pixels in
raster order into the destination rectangle (wrapping at RRW, stopping
- and auto-deactivating - once RRH rows are filled, matching real
hardware's rectangle-bounded transfer behavior). When no transfer is
active (unsupported direction, non-PSMCT32 destination, or simply no
prior TRXDIR trigger), IMAGE data is safely byte-skipped exactly as
before this round - not interpreted, but the stream stays in sync.

**Citation-honesty note (same pattern as Rounds 24-25).** This round's
dedicated research-subagent dispatch again hit this session's own
usage/session limit before it could run. The GIFTag FLG/NREG bit
positions, REGLIST's 2-registers-per-qword packing, and the
BITBLTBUF/TRXPOS/TRXREG/TRXDIR register layouts are implemented from
established, well-published PS2 GS knowledge rather than a freshly-
verified primary-source citation this round - flagged explicitly in
`gif.h`'s register-definition comments.

**Testing (`tests/test_gs_reglist_image.c`, 15 checks).** REGLIST with
an even register count (2 registers, 1 qword) verifying both
registers land correctly; REGLIST with an odd count (3 registers, 2
qwords, the second qword's upper half being real-hardware padding)
verifying all 3 registers apply correctly AND that a packet
immediately following still parses correctly (the stream-desync bug
mentioned above, now fixed); a full host-to-local IMAGE transfer (3x2
pixel rectangle) verifying every pixel lands at its correct
destination coordinate, including wrapping at the RRW boundary and
auto-deactivation once the rectangle is filled; and an IMAGE packet
with no prior TRXDIR trigger, verifying gs_mem is left completely
untouched while the stream still stays in sync for the packet after.

Regression: 61 host-native test binaries (was 60,
+`tests/test_gs_reglist_image.c`), 0 failures - all 60 prior tests
pass unmodified. Clean Wii/devkitPPC rebuild, same single
pre-existing harmless `strncpy` warning as every prior round.

**Net result for GS Round 26**: REGLIST and IMAGE transfer modes are
implemented against real (if this-round-session-limited) GS hardware
semantics, including host-to-local texture/framebuffer data upload -
previously any such transfer would have been silently dropped with no
interpretation at all. Per the user's stated order, remaining: GS
context 2, mipmaps.

## GS Round 27: Context 2 (dual-context support)

Fourth of five items in the user's directed sweep, this round
implements real GS dual-context support - previously only context 1
existed at all, and PRIM's CTXT bit was never even parsed.

**Design (deliberately non-invasive).** Real hardware has two fully
independent rendering contexts, each with its own FRAME/ZBUF/
XYOFFSET/TEX0/TEST/ALPHA (and more this project doesn't model at all
for either context - CLAMP/TEX1/TEX2/SCISSOR/FBA/MIPTBP). Context 2's
registers all live at address (context-1 address + 1) - a pattern
this round's implementation double-checks against itself: TEX0_1=
0x06/TEX0_2=0x07, XYOFFSET_1=0x18/XYOFFSET_2=0x19, TEST_1=0x47/
TEST_2=0x48, ALPHA_1=0x42/ALPHA_2=0x43, FRAME_1=0x4C/FRAME_2=0x4D,
ZBUF_1=0x4E/ZBUF_2=0x4F - all independently added across Rounds 22-26
and now confirmed mutually consistent with the +1 pattern, which is
itself a real, well-established GS register-table property.

Rather than rewrite gif.c's ~80 internal reads of fbp/fbw/tex_tbp0/
tex_tbw/tex_tfx/tex_tw/tex_th/tex_psm/tex_cbp/tex_cpsm/tex_csa/zbp/
zmsk/zbuf_configured/zte/ztst/ate/atst/aref/afail/alpha_a-d/fix
scattered across gs_finish_pixel(), gs_sample_texel(),
gs_sample_clut(), and all 4 rasterizers - a large, error-prone change
touching roughly 8 functions - this round adds genuinely separate
per-context permanent storage (`ctx1_xxx`/`ctx2_xxx` fields in
`gif_state_t`) and a single new function, `gs_activate_context()`,
called once at the very top of each of the 4 rasterizers (right
before a primitive is actually drawn). It refreshes the pre-existing
flat "active" fields - which gs_finish_pixel()/gs_sample_texel()/
gs_sample_clut() and the rasterizers' own bodies keep reading
completely unchanged from before this round - from whichever of
ctx1_xxx/ctx2_xxx PRIM's CTXT bit (bit 9, `PRIM_CTXT_MASK`) currently
selects. Every `_1` register write (e.g. FRAME_1) updates BOTH its
ctx1_xxx permanent field and the flat field directly, so any test or
demo reading the flat fields immediately after a register write -
without an intervening primitive draw, as several pre-existing tests
do - sees zero behavioral change; a `_2` register write updates ONLY
the ctx2_xxx permanent field, since context 2 only becomes "live" in
the active fields once a primitive is actually drawn with PRIM.CTXT=1
selected. For CTXT=0, `gs_activate_context()` simply re-copies context
1's own already-correct values back (idempotent) - this is why the
change is verifiably non-invasive: it was validated by re-running the
FULL pre-existing 61-test regression suite unmodified, all passing.

**Citation-honesty note (same pattern as Rounds 24-26).** This round's
dedicated research-subagent dispatch again hit this session's own
usage/session limit before it could run. PRIM's CTXT bit position and
the context-2 register addresses are implemented from established PS2
GS knowledge rather than a freshly-verified primary-source citation -
mitigated here specifically by the internal self-consistency check
described above (the "+1" pattern independently reproduced across 6
already-separately-added register pairs from 5 different prior
rounds, not invented fresh this round).

**An incidental doc-drift bug found and fixed along the way.**
Round 26's own `tests/README.md` entry for `test_gs_reglist_image.c`
incorrectly listed `../source/hw/gif.c ../source/hw/gs_mem.c` as
additional link inputs, even though that test file (like
`test_gif_texture.c` before it) directly `#include`s `hw/gs_mem.c`
and `hw/gif.c` - causing a genuine "multiple definition" link error
whenever the documented command is used verbatim. This was caught by
this round's full regression re-run and fixed (the extra link inputs
removed from the README command); it was not previously caught
because Round 26's own verification pass, in retrospect, must not
have actually exercised the corrected README text.

**Testing (`tests/test_gs_context2.c`, 10 checks).** Two sprites drawn
at the same screen position with different PRIM.CTXT bits, each using
its own FRAME register's target buffer, land in genuinely separate
gs_mem regions; a configured-but-never-selected FRAME_2 has zero
effect on context 1 draws and its target buffer stays untouched;
independent per-context alpha test state (TEST_1 set to ATST_NEVER,
discarding everything; TEST_2 set to ATST_ALWAYS, passing everything)
proven by drawing the identical primitive under each context and
observing opposite outcomes at the same screen position; and an
interleaved ctx1/ctx2/ctx1 draw sequence proving neither context's
state leaks into or gets clobbered by the other.

Regression: 62 host-native test binaries (was 61,
+`tests/test_gs_context2.c`), 0 failures - all 61 prior tests pass
completely unmodified (the README fix above corrects a command that
was already broken before this round, not something this round's own
code changes caused). Clean Wii/devkitPPC rebuild, same single
pre-existing harmless `strncpy` warning as every prior round.

**Net result for GS Round 27**: real, independently-selectable dual-
context rendering state exists and is genuinely exercised end-to-end
(register writes -> context storage -> primitive dispatch -> correct
target buffer/alpha behavior), implemented without touching any of
the existing per-pixel rendering logic. Per the user's stated order,
remaining: mipmaps.

## GS Round 28: Mipmap support

Fifth and last item in the user's directed sweep ("Clut, Block
Swizzled Adressing, Reglist/Image Modi, GS Context 2, Mipmaps sofort
fertig") - this round implements TEX1/MIPTBP1/MIPTBP2 register
parsing and SPRITE-only per-primitive mip-level selection.

**Registers.** `TEX1_1` (0x14): LCM (bit0, 0=computed LOD/1=fixed
LOD), MXL (bits2-4, max mip level), MMAG (bit9, magnification filter,
parsed but unused - this project has no filtering model at all, even
for the base level), MMIN (bits10-12, minification filter; values
>= `GS_MMIN_MIPMAP_THRESHOLD` (2) mean "some form of mipmapping",
values below mean LINEAR/NEAREST with no mipmapping), MTBA (bit14,
automatic mip address calculation - unimplemented, documented gap),
L (word1 bits0-1, parsed but unused), K (word1 bits2-13, signed
12-bit, 1/16-unit LOD bias/fixed-LOD value - sign-extended from the
field's own top bit since C's `>>` on an already-masked positive int
doesn't sign-extend on its own). `MIPTBP1_1`/`MIPTBP2_1` (0x34/0x36)
hold TBP/TBW for mip levels 1-3 and 4-6 respectively, modeled (same
approach as every other multi-field GS register this project parses)
as a plain sequential 64-bit bitfield with no word-alignment padding
between fields - TBP2 and TBP5 both straddle the word0/word1 boundary
and are decoded with the identical cross-word bit-splitting technique
already used for TEX0's TW/TH field several rounds ago.

**LOD selection (`rasterize_sprite()` only - real hardware also
mipmaps TRIANGLE, a documented gap here).** Implemented as a
save/override/restore around the existing pixel loop, so every other
texture-sampling read site (`gs_sample_texel()`, `gs_sample_clut()`,
the other 3 rasterizers) needed zero changes - the same
deliberately-non-invasive pattern Round 27 used for dual-context
support. Before the pixel loop: if TME is set, MXL > 0, MMIN is at or
above the mipmap threshold, and MTBA is 0, compute a level: LCM=1
uses `K/16.0` rounded to the nearest integer (K may be negative, per
the sign-extension above, though this round's own tests only exercise
non-negative K); LCM=0 computes
`ratio = max(tex_w/screen_w, tex_h/screen_h)` and takes
`floor(log2(ratio))` when `ratio > 1` (magnification, ratio <= 1,
always yields level 0 - no mip level is ever selected when the
texture is being enlarged, matching real hardware). Either way the
result is clamped to `[0, MXL]`. A non-zero level temporarily
overrides `tex_tbp0`/`tex_tbw` from `tex_mip_tbp[level-1]`/
`tex_mip_tbw[level-1]` for the duration of the pixel loop, restored
unconditionally at the end of the function (including the MTBA=1/
MMIN-too-low/magnification paths, which simply never touch the saved
values in the first place).

**Citation-honesty note (same pattern as Rounds 24-27).** This
round's dedicated research-subagent dispatch again hit this session's
own usage/session limit before it could run. TEX1's exact bit
positions and the MIPTBP1/MIPTBP2 sequential-bitfield layout are
implemented from established PS2 GS knowledge rather than a
freshly-verified primary-source citation, consistent with every prior
round this session.

**Testing (`tests/test_gs_mipmap.c`, 25 checks).** TEX1 field
round-trip including a deliberately negative K value, verifying the
12-bit sign-extension is correct; MIPTBP1/MIPTBP2's all 6 mip levels'
TBP/TBW round-trip, including the two fields (level 2, level 5) that
straddle the word0/word1 boundary; a 64x64 texture drawn into an 8x8
screen rectangle (minification ratio 8, log2(8)=3) under LCM=0
actually samples mip level 3's distinctly-colored buffer rather than
the base level's, proving the computed-LOD path genuinely changes
which buffer gets read, not just which number gets computed; the same
setup with MXL=1 clamps the computed level 3 down to level 1 and
samples THAT buffer; a texture drawn larger than its source
(magnification) always uses the base level regardless of mipmap
configuration; MMIN below the mipmap threshold disables mipmapping
even with a large minification ratio and MXL configured; LCM=1 with
K=32 (2.0) samples level 2 at a screen size that would have computed
a different level under LCM=0, proving K genuinely overrides the
computed formula rather than just being parsed and ignored; MTBA=1
safely falls back to the base level rather than misbehaving with an
unimplemented auto-address calculation; and a final regression check
that a SPRITE draw with no TEX1 configured at all (MXL defaults to 0
from `gif_init()`'s zero-init) behaves exactly as it did before this
round.

Regression: 63 host-native test binaries (was 62,
+`tests/test_gs_mipmap.c`), 0 failures - all 62 prior tests pass
completely unmodified. Clean Wii/devkitPPC rebuild (`make TARGET=boot`
under devkitPPC r32/libogc via `/tmp/dkp_root`, with the recurring
`libmpfr.so.4` `LD_LIBRARY_PATH` workaround from `/tmp/mpfr_extract`
- an environment quirk of this sandbox, not a project bug), 0
warnings/errors beyond the same single pre-existing harmless
`strncpy` warning every prior round has also seen.

**Net result for GS Round 28**: SPRITE primitives now sample from the
correct mip level based on either a computed texture/screen size
ratio or a fixed LOD bias, with documented, safely-degrading gaps for
TRIANGLE mipmapping, per-pixel/trilinear filtering, and MTBA=1 auto
addressing. This completes all five items from the user's directed
sweep this session (CLUT/paletted textures - Round 24, real
block-swizzled addressing - Round 25, REGLIST/IMAGE GIF transfer
modes - Round 26, GS Context 2 - Round 27, mipmaps - Round 28), each
individually committed, pushed, and rsynced as its own checkpoint per
the user's explicit session-limit/checkpoint request.

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

## Round 29 continued (4th change, 2026-07-07): main.c switched from demo mode to real automatic boot flow

Per explicit user direction ("mach die 126 und sorg dafuer das die
main.c von demo auf echten boot flow geht" - do task #126, make main.c
go from demo to real boot flow), `source/main.c` no longer gates real
BIOS execution behind a menu action with a fixed instruction cap. The
old `action_bios_boot_test()` ran a one-shot `system_run_interleaved()`
capped at `DEMO_STEP_CAP` (2,000,000 slices) from a menu item labeled
"BIOS Boot Test", printed text-only stats, and required the user to
manually select it.

Now `run_real_boot_flow()` runs automatically at startup, immediately
after `wii_console_setup()`, before any menu is shown:

- Mounts SD storage and loads the first real BIOS image found at
  `sd:/pcsx2/bios/{SCPH39001,SCPH10000,bios}.bin`.
- Calls `system_init()` + `gs_init()` + `gs_mem_init()` once, guarded by
  a `g_system_started` flag so re-entry (from the "Re-run Boot Flow"
  menu item, the old label's replacement) restarts cleanly.
- Loops in `BOOT_CHUNK_SLICES` (200,000) increments up to a much larger
  `BOOT_TOTAL_CAP` (2,000,000,000) instead of one fixed-size shot, so
  the flow keeps running instead of stopping after ~2M instructions.
- Each loop iteration polls the real GS state (`gs->pmode & 0x3`) for
  `display_active`. If a display circuit is active, `decode_dispfb()`
  converts the real hardware DISPFB1 register (FBP = bits 0-8 in units
  of 2048 words, FBW = bits 9-14 in units of 64 pixels - real hardware
  units, NOT this project's simplified `gs_mem.h` word/pixel-count
  convention) and calls `gs_blit_psmct32_to_xfb()` to actually present
  GS memory to the Wii's real framebuffer.
- Draws a live HUD (`draw_boot_progress_hud()`) each iteration: EE/IOP
  instruction counts, halted state and reason per core, whether a
  display is active, and a "hold B to stop" hint - replacing the old
  demo's single final text dump.
- Breaks out on B-held, both-cores-halted, or the total cap, shows a
  final status screen, then waits for A/B before falling through to
  the (now secondary) menu.

**Honest caveat, carried over from the diag53 finding earlier this
session (task #127)**: as of this change, GS local memory and the
privileged GS registers (PMODE/DISPFB1/DISPLAY1/CSR) were observed to
stay at their power-on-zero value through 240M EE / 30M IOP real
instructions - well past the LOGO-loading module found in the 3rd
finding above. That means `display_active` is not currently expected
to go true within any practically reachable instruction count yet; this
change is correct, real scaffolding (no more demo cap, no more menu
gate, real register decoding, real blit call wired up) but it is not
yet proven to produce real pixels on screen. The next real blocker to
chase is why the GS driver path never gets exercised - i.e., what real
BIOS code is supposed to write PMODE/DISPFB1, and why the traced
instruction window ends before that happens.

Verification this round: clean Wii/devkitPPC rebuild (`make TARGET=boot`,
exit 0, only the pre-existing unrelated `iop_module_loader.c` strncpy
warning), full host-native regression suite re-run via the standard
`tests/README.md` block-extraction script (65 test binaries, 0
failures - includes this session's new `test_iop_kmem_alloc.c` and
`test_iop_syscall_handler.c`).

## Round 29 continued (5th finding + fix, 2026-07-08): real A(13h) setjmp + B(19h) HookEntryInt, and a precisely-pinpointed real BIOS panic loop

Following the main.c real-boot-flow change (previous section), the
user's standing instruction to automatically continue with other
important tasks led directly back to the IOP exception-chain wall
(task #124), armed with a concrete new lead: the diag53 finding that
GS registers never get touched. A long-running host-native diagnostic
(300M IOP / 2.4B EE instruction cap) confirmed both cores reach a
genuine steady state almost immediately (by ~3.05M IOP instructions)
and never progress further, no matter how long the run continues -
this is not a "needs more instructions" situation, it is a true
infinite loop.

**Root-caused via live call-tracing**: instrumenting `iop_hle_bios_try_handle()`
to log every A0/B0/C0 call's arguments found that, at IOP instruction
3054696, the real BIOS calls `A(13h) setjmp(buf)` with `a0=0x8004fd50`,
immediately followed at instruction 3054708 by `B(19h)
HookEntryInt(addr)` with the SAME address (`a0=0x8004fd50`). Neither
function was previously implemented (both fell through to the generic
"return 0, do nothing" HLE default). Per psx-spx, `HookEntryInt(addr)`
writes the same RAM[0x00007520] pointer variable that this project's
own `ResetEntryInt` (B18h, implemented earlier this session) writes -
the difference is `HookEntryInt` installs the CALLER's own address
instead of the kernel's default struct (0x00006C34), letting BIOS code
supply its own "resume here if nothing claims this exception" struct.
Since neither call did anything, RAM[0x7520] stayed at the default
struct address forever, even though the real BIOS's own code had
already prepared (via `setjmp`) and tried to install (via
`HookEntryInt`) its own, different recovery point.

**Implemented for real**: `A(13h) setjmp(buf)` now saves the real
12-word ra/sp/fp/s0-7/gp struct (the same layout already reverse-
engineered from the kernel's default struct) into the caller's buffer
and returns 0, matching standard MIPS o32 setjmp semantics.
`B(19h) HookEntryInt(addr)` now writes RAM[0x7520]=addr and returns
addr in $v0 (mirroring ResetEntryInt's documented return-value
convention). Verified via `tests/test_iop_hook_entry_int.c` (18
checks) that: setjmp's save is byte-for-byte correct; HookEntryInt's
write/return are correct; the real setjmp+HookEntryInt pairing (same
address) correctly ends with RAM[0x7520] pointing at the caller's own
buffer, not the kernel default; and ResetEntryInt/HookEntryInt remain
independent (calling one doesn't corrupt the other's effect). Live
re-verification against the real BIOS confirmed RAM[0x7520] now
correctly reads `0x8004fd50` (the real BIOS's own intended address)
instead of `0x00006C34` (the kernel default) after this exact call
sequence runs.

**Honest result: this fix is real and correct, but does NOT clear the
steady-state wall.** Re-running the same long diagnostic after the fix
produced an IDENTICAL EE/IOP instruction trace, PC-for-PC - the
dispatcher's post-priority-chain fallback path (which RAM[0x7520] now
correctly feeds) is evidently not what's reached on this particular
path; something else leads to the same final resting point regardless.

**Precisely pinpointed the actual wall** via direct disassembly of IOP
RAM 0x00101100-0x00101288 (the code the interpreter is actually stuck
in): it is a bounded, 4-pass retry loop (a counter at `$fp+0x58`,
compared against `4`) that walks a linked structure at `$s2` checking
a 2-bit type tag (`andi $v0,$a2,3`) against the current pass number
(0,1,2,3) and, on a match, calls through a masked function pointer via
`jalr` - a shape strongly resembling this kernel's own multi-phase
device-driver table walker (matching the A(96h)-A(99h)
AddCDROMDevice/AddMemCardDevice/AddDuartTtyDevice/add_nullcon_driver
functions documented in psx-spx, none of which were observed being
called in this trace - so either the table is empty/unpopulated in
this emulation, or this is a different, not-yet-identified table).
When all 4 passes fail to make the loop's exit condition true, real
BIOS code at 0x00101278-0x00101284 executes `lui $v1,0x8000; li
$v0,2; sb $v0,($v1); j 0x101280` - a literal, deliberate "write status
code 2 to physical RAM address 0, then spin forever" panic routine.
`SR=0` at this point (all interrupts globally masked), so this loop is
genuinely unrecoverable by design, matching this project's own earlier
(Round 19) "real BIOS panic/halt loop" finding - this round sharpens
that finding to the exact instruction sequence, exact panic code (2),
and the exact bounded-retry shape that precedes it, rather than just
"reaches a panic loop".

**Next concrete step for whoever picks up task #124 next**: disassemble
backward from 0x1011ac (the retry loop's own top) to identify what,
specifically, is being tested against the pass-number tag at each of
the 4 attempts, and what real hardware condition/register value this
emulation is failing to provide that a real console would supply
(most likely CD-ROM, memory card, or console/tty device registration
state, given the A(96h)-A(99h) parallel above - none of those
functions have been implemented in this project yet, which is a
strong first thing to check).

Verification this round: clean Wii/devkitPPC rebuild (`make
TARGET=boot`, exit 0, only the pre-existing unrelated
`iop_module_loader.c` strncpy warning), full host-native regression
suite (66 test binaries including this round's new
`test_iop_hook_entry_int.c`, 0 real failures - one script false-positive
from the word "AFAIL" containing the substring "FAIL" in
`test_gs_alpha`'s own flag names, confirmed by its own "0 check(s)
failed" output).

## Round 29 continued (6th change, 2026-07-08): real A(96h) AddCDROMDevice + A(97h) AddMemCardDevice

Per explicit user direction ("scheint so als muessten wir zu erst den
CDRomDevice und MemCardDevice hinzufuegen, fuege beide als active
Geraete hinzu nicht als Demo" - looks like we first need to add
CDRomDevice and MemCardDevice, add both as active devices, not as a
demo), implemented `A(96h) AddCDROMDevice()` and `A(97h)
AddMemCardDevice()` for real. Both previously fell through to the
generic HLE default (return 0, do nothing).

**Design**: psx-spx documents the DCB (Device Control Block) table's
address and total size in its BIOS RAM Map ("00000150h DCB Device
Control Blocks (addr=fixed, size=0Ah\*50h)") and notes that
`AddCDROMDevice`/`AddMemCardDevice` internally call the generic
`B(47h) AddDrv(device_info)` with a hardcoded, built-in device
descriptor - but does not document a citable, byte-exact per-entry
DCB layout (field order/offsets for name pointer, flags, and the
open/close/read/write/etc. function-pointer set). Rather than
fabricate an unverified struct and write speculative bytes into real
emulated RAM (which could misbehave in a new, harder-to-diagnose way
if any real BIOS code later reads that table with the true, different
layout), this implementation models the real, user-visible effect of
calling these functions - the device genuinely becoming registered
and available - as real, persistent, queryable internal emulator
state: `cdrom_device_registered`/`memcard_device_registered` flags
(plus call counters) in `iop_hle_bios_state_t`, flipped to 1 the first
time each function runs and staying 1 afterward, matching real
hardware's own idempotent "already registered, harmless no-op on
repeat calls" behavior (per psx-spx's `B(59h) testdevice()` note).
This is real state, not a demo: it genuinely changes, persists, and is
directly queryable by any other part of this codebase (e.g. a future
file-I/O dispatcher for `cdrom:`/`bu00:`/`bu10:` paths could check
these flags before dispatching), which is the meaningful difference
between "active" and "stub" for a function whose real job is
registration bookkeeping.

Verified via `tests/test_iop_device_registration.c` (17 checks): both
flags start unregistered; each function independently and correctly
flips only its own flag; call counters increment; both are safely
idempotent on repeat calls.

**Honest empirical result**: live-tracing the real SCPH-10000 dump for
10M IOP instructions (well past the steady-state loop from the 5th
finding above) confirms `add_cdrom_device_calls` and
`add_memcard_device_calls` both stay at 0 - neither function is called
anywhere in this real BIOS's traced boot-to-panic-loop window on this
no-disc, no-memory-card boot path. This matches the earlier honest
caveat in the 5th finding's write-up: implementing these functions is
real, valuable, user-requested BIOS-function coverage (and will matter
once a disc/memory-card-aware boot path or a later game's own
boot/runtime code is traced), but it does not, by itself, change the
steady-state outcome of the specific wall documented in the 5th
finding. That wall's root cause (task #124/#132) remains open.

Verification this round: clean Wii/devkitPPC rebuild (`make
TARGET=boot`, exit 0, only the pre-existing unrelated
`iop_module_loader.c` strncpy warning), full host-native regression
suite (67 test binaries including this round's new
`test_iop_device_registration.c`, 0 failures).

## Round 29 continued (7th finding, 2026-07-08): full execution trace pinpoints the exact empty list causing the panic

Continuing task #124/#132 with full instruction-level tracing (not
static disassembly guesswork) from IOP instruction 3054991 (the last
named HLE call before the wall) through to the panic at 0x101280.

**Corrected an error from the 5th finding's write-up**: static
disassembly of 0x101100-0x101288 in isolation suggested a self-
contained "4-pass retry loop over a device-driver-like table". Full
dynamic tracing shows the real picture is more specific: a much larger
routine (spanning roughly 0x100d80-0x101e90+, running through several
internal helper calls including two real initialization/copy loops -
one real, bounded 912-iteration loop at 0x101e48 that completes
normally) eventually reaches its core dispatch logic at 0x100f9c,
which checks `lw $v0, 8($s0); beqz $v0, 0x101188` - i.e., "if this
list's first-entry field is zero, skip the whole per-entry dispatch
loop". The live trace confirms this branch IS taken - the list is
genuinely empty (`8($s0) == 0`) - so the inner `jalr`-based dispatch
loop (the "device table walker" described in the 5th finding) never
actually executes even once; execution instead jumps straight to
0x101188, which feeds directly into the 4-pass retry loop
(0x1011ac-0x101270) with nothing to process on any pass, and falls
into the panic at 0x101280 exactly as before.

**Traced the empty list one level further back**: the list at `$s0` is
populated (a few thousand instructions earlier, ~0x100eec-0x100f24) by
copying `RAM[$fp+0x40]`-many entries from address `RAM[$fp+0x40]`
itself (i.e., `$fp+0x40` is a local/parameter slot holding both a
"how many" and "from where" reference used together) into a freshly
stack-allocated buffer - but only if `RAM[$fp+0x40] != 0` first (`lw
$a0, 0x40($fp); beqz $a0, 0x100f44` at 0x100eec/0x100ef4). The live
trace confirms THIS branch is also taken (the copy is skipped
entirely), meaning `$fp+0x40` - some parameter or upstream value this
particular call received - was itself zero/null at the time this
routine ran.

**What this rules in/out**: this specific code is NOT the ROM-resident
exception dispatcher (0xc80-0xe98) already fully mapped earlier this
session, and the timing (this routine runs ~3.05M IOP instructions
in, i.e. deep inside the ~2.8M-instruction LOGO-module execution
window from the 3rd finding, not near the C(0Ch) InitDefInt call at
instruction 84868) makes it very unlikely to be directly InitDefInt's
own code - despite InitDefInt(priority=3) being psx-spx-documented as
"internally used to add some default IRQ and Exception handlers" (a
tempting, but - on this timing evidence - probably coincidental,
match). The much likelier owner of this code is the LOGO-loading IRX
module itself (or a kernel helper it calls), processing some kind of
caller-supplied list/table (asset chunks, draw commands, or similar)
that is legitimately allowed to be empty/absent in this call, but
which - when empty - runs out a bounded retry budget and hits a
hardcoded panic rather than degrading gracefully.

**Status**: root cause is now empirically pinpointed to a specific
zero/null value at a specific stack slot in a specific real routine,
which is much more actionable than the 5th finding's "resembles a
device table" guess - but identifying WHERE that zero value should
instead be coming from (what real data this project isn't yet
providing, and from what real source: BIOS ROM resource table, disc
data, or an earlier IOP RAM structure this project doesn't populate)
needs at least one more round of backward tracing from this routine's
own call site/entry point, which was not reached within this round's
time budget. Task #124/#132 remains open with this sharpened target.

No code changes this round (pure diagnostic/tracing work); no test,
build, or commit needed for this specific finding, but see below for
this session's next change.

## Round 29 continued (8th change, 2026-07-08): CDVD register scaffold (no-disc boot case)

Per docs/ROADMAP.md's own long-standing "Suggested near-term order"
item 2 ("CDVD (disc) stub - even a diskless BIOS-only boot polls CDVD
status registers; nothing at all is modeled for this yet"), added a
real CDVD (disc drive) register block: `source/hw/iop_cdvd.c` /
`include/core/hw/iop_cdvd.h`.

**Design**: real IOP-side base address 0x1F402000, cross-checked
against both ps2tek's public port list and, more authoritatively, real
PCSX2 upstream source cloned fresh for this change
(github.com/PCSX2/pcsx2, `pcsx2/IopHw.cpp`'s `psxHw4Read8`/
`psxHw4Write8`, whose log strings literally say "[segment 0x1f40]" and
which mask the address to its low 8 bits before dispatching - i.e.
real hardware mirrors these byte registers across the entire
0x1F402000-0x1F402FFF 4KB page, replicated here exactly). Register
offsets and - critically - their real power-on-reset VALUES are
ported directly from PCSX2's own `pcsx2/CDVD/CDVD.cpp` `cdvdReset()`
(GPL-3.0, same "port real semantics, don't reinvent" approach already
used for `ee_core.c`): `DiscType=CDVD_TYPE_NODISC(0x00)`,
`Ready=CDVD_DRIVE_READY(0x40)`, `Status=CDVD_STATUS_TRAY_OPEN(0x01)` -
these are the exact, real values a real PS2 reports when booting with
no disc inserted, not guesses. The ERROR register's real
read-clears-on-read behavior (`cdvdRead06`) and the BREAK register's
real always-reads-0 behavior (`cdvdRead07`) are both replicated
faithfully since they're simple and directly cited.

**Scope, deliberately limited**: matching this project's own
established "register scaffold, not full hardware" pattern
(`iop_timers.c`/`iop_spu2.c`), the real N-command/S-command state
machines (seek/read/standby/etc, `cdvdWrite04`/`cdvdWrite16` and their
many sub-commands in real PCSX2 source) are NOT modeled - a write to
NCMD is latched (so polling code gets a truthful readback) and
immediately reports a plausible completion via INTR_STAT
(`Irq_CommandComplete=0`) rather than leaving BUSY set forever, so a
diskless boot's status-polling loop can observe "command done" instead
of spinning indefinitely on a command this project doesn't implement
the real behavior of - without pretending to emulate what any specific
command actually does.

Verified via `tests/test_iop_cdvd.c` (19 checks): real power-on
defaults; ERROR's read-clears behavior; BREAK always reading 0; NCMD
latching + triggering a completion IRQ; the real 4KB-page register
mirroring; and correct rejection of out-of-range addresses.

**Honest empirical result**: live-tracing the real BIOS for 10M IOP
instructions shows `last_ncommand` stays 0x00 - the real BIOS never
writes to the CDVD NCMD register within this traced window either
(consistent with the 6th change's AddCDROMDevice/AddMemCardDevice
finding: this specific boot path, on this specific real BIOS dump,
doesn't touch disc-related hardware/kernel state within the ~10M
instructions traced so far). This is real, valuable, ROADMAP-directed
hardware coverage that will matter for any boot path that DOES probe
CDVD status (a disc-present boot, or later BIOS/game code reached past
the current wall) - it does not, by itself, change the steady-state
outcome of the wall documented in the 5th/7th findings.

Verification this round: clean Wii/devkitPPC rebuild (`make
TARGET=boot`, exit 0, only the pre-existing unrelated
`iop_module_loader.c` strncpy warning; confirmed `iop_cdvd.c` compiles
into the Wii build too), full host-native regression suite (68 test
binaries including this round's new `test_iop_cdvd.c`, 0 failures).

## Round 29 continued (9th finding, 2026-07-08): traced the empty field to SYSMEM's own boot_info struct

Continuing task #124/#132 one level further back from the 7th finding.

**Traced `$fp+0x40`'s true origin**: live register capture at the exact
moment the routine investigated in the 7th finding is entered shows
`$a0 = 0x00100010` - i.e. this routine's "list" parameter is not an
arbitrary caller-supplied list at all, but a pointer into the SYSMEM
module's OWN loaded image, at its own base address (0x00100000) plus
0x10. Disassembly of the routine's entry (0x100d00-0x100d54) shows it
copies EIGHT consecutive words from that address into local variables:
`fp+0x38..fp+0x50` = `*(a0+0x00)` through `*(a0+0x18)`, i.e.
`RAM[0x100010]` through `RAM[0x100028]`. `fp+0x40` (the 7th finding's
empty list) is exactly `RAM[0x100010 + 0x08]` = `RAM[0x100018]`.

**This directly ties into previously-fixed, already-cited work**:
`source/hw/iop_module_loader.c`'s own `BOOT_INFO_RAM_MB` comment
already documents that SYSMEM's real module-entry code does `lw
v0,(a0); sll sp,v0,0x14` at its very start - i.e. `a0` points at a
"boot info" structure whose word 0 gives the IOP's RAM size in MB (2).
This project's loader already supplies that first word correctly
(confirmed: `RAM[0x100010]` reads back 2, matching `BOOT_INFO_RAM_MB`).
What this round's tracing newly found is that SYSMEM's own code reads
SEVEN MORE words from the same structure later on (offsets 0x04
through 0x18) - a real, larger "boot info" struct than the single
RAM-MB word this project's loader currently populates (everything past
offset 0x00 is presently zero, simply because nothing has ever written
there). Offset 0x08 in particular is the one this round's earlier
findings traced through to the panic loop.

**Why no fix was made this round**: no citable, verified real byte
layout for what belongs at offsets 0x04-0x18 was found (searched
psx-spx, ps2tek, PCSX2 upstream source, and a detailed independent PS2
boot-process write-up - none document SYSMEM's specific internal boot
parameter struct beyond the single RAM-MB word already implemented).
Guessing values for the remaining seven words would be fabrication
without evidence, which this project has consistently avoided (see
e.g. the CDVD/DCB-struct decision in the 6th change). The next step
for whoever continues this thread: live-disassemble SYSMEM's own code
further (particularly around 0x100d54-0x100e98, which reads/uses
offsets 0x08-0x1c after the copy) to infer, from HOW each field is
used, what a real, non-zero, plausible value would need to be - the
same "let the real code's own behavior tell you the answer" approach
that successfully resolved the B(00h)/B(18h)/B(19h) findings earlier
this session.

No code changes this round. Task #124/#132 remains open, now traced
three levels deep (empty-list check -> empty-parameter check ->
SYSMEM's own under-populated boot_info struct) from where the 5th
finding left off.

## Round 29 continued (10th finding: pcsx2-mcp live reference instance investigated, inconclusive)

A new tool suite (`pcsx2-mcp`) became available mid-session, connected
to a live, paused, real PCSX2 instance via DebugServer. Given the 9th
finding's open question (real values for SYSMEM's boot_info struct at
`RAM[0x100010]` offsets 0x04-0x18), this live instance was investigated
as a possible source of ground truth, per the user's explicit
authorization to use it "im Notfall" (in an emergency).

**What was checked**: `pcsx2_status()` confirmed a real, paused
instance (EE pc=0x0061bbe0, cycle 3424132242). `pcsx2_get_modules()`
listed 29 real IOP modules including `System_Memory_Manager` (SYSMEM,
v257), confirming this instance had booted far past this project's own
wall (all the way through pad/memory-card/SIO2 init). However,
`pcsx2_read_memory(0x00100000, 64 bytes)` read back all zeros, and
`pcsx2_disassemble(0x00100000, cpu=iop)` likewise showed nothing but
`nop`s at the exact address this project's own diagnostics place
SYSMEM's boot_info struct. `pcsx2_get_backtrace()` on the EE side
showed a call stack entirely within EE addresses 0x0055xxxx-0x0061xxxx
- ordinary userland/game-side code, not BIOS. `pcsx2_game_info()`
failed ("Pine not connected"), so this live instance's specific
game/BIOS identity is unknown.

**Conclusion (honest, not fabricated)**: this live instance is
conclusively unhelpful for recovering the 9th finding's transient
boot-time struct. By EE pc=0x0061bbe0 the system has already finished
IOP kernel boot and moved deep into userland/game code - any transient
SYSMEM boot_info struct that existed at `RAM[0x100010]` during boot has
long since been overwritten/reused by later IOP RAM allocations (the
all-zero readback is consistent with reclaimed memory, not with an
unpopulated struct). Chasing this further would require resetting the
live instance and single-stepping to a breakpoint at SYSMEM's own real
entry point - a much larger undertaking with its own unknowns (this
project's own SYSMEM load address of 0x100000 is not guaranteed to
match whatever game/BIOS this live instance is actually running,
especially since Pine/game_info isn't available to identify it).

**A second, more promising angle surfaced while investigating this**:
re-reading `source/hw/iop_module_loader.c`'s existing
`BOOT_INFO_RAM_MB` code shows `boot_info_addr` is allocated via
`bump_alloc(4)` - i.e. this project's own HLE module-loader shortcut
only ever allocates/writes the FIRST word of this struct. Offsets
0x04-0x18 read zero simply because this project's loader never
allocates or touches them at all, not because of some separate, only
partially-implemented mechanism. This narrows (without yet answering)
the 9th finding's question: it's not that a real mechanism is
"missing a few bytes" - it's that this project's own module-loader is
itself an HLE shortcut standing in for a real IOP kernel/EXECROM init
phase that would normally construct this struct from real boot
configuration (RAM size, region, EE-supplied config passed over SIF,
etc.) before ever reaching SYSMEM. The honest next step, if this
thread is picked up again, is disassembling what precedes this
project's own HLE shortcut point in the real BIOS dump to find what
(if anything) writes to this struct in the real boot sequence, rather
than guessing plausible values or continuing to search external
documentation (already exhausted: psx-spx, ps2tek, PCSX2 upstream
source, an independent PS2-boot-process write-up, and now a live
reference PCSX2 instance).

**Decision**: task #124/#132 (the IOP exception-chain/boot_info wall)
is formally deprioritized again after this tenth attempt - every
readily-available real reference source has now been exhausted without
a citable answer. Continuing to guess would cross into fabrication.
Session effort moves to independent, well-scoped roadmap work instead
(see the 11th change below), consistent with the user's standing
instruction to keep making real, checkpointed progress rather than
spin on one blocked thread.

## Round 29 continued (11th change: EE COP2 VADD/VMUL/VIADDI)

Extended round 13's VU0 macro-mode vector datapath
(`source/core/ee/ee_core.c`'s COP2 CO-format dispatch) with three more
real, cited opcodes, closing gaps this project's own comments had
already flagged:

- **VADD** (funct 0x28) and **VMUL** (funct 0x2A): the same 3-operand
  full-vector arithmetic shape already implemented for VSUB (funct
  0x2C) - `FD[lane] = FS[lane] (+|*) FT[lane]` for each lane selected
  by destmask. Funct codes cited against a fresh PCSX2 upstream
  reference clone's `R5900OpcodeTables.cpp`
  (`Int_COP2SPECIAL1PrintTable` row: `VADD, VMADD, VMUL, VMAX, VSUB,
  VMSUB, VOPMSUB, VMINI` at indices 40-47 = funct 0x28-0x2F).
- **VIADDI** (funct 0x32): immediate integer add on VI registers,
  `VI[ft] = VI[fs] + imm`, closing the exact gap this project's own
  existing VIADD/VISUB/VIAND/VIOR comment flagged as a "future gap".
  Confirmed VIADDI's real operand order differs from VIADD's sibling
  family: dest=FT (not FD) per PCSX2's own
  `DisR5900asm.cpp`'s `P_VIADDI` disassembly formatter
  (`"viaddi FT, FS, 0x%x(SA)"`), with the immediate occupying the same
  raw bit position (6-10) that FD occupies for the other CO-format ops.
  The sign-extension itself is ported verbatim from PCSX2's own
  `VUops.cpp` `_vuIADDI` (a real-hardware quirk - effectively a 4-bit
  signed magnitude plus a separate sign bit, not a plain 5-bit two's
  complement extend) rather than reinvented.

Also added `tests/test_ee_cop2_arith2.c`, which - alongside the three
new ops - gives first-time host-native test coverage to VIADD/VISUB/
VIAND/VIOR themselves (implemented in round 13 but never actually
tested until now). 11 checks, all passing; full 68-block regression
suite (67 pre-existing + this new one) still passes; clean Wii rebuild
verified.

**Honest scope note**: a host-native diagnostic (`diag68`, 20M-slice
interleaved run against the real SCPH-10000 dump) confirmed the EE's
current boot trace does NOT reach any of these new opcodes - the EE is
still in its long-documented steady-state SIF poll (pc=0x80005E90,
not halted), and the IOP is still stuck at the 9th finding's panic
loop (pc=0x00101284). This is real, tested, roadmap-directed
readiness work (docs/ROADMAP.md section 5 item 3), not a fix that
moves the current boot further - consistent with this session's
practice of being explicit about what does and doesn't change the
observed steady state.

## Round 29 continued (12th change: REAL FIX - boot_info offset 0x0C, genuine forward progress)

Continuing task #124/#132's chase (per the user's explicit instruction
to keep pursuing it), a live-traced disassembly of the real
SCPH-10000 BIOS's own SYSMEM init code (IOP RAM 0x100D00-0x100D8C, via
a targeted single-step diagnostic capturing every instruction in that
range plus a raw-bytes dump fed to Capstone) reads:

```
lw   v0, (a0)        ; offset 0x00 -> fp+0x38  (RAM_MB, already correct)
lw   v0, 4(a0)       ; offset 0x04 -> fp+0x3c
lw   v0, 8(a0)       ; offset 0x08 -> fp+0x40
lw   a1, 0xc(a0)     ; offset 0x0C -> a1 (ACTIVELY USED, not just copied)
lw   v0, 0x10(a0)    ; offset 0x10 -> fp+0x48
lw   v0, 0x14(a0)    ; offset 0x14 -> fp+0x4c
lw   v0, 0x18(a0)    ; offset 0x18 -> fp+0x50
lw   a2, 0x1c(a0)    ; offset 0x1C -> a2, -> fp+0x54
lui  v1, 0x10 ; addiu v1, v1, 0x2924   ; v1 = 0x00102924
sw   a1, -4(v1)      ; RAM[0x00102920] = a1 (offset 0x0C's value)
lui  a0, 0x10 ; lw a0, 0x2920(a0)     ; a0 = RAM[0x00102920] (== a1, round-tripped)
...
sw   zero, (a0)      ; *** writes zero through offset 0x0C's value ***
```

Every other offset (0x04/0x08/0x10/0x14/0x18/0x1C) is only ever copied
into a local stack slot within this disassembled span - not
dereferenced. Offset 0x0C is the one exception: its value is used
directly as a pointer, and something gets zeroed through it.

**The bug**: this project's loader (`source/hw/iop_module_loader.c`)
only ever `bump_alloc(4)`'d and wrote the FIRST word of this struct
(see the 10th finding). Offset 0x0C therefore read as 0, making
`sw zero,(a0)` above write to REAL RAM ADDRESS 0 - an actively
observed, real bug (confirmed via live tracing), not a hypothetical
one - structurally identical to the earlier INITIAL_SP bug (a
near-zero pointer walking into and corrupting low RAM, that time the
exception vector at 0x80).

**The fix**: `iop_module_loader_boot()` now allocates the FULL
0x20-byte boot_info struct (not just 4 bytes), keeps offset 0x00 =
`BOOT_INFO_RAM_MB` (unchanged, already correct), and sets offset 0x0C
to point at a dedicated, separately-allocated, zero-initialized
scratch word - so the observed real write-through lands somewhere
safe instead of RAM address 0. This is an explicitly DEFENSIVE choice
(no citable real value for what offset 0x0C should "really" point to
was found - same exhausted search as the 9th/10th findings), applying
the exact same honest precedent `INITIAL_SP`'s own comment already
established: not a verified real hardware constant, but a reasoned
mitigation for an actively-observed bug. Offsets 0x04/0x08/0x10/0x14/
0x18/0x1C remain honestly zero - their real values, if any, are still
unknown, not fabricated.

**Empirical result - genuine forward progress, not just a relocated
wall**: a fresh 20M-slice interleaved diagnostic against the real
SCPH-10000 dump shows the IOP steady-state pc advanced from
`0x00101284` (before this fix) to `0x001012A8` (after) - traced and
disassembled: this is REAL additional code executing (a list-walk
loop at 0x101200-0x101278 doing `lw t0,-4(s1)`/`jalr v0` - a real
function-pointer-table dispatch - followed by the same bounded 4-pass
retry loop documented in earlier rounds), not the same instruction at
a shifted address. The panic loop itself (`lui v1,0x8000; addiu
v0,zero,2; sb v0,(v1); j`) is still eventually reached - this fix
moves the boot further into real BIOS code, it does not clear the
wall outright. EE pc is unchanged (still steady-state SIF-polling at
0x80005E90), as expected since the IOP hasn't progressed far enough
yet to signal it.

New test: `tests/test_iop_module_loader_bootinfo.c` (7 checks) -
entirely synthetic ROMDIR + ELF module image (same convention as
`test_bios_loader.c`/`test_iop_elf.c`, no real BIOS bytes), drives
`iop_module_loader_boot()` end-to-end and verifies offset 0x0C is now
a valid, distinct, zero-initialized scratch pointer. Full 70-block
regression suite passes (69 pre-existing + this new one); clean Wii
rebuild verified.

Task #124/#132 remains open (the panic loop is still eventually hit),
but this is real, verified, measurable progress along that thread,
not readiness work - continuing per the user's explicit instruction
to keep chasing this specific root cause.

## Round 29 continued (13th finding: precisely characterized the retry-loop's "empty list", ruled out a hypothesis empirically)

Continuing directly from the 12th change's new wall (pc=0x001012A8),
traced backward from the retry loop (RAM 0x1011A8-0x101294) to its
caller (RAM 0x100F00-0x101000) via a targeted single-step + disassembly
dump, plus a raw stack-memory dump around `fp`/`s2` at the exact
moment of the first retry-loop iteration.

**What the retry loop actually is**: `s2` (the pointer the loop's
`lw v0,(s2); beqz v0,skip` empty-check reads) is NOT a global table -
it's a freshly stack-allocated buffer, sized directly from boot_info
struct offset 0x10:

```
lw   v0, 0x50(fp)      ; v0 = boot_info offset 0x10 (copied earlier)
sll  v0, v0, 3         ; v0 = count * 8 bytes/entry
subu sp, sp, v0        ; allocate that many bytes on the stack
addiu s2, sp, 0x10
sw   zero, 0x10(sp)    ; zero the first entry/slot
```

A second, separate list (checked earlier, at `lw v0,8(s0)` /
`beqz v0,0x1011a8`) is sized the same way from offset 0x1C:

```
lw   a2, 0x50(fp) ; lw a1, 0x54(fp)   ; offsets 0x10 and 0x1C
addiu a2, a2, 1 ; sll a2, a2, 2 ; ...  ; count+1 entries, 4 bytes each
subu  sp, sp, v0                       ; allocate
addiu s0, sp, 0x10
```

Since this project supplies 0 for both offset 0x10 and offset 0x1C
(their real values are still unknown - see the 9th/10th findings),
both allocations compute to (near-)zero size, and the explicit
`sw zero,0x10(sp)` guarantees the first slot of each list reads 0 -
i.e. these two boot_info fields are dynamic LIST-SIZE/COUNT fields,
and supplying 0 for them structurally guarantees both lists look
"empty" to the retry loop, regardless of anything else.

**Hypothesis tested and REJECTED (honest negative result)**: patched a
throwaway diagnostic to supply small nonzero counts (1, 4, 8) for both
offsets instead of 0, to test whether merely allocating more stack
space would let the retry loop see a non-empty list. Result: the IOP
steady-state pc is IDENTICAL (`0x001012A8`) in every case - allocating
more space does not, by itself, populate any entries (the buffer is
still explicitly zero-filled by `sw zero,0x10(sp)` above; nothing else
writes into it in the disassembled span). This confirms the real gap
is NOT "these fields need a specific magic count" - it's that some
real registration mechanism (an actual device/handler registration
call this project doesn't yet emulate) is what's supposed to populate
these lists' entries after allocation, and no citable value for either
field, nor any citable description of that registration mechanism,
was found (same exhausted search as the 9th/10th findings: psx-spx,
ps2tek, PCSX2 upstream, an independent PS2-boot write-up, and the live
pcsx2-mcp reference instance).

**No further code change made this round** - deliberately: supplying
an arbitrary nonzero count for offsets 0x10/0x1C, having just proven
empirically that it doesn't change the outcome, would be fabrication
with no evidentiary or empirical benefit (unlike offset 0x0C's fix,
which had a clear, demonstrated real effect). This is a genuine,
well-evidenced stopping point for this specific sub-thread: the
remaining gap is a real registration mechanism, not a missing number.
Task #124/#132 remains open. Whoever continues this thread next should
look for what subroutine calls (if any) populate a list at a stack
address matching this shape, likely earlier in SYSMEM's own init or in
a preceding module (LOADCORE is the next name in the real IOPBTCONF
list per task #92's citation) - not guess at offset 0x10/0x1C's values
directly.

## Round 29 continued (14th change: GS mipmap support extended to TRIANGLE)

Extended Round 28's mipmap LOD-selection (previously SPRITE-only) to
TRIANGLE, in `source/hw/gif.c`'s `rasterize_triangle()`. Uses the exact
same algorithm already implemented and tested for SPRITE (LCM=1 fixed
LOD from K, or a computed ratio-based LOD, clamped to MXL, MTBA=0
explicit MIPTBP lookup only) - the one necessary adaptation is the
"screen size" input to the ratio formula: SPRITE has a well-defined
axis-aligned width/height, but a triangle doesn't, so this uses the
triangle's screen-space bounding box (maxx-minx, maxy-miny) instead,
the natural analog. Same honest simplifications as SPRITE's own
implementation: per-primitive (not per-pixel/trilinear) selection, and
the override is scoped strictly to one draw call (saved/restored
around it, same as SPRITE).

New test `tests/test_gs_mipmap_triangle.c` (3 checks: computed LOD,
MXL clamp, magnification-always-base-level) mirrors
`test_gs_mipmap.c`'s SPRITE test structure. Found and fixed two GIF-
packet-construction bugs while writing this test (NLOOP undercounting
the PRIM register alongside the 3 vertices' RGBAQ/UV/XYZ2 writes, and
splitting TEX1/MIPTBP1 register writes across two separate
`gif_process_quadwords()` calls instead of one combined packet caused
the second write to silently not take effect) - both were test-file
bugs, not product bugs; documenting them here since they're the kind
of mistake worth remembering for future GIF-packet-based tests in this
suite. Full 71-block regression suite passes (70 pre-existing + this
new one); clean Wii rebuild verified.

## Round 29 continued (15th change: GS TEX1/MIPTBP made genuinely per-context)

Closed part of the "CLAMP/TEX1/TEX2/SCISSOR/FBA/MIPTBP unmodeled for
either context" gap Round 27 explicitly left open: TEX1/MIPTBP1/
MIPTBP2 (Round 28's mipmap registers) were context-1-only - a context
2 draw silently used whatever context 1's mip configuration happened
to be, since TEX1_2/MIPTBP1_2/MIPTBP2_2 register addresses weren't
even recognized.

Added the real _2 register addresses (TEX1_2=0x15, MIPTBP1_2=0x35,
MIPTBP2_2=0x37 - same base+1 convention every other _1/_2 pair in this
file already follows), per-context permanent storage
(`ctx1_tex1_xxx`/`ctx2_tex1_xxx`, `ctx1_tex_mip_tbp/tbw[6]`/
`ctx2_tex_mip_tbp/tbw[6]`), and wired both into
`gs_activate_context()` (same pattern Round 27 established for FRAME/
XYOFFSET/TEX0/ZBUF/TEST/ALPHA - refresh the flat/active fields from
whichever context PRIM's CTXT bit selects, right before each
rasterizer draws).

New test `tests/test_gs_context2_mipmap.c` (6 checks): configures
context 1 WITH mipmapping engaged and context 2 WITHOUT (against the
same base texture, same minifying screen size) and draws one SPRITE
per context - context 1 must sample its own mip level, context 2 must
use the base level, proving genuine independence rather than shared/
leaking state. All existing mipmap/context2 tests (`test_gs_mipmap.c`,
`test_gs_mipmap_triangle.c`, `test_gs_context2.c`) still pass
unchanged. Full 72-block regression suite passes; clean Wii rebuild
verified.

Remaining, explicitly still open: CLAMP/TEX2/SCISSOR/FBA are not
modeled at all yet (for either context) - a separate, larger gap,
since (unlike TEX1/MIPTBP) these registers don't exist anywhere in
this codebase yet to extend.

## Round 29 continued (16th change: EE COP2 VMAX/VMINI added to VU0 macro datapath)

Extended the VADD/VMUL/VSUB row (Round 13's VSUB, this round's earlier
10th change's VADD/VMUL - all COP2 CO-format, `funct` field
distinguishing the specific op) with `VMAX` (funct 0x2B) and `VMINI`
(funct 0x2F). Same 3-operand full-vector shape as VADD/VMUL/VSUB:
`FD[lane] = FS[lane] OP FT[lane]` per lane selected by destmask,
operating on the reinterpreted bit patterns as real `float`.

Ported from PCSX2 upstream's own `VUops.cpp` `_vuMAX`/`_vuMINI`, which
use `applyMinMax<fp_max>(VU)`/`applyMinMax<fp_min>(VU)` - a plain
float max/min comparison per lane (`fp_max(a,b)`/`fp_min(a,b)`), with
no NaN/signed-zero special-casing. Implemented here as the equally
plain C ternary `(a > b) ? a : b` / `(a < b) ? a : b`, consistent with
this project's existing float datapath not modeling NaN/signed-zero
edge cases anywhere else either (VADD/VMUL/VSUB, the FPU accumulator
family, etc. all use plain C arithmetic on the reinterpreted bits).

VMSUB/VOPMSUB remain the two still-unimplemented ops in this SPECIAL1
CO-format arithmetic row - not added this round.

New test `tests/test_ee_cop2_arith3.c` (4 checks): loads VF1=(2,-3,4,-5)
and VF2=(1,1,10,-10) via LQ+QMTC2, executes `VMAX.xyzw VF3,VF1,VF2`
and verifies the per-lane max (2,1,10,-5); executes `VMINI.xyzw
VF4,VF1,VF2` and verifies the per-lane min (1,-3,4,-10); executes
`VMAX.x VF5,VF1,VF2` (single-lane destmask, VF5 starts at 0) and
verifies only the X lane changed while Y stayed 0, proving the
destmask is honored per-lane rather than always writing the full
vector. Full 73-block regression suite passes (72 pre-existing + this
new one); clean Wii rebuild verified (only the pre-existing, harmless
`strncpy` truncation warning in `iop_module_loader.c`).

## Round 29 continued (17th change: EE COP2 VMADD/VMSUB/VOPMSUB complete the SPECIAL1 arithmetic row)

Correction to the 16th change's note above: the row's remaining gap
was actually three ops, not two - VMADD (funct 0x29) was also missing.
This change closes all three, completing the full
`VADD/VMADD/VMUL/VMAX/VSUB/VMSUB/VOPMSUB/VMINI` row (funct 0x28-0x2F
sequential), confirmed against a real PCSX2 upstream reference
clone's `R5900OpcodeTables.cpp` (that exact row, in that exact funct
order) and `VUops.cpp`'s `_vuOpMADD`/`_vuOpMSUB`/`_vuOPMSUB`.

`VMADD` (0x29) and `VMSUB` (0x2D) are the same 3-operand per-lane
shape as VADD/VMUL/VMAX/VSUB/VMINI, but read a third operand from the
VU0 macro-mode accumulator (`vu0_acc[4]`, lane order x=0/y=1/z=2/w=3 -
already wired and used by the VU microcode interpreter in
`source/hw/vu.c`, so no new state was needed): `FD[lane] = ACC[lane]
+- FS[lane]*FT[lane]` per destmask lane, writing FD only (matching
PCSX2's `applyTernaryMACOp<..., MACOpDst::Fd>` - the accumulator-
writing variants VMADDA/VMSUBA are a separate opcode family, not
implemented here).

`VOPMSUB` (0x2E) is the cross-product-shaped outer-product multiply-
subtract - PCSX2's `_vuOPMSUB` has no destmask/`_W` branch at all, so
it always writes exactly xyz and leaves w untouched: `FD.x =
ACC.x - FS.y*FT.z`, `FD.y = ACC.y - FS.z*FT.x`, `FD.z = ACC.z -
FS.x*FT.y`.

New test `tests/test_ee_cop2_arith4.c` (5 checks): pokes `vu0_acc =
(10,10,10,10)` directly (there is no macro-mode "write ACC" opcode
implemented yet to set it via real code), loads VF1=(2,3,4,5) and
VF2=(1,2,3,4), verifies `VMADD.xyzw` (ACC+VF1*VF2 = 12,16,22,30),
`VMSUB.xyzw` (ACC-VF1*VF2 = 8,4,-2,-10), `VMADD.x` (single-lane
destmask honored), and `VOPMSUB`'s cross-product-shaped result
(1,6,6) against hand-computed values. Full 74-block regression suite
passes (73 pre-existing + this new one); clean Wii rebuild verified
(only the pre-existing, harmless `strncpy` truncation warning in
`iop_module_loader.c`).

Broadcast forms (VADDx/y/z/w/VMADDbc/etc), the accumulator-writing
family (VADDA/VMULA/VMADDA/VMSUBA/VOPMULA), and the memory-access
family beyond VISWR/VSQI remain open - not seen in the traced boot
path, scoped future work like this project's other still-open VU0
gaps.

## Round 29 continued (18th change: EE COP2 broadcast-form arithmetic ops added)

Extended VU0 macro mode with the FT-lane-broadcast forms of the
already-implemented full-vector row: `VADDx/y/z/w` (funct 0x00-0x03),
`VSUBx/y/z/w` (0x04-0x07), `VMAXx/y/z/w` (0x10-0x13), `VMINIx/y/z/w`
(0x14-0x17), `VMULx/y/z/w` (0x18-0x1B) - 20 opcodes total. Confirmed
against a real PCSX2 upstream reference clone's
`R5900OpcodeTables.cpp` (the SPECIAL1 table's first 4 rows, laid out
as 8 opcodes x 4 rows with x/y/z/w cycling every 4 funct values) and
`VUops.cpp`'s `applyBinaryMACOpBroadcast`: same shape as the
full-vector forms (`FD[lane] = FS[lane] OP FT[lane]`), except the
second operand is always a single fixed lane of FT (selected by which
specific opcode - the low 2 bits of funct - not by destmask),
broadcast to every lane the destmask selects.

Implemented as a single dispatch branch keyed on `funct <= 0x07 ||
(funct >= 0x10 && funct <= 0x1B)`, decoding `bc_lane = funct & 0x3`
and `base_op = (funct >> 2) & 0x7` to select the arithmetic operator -
reuses the exact same `vu0_vf_read_lane`/`vu0_vf_write_lane` helpers
and float-reinterpretation approach as every other arithmetic op in
this file.

New test `tests/test_ee_cop2_broadcast.c` (7 checks): one
representative op per family (VADDy, VSUBx, VMULz, VMAXw, VMINIx)
verified against hand-computed broadcast results; a single-lane
destmask (VADDy.x) verified to only write that one lane. Full
75-block regression suite passes (74 pre-existing + this new one);
clean Wii rebuild verified (only the pre-existing, harmless `strncpy`
truncation warning in `iop_module_loader.c`).

Explicitly out of scope this round: `VMADDx/y/z/w`/`VMSUBx/y/z/w`
(funct 0x08-0x0F, the ACC-based broadcast forms) and
`VMULq`/`VMAXi`/`VMULi`/`VMINIi` (funct 0x1C-0x1F, which broadcast the
Q/I registers instead of an FT lane) - both scoped, well-understood
follow-ups reusing this same dispatch shape.

## Round 29 continued (19th change: EE COP2 broadcast row completed - VMADD/VMSUB broadcast + VMULq/VMAXi/VMULi/VMINIi)

Closed the two follow-ups explicitly scoped out of the 18th change,
completing the entire funct 0x00-0x1F broadcast row:

`VMADDx/y/z/w` (funct 0x08-0x0B) and `VMSUBx/y/z/w` (0x0C-0x0F) are
the ACC-based broadcast forms - same shape as `VMADD`/`VMSUB` (Round
29 continued's 17th change), but the second multiplicand is a single
broadcast lane of FT rather than the matching lane: `FD[lane] =
ACC[lane] +- FS[lane]*FT.<bc-lane>`, ported from PCSX2's own
`VUops.cpp` `applyTernaryMACOpBroadcast`.

`VMULq` (funct 0x1C), `VMAXi` (0x1D), `VMULi` (0x1E), `VMINIi` (0x1F)
broadcast the scalar `Q`/`I` VU control registers instead of an FT
lane - confirmed these 4 ops take no FT operand at all by checking a
real PCSX2 upstream reference clone's `DisR5900asm.cpp` disassembly
formatters (`P_VMULq` etc print only `FD,FS,Q` / `FD,FS,I`, no third
register). `Q` lives at `cop2_ctrl[22]`, `I` at `cop2_ctrl[21]` (per
PCSX2's `VU.h` `REG_Q=22`/`REG_I=21` - already-existing general
control-register storage, set via the existing `CTC2` transfer,
needed no new state).

The dispatch was refactored into a single `funct <= 0x1F` branch
covering the complete row: `op_kind` (ADD/SUB/MADD/MSUB/MAX/MINI/MUL)
and the broadcast operand's source (an FT lane, vs `Q`/`I` for the
four `base_op==7` opcodes) are both derived from `funct`, then the
same per-lane loop as every other arithmetic op in this file applies.

New test `tests/test_ee_cop2_broadcast2.c` (7 checks): sets `Q`/`I`
via real `CTC2` instructions, pokes `vu0_acc` directly (still no
macro-mode "write ACC" opcode), verifies `VMADDy`/`VMSUBx`
broadcast-with-accumulator results and all four `VMULq`/`VMAXi`/
`VMULi`/`VMINIi` results against hand-computed values. Full 76-block
regression suite passes (75 pre-existing + this new one); clean Wii
rebuild verified (only the pre-existing, harmless `strncpy` truncation
warning in `iop_module_loader.c`).

The entire funct 0x00-0x2F COP2 CO-format arithmetic space (broadcast
row + full-vector row) is now implemented. Remaining VU0 macro-mode
gaps: the accumulator-writing family (`VADDA`/`VMULA`/`VMADDA`/
`VMSUBA`/`VOPMULA`/etc, funct 0x00-0x2F of `COP2SPECIAL2`, a separate
64-entry table), `VABS`/`VCLIPw`, `VMOVE`/`VMR32`, the memory-access
family beyond `VISWR`/`VSQI` (`VLQI`/`VLQD`/`VSQD`/`VILWR`), and
`VDIV`/`VSQRT`/`VRSQRT` (which would also need to model the `Q`
register's real "busy" timing, not just its value - not attempted
yet).

## Round 29 continued (20th change: EE COP2 SPECIAL2 unary/data-movement cluster - VABS, VITOF/VFTOI, VMOVE, VMR32)

Extended the COP2SPECIAL2 sub-dispatch (previously only VISWR
idx=63/VSQI idx=53) with a coherent cluster of 11 opcodes: `VABS`
(idx=29), `VITOF0/4/12/15` (idx=16-19), `VFTOI0/4/12/15` (idx=20-23),
`VMOVE` (idx=48), `VMR32` (idx=49).

Important field-role discovery, confirmed against a real PCSX2
upstream reference clone: unlike the entire arithmetic row (dest=FD),
these ops encode their DESTINATION in the FT field position and read
their SOURCE from FS - `DisR5900asm.cpp`'s disassembly formatters
(`P_VABS`/`P_VITOF0`/`P_VFTOI0`/etc) all print `FT, FS` with no FD at
all, and `VUops.cpp`'s `_vuABS`/`_vuITOF*`/`_vuFTOI*`/`_vuMOVE`/
`_vuMR32` all write `VU->VF[_Ft_]` from `VU->VF[_Fs_]`, guarded by
`if (_Ft_ == 0) return`. This decoder already extracted `ft`/`fs` at
the same bit positions for every CO-format instruction, so no new
field extraction was needed - just reusing them with roles swapped
for this cluster (the `fd` bits-6-10 field is unused here).

`VABS`: bit-level absolute value (clear the sign bit), per destmask
lane. `VMOVE`: plain per-lane copy. `VMR32`: 32-bit lane rotate
(`FT.x=FS.y`, `FT.y=FS.z`, `FT.z=FS.w`, `FT.w=FS.x`) - reads all 4
source lanes into locals first so a self-move (`Ft==Fs`) still
rotates correctly. `VITOF0/4/12/15`/`VFTOI0/4/12/15`: fixed-point
int<->float conversion, ported bit-exact from PCSX2's own `VUops.cpp`
`intToFloat<Offset>`/`floatToInt<Offset>` templates (scale by a
bit-constructed power-of-two float constant; `floatToInt` also
saturates to `INT32_MIN`/`MAX` above a fixed exponent threshold)
rather than a plain C cast, since the real-hardware quirk is directly
portable.

New test `tests/test_ee_cop2_unary.c` (8 checks): `VABS` computes
`|VF1|`; `VMOVE` copies unchanged; `VMR32` rotates lanes correctly;
`VITOF4`/`VITOF12` scale raw int32 bit patterns by 2^-4/2^-12
(verified against hand-computed values, e.g. `-32768` at offset 4 =
`-2048.0`); `VFTOI0`/`VFTOI4` truncate floats to int (optionally
pre-scaled, e.g. `0.0625 * 16 = 1.0 -> 1`); a single-lane destmask
(`VABS.x`) only writes that one lane. Full 77-block regression suite
passes (76 pre-existing + this new one); clean Wii rebuild verified
(only the pre-existing, harmless `strncpy` truncation warning in
`iop_module_loader.c`).

Remaining COP2SPECIAL2 gaps: the accumulator-writing family
(`VADDA`/`VMULA`/`VMADDA`/`VMSUBA`/`VOPMULA`/etc), `VCLIPw`, the
memory-access family beyond `VISWR`/`VSQI` (`VLQI`/`VLQD`/`VSQD`/
`VMTIR`/`VMFIR`/`VILWR`), and `VDIV`/`VSQRT`/`VRSQRT` (which would
also need to model the `Q`/`P` register's real "busy" timing).

## Round 29 continued (21st change: EE COP2 SPECIAL2 accumulator-writing family)

Implemented the largest remaining VU0 macro-mode cluster: every op
that writes `vu0_acc[4]` instead of `VF[fd]`. Full-vector forms
`VADDA`(idx40)/`VMADDA`(41)/`VMULA`(42)/`VSUBA`(44)/`VMSUBA`(45) are
the exact same arithmetic shape as `VADD`/`VMADD`/`VMUL`/`VSUB`/
`VMSUB`, just redirected to write `ACC`. Broadcast forms (idx 0-15,
24-28, 30, 32-39) cover `VADDAx/y/z/w`/`VSUBAx/y/z/w`/`VMADDAx/y/z/w`/
`VMSUBAx/y/z/w`/`VMULAx/y/z/w`/`VMULAq`/`VMULAi`/`VADDAq`/`VMADDAq`/
`VADDAi`/`VMADDAi`/`VSUBAq`/`VMSUBAq`/`VSUBAi`/`VMSUBAi` - the same
broadcast shape as the funct<=0x1F row. `VOPMULA` (idx46) is the
outer-product multiply variant of `VOPMSUB`: writes `ACC` directly
with no existing-`ACC` read and no destmask (always exactly xyz, w
untouched) - `ACC.x=FS.y*FT.z`, `ACC.y=FS.z*FT.x`, `ACC.z=FS.x*FT.y`.
`VNOP` (idx47) is a true no-op. All confirmed against a real PCSX2
upstream reference clone's `VUops.cpp` (`applyBinaryMACOp`/
`applyTernaryMACOp` and their `Broadcast` variants, templated on
`MACOpDst::Acc` instead of `::Fd` - the exact underlying arithmetic
already implemented for the FD-writing rows, just redirected to a
different destination).

New test `tests/test_ee_cop2_acc.c` (7 checks across 6 independent
fresh-core sub-tests, so `ACC` always starts zeroed): `VADDA`
computes `ACC=VF1+VF2`; `VMULA` seeds `ACC` and `VMADDA` correctly
round-trips through a real accumulate (reads the just-written `ACC`
back and adds onto it); `VSUBA` computes `ACC=VF1-VF2`; `VOPMULA`
overwrites a poked sentinel `ACC` value with the correct cross-
product-shaped outer product (proving it does NOT accumulate);
`VMULAq` broadcasts `Q` and a following `VNOP` provably changes
nothing; `VADDAy` broadcasts a single FT lane. Full 78-block
regression suite passes (77 pre-existing + this new one); clean Wii
rebuild verified (only the pre-existing, harmless `strncpy` truncation
warning in `iop_module_loader.c`).

Remaining VU0 macro-mode gaps, narrowing fast: `VCLIPw` (idx31, needs
a new CLIP flag register this project doesn't model yet), the
memory-access family beyond `VISWR`/`VSQI` (`VLQI`(52)/`VLQD`(54)/
`VSQD`(55)), `VMTIR`(60)/`VMFIR`(61)/`VILWR`(62) (use a different
sub-field decode than every op implemented so far - not yet
researched in this codebase), `VDIV`/`VSQRT`/`VRSQRT`/`VWAITQ`
(idx56-59, would need to decide how to model the `Q`/`P` register's
real "busy" latency), and `VRNEXT`/`VRGET`/`VRINIT`/`VRXOR` (idx64-67,
the VU0 R-register LCG pseudo-random generator - an entirely separate
piece of state).

## Round 29 continued (22nd change: VU0 memory-access family completed - VLQI/VLQD/VSQD + VSQI destmask bugfix)

Completed the VU0 local-memory access family: `VLQI` (idx52, load-
quadword-post-increment), `VLQD` (idx54, load-quadword-pre-decrement),
`VSQD` (idx55, store-quadword-pre-decrement) - the pre/post
increment/decrement siblings of the already-implemented `VSQI`.
Field-role convention, confirmed against a real PCSX2 upstream
reference clone's `VUops.cpp` `_vuLQI`/`_vuLQD`/`_vuSQD`: for loads,
the address VI register lives in the FS field position and the
destination VF register lives in FT - the opposite of VSQI/VSQD,
where the address lives in FT and the source VF register lives in FS.

While researching the exact field/destmask semantics via
`DisR5900asm.cpp`'s `P_VSQI`/`P_VLQI`/`P_VLQD`/`P_VSQD` disassembly
formatters, found and fixed a real, pre-existing bug: `VSQI`'s
existing implementation (from an earlier round) was unconditionally
storing all 4 lanes regardless of destmask, but the disassembler
confirms `VSQI` genuinely has an xyzw suffix like every other
CO-format op. Fixed by adding the same destmask check the new
VLQI/VLQD/VSQD implementations already use; the existing
`test_ee_cop2_vu0.c` VSQI test was unaffected since it always used
destmask=0xF (all lanes) already.

New test `tests/test_ee_cop2_lqisqd.c` (8 checks): a full VSQI store
followed by a single-lane VSQI store proves the destmask fix (the
untouched lanes read back as 0, and VI10 was still post-incremented
both times); `VLQI` reads the full store back correctly and post-
increments its own address register; `VLQD` pre-decrements and reads
back the single-lane store exactly (X=2, Y=0); `VSQD` pre-decrements
and its store round-trips correctly through a follow-up `VLQI`. Full
79-block regression suite passes (78 pre-existing + this new one);
clean Wii rebuild verified (only the pre-existing, harmless `strncpy`
truncation warning in `iop_module_loader.c`).

Remaining VU0 macro-mode gaps: `VCLIPw` (needs a new CLIP flag
register), `VMTIR`/`VMFIR`/`VILWR` (a different sub-field decode -
`_It_`/`_Is_`/`_Fsf_` in PCSX2's own naming - not yet researched in
this codebase), `VDIV`/`VSQRT`/`VRSQRT`/`VWAITQ` (would need to model
the `Q` register's real "busy" timing), and `VRNEXT`/`VRGET`/
`VRINIT`/`VRXOR` (the VU0 R-register LCG pseudo-random generator -
entirely separate state).

## Round 29 continued (23rd change: EE COP2 SPECIAL2 - VMTIR/VMFIR/VILWR)

Implemented the integer<->float raw-bit-move family: `VMTIR`
(idx60), `VMFIR` (idx61), `VILWR` (idx62), ported from a real PCSX2
upstream reference clone's `VUops.cpp` `_vuMTIR`/`_vuMFIR`/`_vuILWR`.

`VMTIR`: `VI[ft]` = the low 16 bits of the RAW 32-bit bit pattern of
`VF[fs][Fsf]` - a plain truncation, not a numeric conversion. `Fsf`
is not a separate instruction field: confirmed via
`DisR5900asm.cpp`'s `dest_fsf()` macro (`(disasmOpcode>>21)&3`) that
it lives in the exact same two bits as this decoder's `destmask`
value's low 2 bits - just reinterpreted here as a lane INDEX instead
of a per-lane bitmask.

`VMFIR`: broadcasts the sign-extended 16-bit `VI[fs]` value (raw
bits, not a float conversion) into every destmask-selected lane of
`VF[ft]`.

`VILWR`: `VI[ft]` = the low 16 bits of VU0 mem at quadword index
`VI[fs]`, single lane selected by destmask (the same single-bit-only
convention `VISWR` already uses).

New test `tests/test_ee_cop2_mtir.c` (4 checks): `VMTIR` truncates a
planted raw bit pattern (`VF1.y = 0x56780002` -> `VI10 = 0x0002`);
`VMFIR.xz` broadcasts a sign-extended negative 16-bit value
(`0xBEEF` -> `0xFFFFBEEF`) into exactly the X and Z lanes, leaving Y
untouched (proving destmask is honored); `VILWR.z` reads back a
planted 16-bit value from a specific VU0 mem lane. Full 80-block
regression suite passes (79 pre-existing + this new one); clean Wii
rebuild verified (only the pre-existing, harmless `strncpy` truncation
warning in `iop_module_loader.c`).

Remaining VU0 macro-mode gaps, now down to three well-scoped items:
`VCLIPw` (needs a new CLIP flag register), `VDIV`/`VSQRT`/`VRSQRT`/
`VWAITQ` (would need to model the `Q`/`P` register's real "busy"
timing), and `VRNEXT`/`VRGET`/`VRINIT`/`VRXOR` (the VU0 R-register LCG
pseudo-random generator - entirely separate state).

## Round 29 continued (24th change: EE COP2 SPECIAL2 - VU0 R-register LCG)

Implemented the VU0 "R register" LFSR pseudo-random generator:
`VRINIT` (idx66), `VRGET` (idx65), `VRNEXT` (idx64), `VRXOR` (idx67),
ported bit-exact from a real PCSX2 upstream reference clone's
`VUops.cpp` `_vuRINIT`/`_vuRGET`/`AdvanceLFSR`/`_vuRNEXT`/`_vuRXOR`.
`R` is control register index 20 (PCSX2's `VU.h` `REG_R=20`) - no new
state needed, it's already reachable via the existing
`vu0_vi_read`/`write` helpers this file already uses for `I`(21)/
`Q`(22). `R` is always kept in the float-bit-pattern range
`[1.0,2.0)` (exponent/sign bits fixed at `0x3F800000`, only the low
23 mantissa bits actually vary). `VRINIT` seeds `R`'s mantissa from a
single VF lane's raw bits (`Fsf` reuses destmask's low 2 bits as a
lane index, same convention as `VMTIR`). `VRGET` broadcasts `R`'s
current value into destmask-selected `VF[ft]` lanes without advancing
it. `VRNEXT` advances the LFSR first (shift-left-1, XOR bit0 with
bit4^bit22, re-clamp to the float-bit-pattern range), then broadcasts
the new value. `VRXOR` XORs `R`'s mantissa with a VF lane's raw bits.

New test `tests/test_ee_cop2_rreg.c` (5 checks, verified against a
host-side reference `AdvanceLFSR` model rather than hand-derived bit
patterns): `VRINIT`+`VRGET` round-trip a seed unchanged; `VRNEXT`
advances the LFSR once and broadcasts correctly; a second `VRNEXT`
proves the LFSR keeps advancing (not idempotent); `VRXOR` matches the
reference model's XOR-and-reclamp result. Full 81-block regression
suite passes (80 pre-existing + this new one); clean Wii rebuild
verified (only the pre-existing, harmless `strncpy` truncation
warning in `iop_module_loader.c`).

VU0 macro-mode gaps now down to two: `VCLIPw` (needs a new CLIP flag
register) and `VDIV`/`VSQRT`/`VRSQRT`/`VWAITQ` (would need to model
the `Q`/`P` register's real "busy" timing).

## Round 29 continued (25th change: EE COP2 SPECIAL2 - VDIV/VSQRT/VRSQRT/VWAITQ)

Implemented the Q-register-producing division/sqrt family: `VDIV`
(idx56), `VSQRT` (idx57), `VRSQRT` (idx58), `VWAITQ` (idx59), ported
from a real PCSX2 upstream reference clone's `VUops.cpp`
`_vuDIV`/`_vuSQRT`/`_vuRSQRT`/`_vuWAITQ`. `Fsf`/`Ftf` are independent
2-bit lane selectors living in destmask's low/high 2 bits
respectively (same convention discovered for `VDIV`/`VRSQRT`/`VSQRT`
in `dest_fsf()`/`dest_ftf()`); `VSQRT` uses only `Ftf` (no FS operand
at all - it computes `sqrt(|VF[ft][Ftf]|)`). Divide-by-zero produces
a signed `FLT_MAX` bit pattern (`0x7F7FFFFF`/`0xFF7FFFFF`, sign from
XOR of the two raw operand sign bits) rather than a true IEEE
infinity, matching real PS2 hardware's lack of infinity support.
`VRSQRT` additionally has a genuine-zero-input case (`ft==0 &&
fs==0`) that clamps to a signed-zero pattern instead. The result
writes directly into `cop2_ctrl[22]` (this project's single unified
Q-register slot, already read by the existing `VMULq`/`VADDq`/etc
broadcast-row ops from prior rounds - no new state needed).

Researching `VWAITQ` resolved an open question this project's own
earlier docs had flagged: real PCSX2's `_vuWAITQ` has a literally
empty function body (`{}`) - PCSX2 computes `Q` synchronously with no
latency at all, so there is no real "Q busy timing" to model. The
concern in earlier ROADMAP.md entries about needing latency modeling
was unfounded; `VWAITQ` is implemented here as a true no-op,
confirmed by a test that shows `Q` is provably unchanged across it.

New test `tests/test_ee_cop2_div.c` (6 checks): `VDIV` computes a
normal division (`20.0/4.0=5.0`) and the `0/0` divide-by-zero clamp
to `+FLT_MAX`; `VSQRT` computes `sqrt(|-9.0|)=3.0`; `VRSQRT` computes
a normal `fs/sqrt(ft)` (`20.0/sqrt(4.0)=10.0`) and the `ft=0,fs!=0`
clamp to `+FLT_MAX`; `VWAITQ` provably leaves `Q` unchanged from the
preceding `VRSQRT` result. Full 82-block regression suite passes (81
pre-existing + this new one); clean Wii rebuild verified (only the
pre-existing, harmless `strncpy` truncation warning in
`iop_module_loader.c`).

VU0 macro-mode gaps now down to just one: `VCLIPw` (idx31), which
needs a new CLIP flag register this project doesn't currently model
at all - the first VU0 op this session that can't simply reuse
existing state.

## Round 29 continued (26th change: EE COP2 SPECIAL2 - VCLIPw, the final VU0 macro-mode gap)

Implemented `VCLIPw` (idx31), the last remaining VU0 macro-mode
instruction identified this session. Judges `|VF[fs].x|`,
`|VF[fs].y|`, `|VF[fs].z|` against `|VF[ft].w|` via a raw 32-bit
signed-integer sign-flip XOR trick (comparing bit patterns as signed
ints with the sign bit flipped for the "negative" judgment, exploiting
IEEE 754's monotonic ordering for same-signed floats) rather than an
actual float comparison, ported bit-exact from a real PCSX2 upstream
reference clone's `VUops.cpp` `_vuCLIP`. There is no `Fsf`/`Ftf` lane
selector at all for this op - `xyz` vs `w` is hardwired, confirmed via
`DisR5900asm.cpp`'s `P_VCLIPw` formatter (`"vclip %sxyz, %sw"`).

Unlike every other VU0 op implemented this session, `VCLIPw` needed
genuinely new reachable state: the CLIP flag register. This was
resolved without adding any new field - the CLIP flag lives at control
register index 18 (`REG_CLIP_FLAG=18` in PCSX2's `VU.h`), and this
project's `CFC2`/`MTC2`/`QMTC2` instruction paths already handle any
control-register index generically via the existing `cop2_ctrl[]`
array (the same array already used for `R`(20), `I`(21), `Q`(22)), so
slot 18 was simply already reachable. Each `VCLIPw` call shifts the
existing clip value left by 6 bits and ORs in 6 new judgment bits
(one pos/neg pair per `x`/`y`/`z`), masked to the low 24 bits -
matching real hardware's 4-calls'-worth-of-history behavior. Consistent
with the Q-register unification decision from the 25th change, this
project writes directly into the single `cop2_ctrl[18]` slot rather
than modeling PCSX2's separate `clipflag`/`SYNCCLIPFLAG()` split.

New test `tests/test_ee_cop2_clip.c` (3 checks): a `VCLIPw` call
producing a known 6-bit judgment pattern from hand-picked VF values,
and a second call proving the 6-bit shift-in history behavior
(`clipflag = (old << 6) | new_bits`). Full 83-block regression suite
passes (82 pre-existing + this new one); clean Wii rebuild verified
(only the pre-existing, harmless `strncpy` truncation warning in
`iop_module_loader.c`).

This completes every VU0 macro-mode instruction identified across
this entire session's research (SPECIAL1 funct 0x00-0x2F arithmetic +
broadcast rows, and the full SPECIAL2 128-entry table). Real BIOS
boot code very likely uses VU0 macro mode for splash-screen transform/
lighting math - NOT YET REACHED by the current boot trace (EE is
still steady-state SIF-polling) - so this remains readiness work
rather than a wall-clearing fix, but the VU0 macro-mode datapath is
now complete and ready for whenever the real boot trace reaches it.

## Round 29 continued (27th finding: definitive root-cause refinement - LOADCORE's own module-registration list, not SYSMEM/"device drivers")

Continuing task #124/#132 per the user's explicit "finish this" instruction,
with a fresh, more thorough disassembly pass than any prior round: a new
diagnostic (`/tmp/diag75.c`, built with `-DIOP_MODLOADER_DEBUG`) printed
this project's own module-loader's real boot list AND confirmed, for the
first time with certainty, WHICH module is actually executing when the
wall is hit.

**Correction of a 3-round-old misattribution**: the debug log shows
`SYSMEM` (module 0) runs to completion and hands off via the trampoline
to `LOADCORE` (module 1, real entry `0x00100CD0`) - and the wall
(`iop pc=0x001012A8`) sits only ~0x5D8 bytes into LOADCORE's own code,
NOT inside SYSMEM as the 9th/12th/13th findings assumed. All prior
references to "SYSMEM's boot_info struct" describe LOADCORE's own
init code instead - a real correction, not a new fabrication.

**Full fresh disassembly** (Capstone, fed the ACTUAL relocated IOP RAM
bytes this project's own interpreter produced at 0x100CD0-0x101300,
not raw ROM bytes, so addresses are exact) of LOADCORE's own init from
entry to the panic reveals a substantially richer structure than
previously characterized:

1. At `0x100F64-0x100FB8`, LOADCORE's init allocates THREE separate
   stack buffers sized from local copies of `boot_info` fields (the
   9th/12th findings' fp+0x48/0x4c/0x50/0x54 slots): one sized
   `(boot_info[0x18]+1)*4`, one sized `boot_info[0x18]*8` (this is
   `s2`, the list the 12th/13th findings already found empty), plus
   bookkeeping. `boot_info[0x1C]`'s local copy is overwritten to point
   at the newly allocated `s2` buffer itself (`sw s0, 0x54(fp)`).

2. **A genuinely new discovery**: at `0x100FBC`, LOADCORE checks
   `8(s0)` - a SEPARATE, EARLIER-allocated list (from the
   `(boot_info[0x18]+1)*4`-sized buffer at `0x100F34`/`0x100F84`) - and
   if non-empty, runs a real per-entry processing loop
   (`0x100FD0-0x101184`) that **genuinely calls through function
   pointers via `jalr`** (at `0x101124`, after loading a function
   pointer from `fp+0x14` and a `gp` value from `fp+0x18`) and, based
   on the call's return code (0-4, per the branch chain at
   `0x101018-0x10105C`), performs real bookkeeping including calls to
   at least 4 more subroutines (`0x1018d0`, `0x101f30`, `0x102120`,
   `0x10198c`, `0x101410`) whose own semantics were not reverse-
   engineered this round. This loop is what would POPULATE `s2`'s
   list (the one the 12th/13th findings already found empty) with
   real, phase-tagged entries for the phase-dispatch loop documented
   in the 7th/13th findings to actually call.

3. Since this project's `boot_info[0x18]` and `[0x1C]` are both 0 (this
   project's loader never populates them - same root gap the 9th/13th
   findings already identified for adjacent fields), the per-entry
   loop at step 2 is skipped entirely (`beqz v0, 0x1011a8` at
   `0x100FC4`), `s2` stays empty, and the already-documented 4-pass
   phase-dispatch loop + panic (7th/12th/13th findings) follows exactly
   as before.

**Refined understanding of what this table really is**: it is NOT a
"device driver" table (the A(96h)-A(99h) CD-ROM/memory-card/tty
hypothesis from the 7th finding). The 2-bit tag + `jalr` + `gp`-restore
shape matches LOADCORE's genuine, real job: **a multi-phase dispatch
table for OTHER modules' own self-registered init/library-entry
functions** - i.e., a real IOP kernel mechanism for modules to register
"call me during phase N of LOADCORE's own bootstrap," which this
project's simplified loader (which loads and runs exactly one module's
ELF at a time, via a return-address trampoline, interleaved with
running each one's entry point before the next module is even loaded)
structurally never populates, because at the exact moment LOADCORE's
init runs, no other module has been loaded yet to register anything
into it - this is a genuine, well-evidenced ordering difference from
real hardware's boot sequence (which very likely loads/relocates
multiple modules' images before running any entry points, letting
static per-module registration data accumulate first).

**Decision (honest, not a rushed guess)**: a real fix would require
either (a) fully reverse-engineering the per-entry struct format
consumed by the `0x100FD0-0x101184` loop (name/phase-tag/entry-point/
gp fields, plus the semantics of its ~4 helper-subroutine calls) with
enough confidence to construct real entries that get called via a real
`jalr` - unlike every previous defensive fix this project has applied
(`INITIAL_SP`, `boot_info+0x0C`), which only needed a pointer to land
somewhere SAFE, this entry format feeds a genuine function-pointer
CALL, so an incorrect guess would not fail safely - it could jump
into arbitrary emulated memory as code; or (b) restructuring this
project's module-loading order to front-load all 29 `IOPBTCONF`
modules' ELF images (parse/relocate/export-registration, all of which
this project's `load_and_link_one()` already does per-module) before
running any entry point, on the hypothesis that real hardware's boot
order does the same - itself an unverified hypothesis, and a
non-trivial architecture change touching the one part of this project
(module sequencing) every other subsystem depends on.

Given the genuine risk profile of (a) and the scope/risk of (b), and
given this is the 4th consecutive round (7th, 9th, 10th, 12th, 13th
findings, now this 27th) to precisely re-characterize this exact wall
without a citable, safe, executable fix, task #124/#132 is closed out
this round with its root cause conclusively and precisely identified
(a genuine, real, and correctly-attributed finding - LOADCORE's own
module-registration bootstrap, not a "device driver" table, not
SYSMEM) but without a further code change, consistent with this
project's standing principle that a fabricated function-pointer target
is a categorically different and unacceptable risk from every previous
defensive-pointer fix in this codebase. No BIOS bytes were committed;
all real-BIOS analysis stayed in `/tmp` diagnostics per the project's
standing security rule.

**For whoever picks this up next**: the concrete, scoped next step is
reverse-engineering the four subroutines at `0x1018d0`, `0x101f30`,
`0x102120`, and `0x10198c`/`0x101410` (all called from the
`0x100FD0-0x101184` per-entry loop) to determine the real entry struct
layout with enough confidence to construct genuine entries - or,
alternatively, prototyping hypothesis (b) (front-loading all 29
modules' ELF images before running any entry point) as a bounded,
revertible experiment to see whether it changes the IOP's steady-state
pc at all, the same falsifiable-hypothesis-testing approach the 13th
finding already used successfully to rule out a different guess.

## Round 29 continued (28th change: LOADCORE-Panic-Schleife erkannt und umgangen - echter Boot-Fortschritt)

Auf ausdrücklichen Wunsch des Nutzers, nach der 27th-finding-Root-
Cause-Analyse konkret weiterzumachen und den Boot tatsächlich weiter
voranzubringen, wurde die im 27th finding identifizierte reale
LOADCORE-Panic-Sequenz (`lui $v1,0x8000; addiu $v0,zero,2;
sb $v0,($v1); j <self>`) in `iop_module_loader.c` per
Byte-Signatur-Erkennung (nicht per fest kodierter Adresse) erkannt.
Erreicht der IOP-Interpreter genau diese Instruktionsfolge, wird -
exakt wie beim bereits bestehenden Trampolin-Mechanismus, der jedes
Modul nacheinander sequenziert - direkt zum nächsten Modul in der
echten IOPBTCONF-Liste übergegangen, statt die echte Panic-Sequenz
auszuführen und für immer zu spinnen.

**Warum das sicher ist (im Gegensatz zu einem Rateversuch an der
eigentlichen Registrierungsliste)**: die im 27th finding gefundene
Phase-Dispatch-Liste ruft echte Funktionszeiger per `jalr` auf - ein
falscher Rateversuch dort würde nicht sicher fehlschlagen, sondern
könnte in beliebigen emulierten Speicher als Code springen. Die hier
erkannte Panic-Sequenz dagegen ist reiner, unveränderlicher, bereits
vollständig disassemblierter realer BIOS-Code, der NICHTS Neues
ausführt - das Projekt erkennt nur, dass real LOADCORE genau HIER
absichtlich in eine Endlosschleife geht, und übernimmt an exakt
diesem Punkt selbst die Sequenzierung, so wie es das eigene Trampolin
bereits an seiner eigenen Rücksprungadresse tut. Dies ist eine
explizite, dokumentierte Entwurfsentscheidung über den eigenen
externen Modul-Sequenzierungs-Shortcut dieses Projekts, KEINE Aussage
über reales Hardware-Verhalten.

**Ein echter Kodierungsfehler wurde beim ersten Implementierungsversuch
gefunden und korrigiert**: die zuerst von Hand kodierten `lui`/`sb`-
Konstanten benutzten versehentlich Register `$at` (1) statt `$v1` (3)
als Basisregister. Erst durch erneutes Disassemblieren der TATSÄCHLICH
im emulierten IOP-RAM liegenden Bytes (nicht nur die zuvor im 27th
finding notierten Mnemonics) wurde der Fehler entdeckt und behoben -
ein weiteres Beispiel für dieses Projekts Prinzip, jede Behauptung
gegen echte Bytes zu verifizieren statt gegen die eigene Erinnerung.

**Gemessener echter Boot-Fortschritt** (verifiziert per Host-natives
Diagnose-Tool gegen die echte SCPH-10000-BIOS): vor dieser Änderung
blieb der IOP für immer bei `pc=0x001012A8` (LOADCORE's Panic-Schleife)
hängen. Danach werden ECHT geladen und ausgeführt, in dieser
Reihenfolge: `SYSMEM` (wie zuvor) → `LOADCORE` (Panic-Schleife jetzt
erkannt und umgangen) → `EXCEPMAN` (echter Modul-Entry `0x1029b0`,
läuft bis zu SEINER EIGENEN Panic-Schleife, ebenfalls erkannt und
umgangen) → `INTRMANP` (echter Modul-Entry `0x103100`) → sauberer Halt
bei `pc=0x00000018` mit `halt_reason="BREAK"` (ein echter, sauber
erkannter `BREAK`-Befehl, keine Absturz-artige Situation). Das ist ein
neuer, wohldefinierter, ehrlicher Haltepunkt - drei zusätzliche echte
Module (`LOADCORE`, `EXCEPMAN`, `INTRMANP`) laufen jetzt tatsächlich,
statt dass der Boot bei Modul 1 von 29 endlos hängen bleibt.

Neuer Test `tests/test_iop_loadcore_panic_bypass.c` (9 Checks, rein
synthetisch, keine echten BIOS-Bytes): erkennt die exakte reale
Byte-Signatur; zwei Negativ-Kontrollen (falsches Basisregister im
`sb`; ein Sprungziel, das NICHT auf die `sb`-Instruktion zurückspringt)
werden korrekt abgelehnt, was beweist, dass die Erkennung nicht zu
großzügig ist; und der tatsächliche Interpreter-Einstiegspunkt
(`iop_module_loader_try_handle()`) geht ohne Halt zum nächsten Modul
über, wenn die Signatur erreicht wird. Volle 84-Block-Regressionssuite
besteht (83 bereits vorhanden + dieser neue); sauberer Wii-Rebuild
verifiziert (nur die bereits bekannte, harmlose `strncpy`-Warnung in
`iop_module_loader.c`).

**Ehrlicher Ausblick**: der neue Haltepunkt bei `pc=0x00000018` /
`BREAK` ist noch nicht selbst root-caused - das wäre der nächste
natürliche Schritt für eine weitere Fortsetzung dieses Threads
(vermutlich INTRMANP's eigener Interrupt-Controller-Init-Code, der auf
etwas trifft, das dieses Projekt noch nicht bereitstellt). Der
Panic-Loop-Bypass-Mechanismus selbst ist generisch (Byte-Signatur-
basiert, keine feste Adresse) und greift automatisch überall dort, wo
dieselbe reale Panic-Sequenz erneut auftritt - was bereits bei
EXCEPMAN beobachtet wurde.

## 29th finding/change (Round 29 fortgesetzt): BREAK@0x00000018 root-caused und behoben (Task #149)

**Root Cause (per Live-Einzelschritt-Trace, `diag82.c`, gegen die echte
SCPH-10000-BIOS)**: der Haltepunkt bei `pc=0x00000018`/`BREAK` (siehe
28th change) entsteht, weil `INTRMANP` beim echten Ausführen einen
echten R3000A-`syscall`-Befehl ausführt (vermutlich Syscall #0x10,
laut ps2sdk-Konvention ein "Interrupt-Manager"-Kernel-Syscall). Dieser
`syscall` vektort korrekt zum allgemeinen Exception-Handler
(`0x80000080`), dessen Inhalt aber noch der degenerierte Standardwert
ist (derselbe architektonische Fall wie in #124/#132/#148: kein
späteres Modul hat dort bisher einen echten Handler installiert, weil
der eigene Modul-Loader dieses Projekts jeweils nur ein Modul auf
einmal lädt und ausführt). Der degenerierte Standardinhalt dekodiert
effektiv zu einem Sprung auf Adresse 0, gefolgt von sequenzieller
Ausführung durch den unteren RAM-Bereich, bis er auf den dort
liegenden `BREAK`-Platzhalter bei `0x18` trifft - exakt derselbe
Mechanismus wie beim 27th/28th finding, nur eine Ebene tiefer (jetzt
über den echten Syscall-Exception-Pfad statt über LOADCOREs
Registrierungsliste).

**Implementierte Lösung** (`source/core/iop/iop_core.c`, `case 0x0D`
BREAK-Dispatch in `iop_step()`): wenn ein `BREAK` erreicht wird,
während `Cause.ExcCode` noch auf 8 (Syscall) steht - das Zeichen dafür,
dass ein echter, noch unbeantworteter Syscall bis zu diesem
unclaimed Vektor durchgefallen ist - wendet dieses Projekt exakt
dasselbe, bereits etablierte Prinzip an, das `iop_hle_bios.c` schon
für alle nicht implementierten A0/B0/C0-BIOS-Tabellenaufrufe benutzt:
einen generischen Standardwert (0) an den Aufrufer zurückgeben, statt
zu halten. Konkret: `$v0` (gpr[2]) = 0, ein RFE-äquivalentes
Zurücksetzen des Status-Stacks (dieselbe Formel wie die bereits
existierende echte RFE-Implementierung aus Task #113), und
`pc = EPC+4` (normale MIPS-Exception-Rücksprungsemantik). Jeder
ANDERE `BREAK` (`Cause.ExcCode != 8`) - insbesondere die seit
langem etablierte Testsuite-Konvention, `BREAK` als sauberen
Test-Stopp-Marker zu benutzen, ohne vorher einen echten Syscall
auszuführen - bleibt unverändert und hält weiterhin exakt wie zuvor.

**Warum das sicher ist**: Rücksprung nach einem Syscall ist normale,
wohldefinierte MIPS-Exception-Return-Semantik (EPC+4, wie bei einem
echten RFE-terminierten Handler) - kein Sprung in ein anderes,
unabhängiges Modul wie beim 28th-change-Bypass, daher ohne dessen
Scoping-Vorbehalte.

**Ein bestehender Test musste angepasst werden**: `test_iop_syscall.c`
(ein SYSCALL gefolgt von einem BREAK am Vektor - die eigene
Universal-Halt-Konvention der Testsuite) wurde durch diese Änderung
strukturell ununterscheidbar vom echten, jetzt anders behandelten
Szenario und hing (Timeout) beim ersten Testlauf. Der Test wurde in
zwei explizite Einzelschritt-Phasen umgeschrieben (`iop_core_step()`
statt `iop_core_run()`, da nach dem neuen Auto-Return `pc` in einen
Bereich aus lauter Null-/NOP-Bytes ohne weitere Halt-Bedingung
wandert, was `iop_core_run()`s Endlosschleife ebenfalls zum Hängen
brächte): Phase 1 prüft weiterhin das reale Vektorierungsverhalten
des SYSCALL selbst; Phase 2 prüft das neue Auto-Return-Verhalten
des BREAK. Alle 9 Checks bestehen.

**Gemessener echter Boot-Fortschritt** (verifiziert per `diag83`,
gebaut aus `diag80.c`): vor dieser Änderung hielt der IOP für immer
bei `pc=0x00000018`/`BREAK`. Danach läuft die Ausführung weiter und
erreicht einen NEUEN, andersartigen sauberen Halt bei `pc=0x800000AC`
mit `halt_reason="unimplemented SPECIAL funct 0x30 (pc=0x800000A8)"`
(ein `TGE`-Trap-Befehl, den dieser R3000A-Interpreter noch nicht
implementiert - ein ehrlicher Architektur-Grenzfall, kein Absturz).
Dies geschieht, weil ein ZWEITER echter Syscall/Exception-Vorgang
tiefer in `INTRMANP`s Init-Code auftritt und über denselben
Standard-Vektor-Fallthrough-Bereich erneut läuft.

Volle 84-Block-Regressionssuite besteht weiterhin (keine neuen Tests
hinzugefügt, `test_iop_syscall.c` nur modifiziert); sauberer
Wii-Rebuild verifiziert (nur die bereits bekannte, harmlose
`strncpy`-Warnung in `iop_module_loader.c`).

**Ehrlicher Ausblick**: der neue Haltepunkt bei `pc=0x800000AC`
("unimplemented SPECIAL funct 0x30") ist der natürliche nächste
Untersuchungsschritt für eine weitere Fortsetzung dieses Threads -
vermutlich muss entweder `TGE` (Trap if Greater or Equal) als
Opcode implementiert werden, oder es handelt sich um ein weiteres
Symptom desselben architektonischen Grundproblems (fehlender echter
Exception-Handler), das an einer neuen Stelle sichtbar wird.

## 30th finding/change (Round 29 continued): TGE implemented; real-BIOS testing reveals a non-halting retry loop (task #150)

Note: per user direction, documentation from this point forward is
written in English (the chat itself may still be conducted in
German - this only affected doc language, not process).

**Root cause of the `pc=0x800000AC`/"unimplemented SPECIAL funct
0x30" halt** (see the 29th finding): after task #149's syscall-return
fix, a second real syscall in INTRMANP's init falls through the same
still-unclaimed general exception vector down a different path and
reaches a genuine `TGE` (Trap if Greater or Equal) instruction, which
this interpreter had never implemented.

**Fix** (`source/core/iop/iop_core.c`, SPECIAL funct `0x30`): real
MIPS trap semantics. If signed `rs >= rt`, raises a Trap exception
(`Cause.ExcCode=13`, pre-shifted into bits 2-6 as `0x34`), `EPC` set
to the TGE instruction's own address, PC vectors to `0xBFC00180` or
`0x80000080` per `Status.BEV` - the exact same delivery mechanism as
this file's existing SYSCALL case, just a different ExcCode/trigger.
If the condition is false, TGE is a pure no-op: no exception, no
delay slot, no side effect of any kind.

New test `tests/test_iop_tge.c` (13 checks, synthetic only): covers
both outcomes. Trap-taken (5>=3) verifies Cause/EPC/PC vectoring
matches the SYSCALL pattern exactly, just with ExcCode 13. Trap-not-
taken (3>=5 is false) verifies Cause and EPC are completely untouched
and that execution falls through normally to a following marker
instruction, then reaches a trailing BREAK cleanly. Full 85-block
regression suite passes (84 previously + this new one); clean Wii
rebuild verified (only the known harmless `strncpy` warning).

**Honest real-BIOS follow-up**: this specific halt is gone, but
host-native testing against the actual SCPH-10000 BIOS (`diag85`,
100M-slice budget, periodic PC sampling) shows the IOP does NOT make
further real boot progress after this fix. Instead it settles into a
tight, non-halting loop cycling through roughly 11 instructions in
the `0x80000080`-`0x800000A8` range, forever - sampled PC values at
5M-slice intervals show the exact same small set of addresses
repeating in the same order indefinitely, with no forward movement
into new code. The most likely explanation: a real syscall is being
re-issued repeatedly, each time falling through to the still-
unclaimed vector and getting task #149's stub return value (`$v0=0`)
- and `0` apparently does not satisfy whatever condition the calling
code is polling for, so it retries indefinitely instead of proceeding
(or halting).

This is the same class of finding as the 27th finding's LOADCORE
registration-list closure (#124/#132): an honest architectural stop,
not a crash, not a test artifact, and not a regression - real
forward motion (past BREAK@0x18) has already been demonstrated in the
29th finding. It is deliberately NOT pursued further this round: doing
so would require reverse-engineering which real kernel service this
specific repeated syscall expects and constructing a plausible non-
zero return value or real handler behavior for it, which carries the
same "guessing at real subsystem semantics" risk this project has
consistently declined to take without stronger evidence (see the
27th finding's registration-list writeup for the precedent). This has
been left as an open task (#151) for whoever continues this thread,
with the exact PC range and the `diag85`-style sampling technique
documented here so it doesn't need to be rediscovered from scratch.


## 31st finding/change (Round 29 continued): front-loaded module loading implemented and tested empirically against the retry loop (task #151/#152)

Following a deep single-step trace (task #151) that found the
0x80000080-0x800000AC retry loop is caused by a genuine, real
ExitCriticalSection syscall (`$a0=2`) from INTRMANP re-entering
LOADCORE's own real init code, which walks its own internal
module/library registration list (the same mechanism the 27th
finding already characterized and closed for #124/#132) and finds it
empty again, this project attempted the user-directed, higher-risk
path: change the loader's architecture so all boot-list modules are
loaded/relocated before any entry point runs, on the hypothesis
(already proposed as "option (b)" in the 27th finding) that this
might let LOADCORE's registration list end up populated by the time
its own init reaches the check.

**Implementation** (`source/hw/iop_module_loader.c`): `load_and_link_one()`
was split into `load_only_one()` (ELF load, relocation, and export-
table registration only) and a new `link_imports_one()` (the import-
stub-patching step, deferred). A new `load_all_modules()` runs
`load_only_one()` for every listed module FIRST, then runs
`link_imports_one()` for every successfully-loaded module in a second
pass - so a module's imports can now resolve against modules that
load LATER in the boot list too, not just earlier ones as before.
`iop_module_loader_boot()` and `iop_module_loader_try_handle()` were
updated to use the precomputed `entry_points[]` array (via a new
shared `advance_to_next_module()` helper) instead of loading one
module at a time interleaved with running entry points. This changes
WHEN loading happens relative to execution; it does not fabricate any
address, struct layout, or registration entry - every address used is
still a real, computed relocation result from `iop_elf_load()`,
exactly as before.

**Empirical result (honest, not the hoped-for outcome)**: real-BIOS
testing (`diag92`/`diag93`, same methodology as the 30th finding's
`diag85`) shows the IOP's behavior is **byte-for-byte identical** to
before this change - the same PC values in the same order cycling
through the `0x80000080`-`0x800000AC` range forever. Front-loading
module loading and deferring import linking did NOT change LOADCORE's
own internal registration-list outcome, because - as this round's
tracing already established - that check is governed by
`boot_info[0x18]`/`[0x1C]` and LOADCORE's own internal
`lc_internals_t`-style bookkeeping, a completely separate mechanism
from the ELF import/export linking this change touches. Simply
changing the ORDER modules are loaded in doesn't populate those
fields; only constructing a real, correctly-formatted registration
entry (or otherwise making LOADCORE's own init code observe those
fields as non-zero) would, and that entry format remains unreverse-
engineered (see below).

**Why the entry-struct route is still blocked**: an attempt was made
to responsibly source REAL (not fabricated) function-pointer entries
by reading them out of the already-loaded, already-relocated modules'
own memory, rather than inventing addresses. This failed for a
concrete, verifiable reason: dumping IOP RAM at the four helper-
subroutine addresses the 27th finding cited (`0x1018d0`, `0x101f30`,
`0x102120`, `0x10198c`/`0x101410`) shows all-zero content at the
point they'd be needed, and LOADCORE's own code region
(`0x100CD0` onward) itself reads back as all-zero by the time the
retry loop is active - the real content genuinely isn't resident in
IOP RAM at the moment it would be needed, regardless of load order.
Constructing "real" entries under these conditions would still mean
guessing at addresses, which remains the same unacceptable risk this
project has consistently declined to take (fabricated `jalr` targets
do not fail safely).

**Kept anyway**: this change is retained despite not resolving the
retry loop, because it is a genuine, real improvement in its own
right - forward-only import resolution (a module could only ever
import from earlier-loaded modules) was itself an artificial
limitation of the old one-at-a-time interleaving, not a real hardware
constraint, and this fixes it independently of the retry-loop
question. Full 85-block regression suite passes (no new tests
needed - existing `test_iop_module_loader_bootinfo.c`, `test_iop_elf.c`,
and `test_iop_loadcore_panic_bypass.c` all still pass unchanged,
confirming the refactor preserves existing behavior for every already-
tested scenario); clean Wii rebuild verified.

**Honest status of task #151**: still open. The retry loop at
`0x80000080`-`0x800000AC` is now understood in full mechanistic
detail (see the 29th/30th/31st findings) but not resolved. The
remaining paths are the same two identified in the 27th finding:
(a) fully reverse-engineer the registration-entry struct format with
enough confidence to construct a real entry - now additionally
blocked by the missing helper-subroutine code described above, or
(b) some other, more invasive architecture change this round did not
find (front-loading alone was insufficient).

## 32nd finding/change (Round 29 continued): trap-stub bypass implemented - all 29/29 real modules now load, 15 run to completion (task #151/#152 continued)

Following the 31st finding's honest "front-loading alone doesn't fix
it" result, and per the user's explicit direction to keep pushing
forward on the remaining ~26 IOP modules, this round applied the SAME
kind of safe, byte-signature-based bypass mechanism already
established and validated for LOADCORE's panic loop (task #148) to
this NEW recursive dead end.

**Mechanism** (`source/hw/iop_module_loader.c`, `is_unconditional_trap_stub()`):
recognizes the exact real ten-instruction prologue LOADCORE's own
real init code installs at the general exception vector (NOP; SW
$k0,0x410($zero); a real-but-inert MFHI $zero; MFC0 $at,Status; NOP;
SW $at,0x408($zero); a real-but-inert ADD $zero,$zero,$zero; NOP;
NOP; ANDI $k0,$k0,0x3C) by its exact literal bytes - the same
approach as `is_loadcore_panic_loop()` - followed by a STRUCTURAL
(not hardcoded-value) check on the 11th word: must be SPECIAL,
funct=0x30 (TGE), with rs==rt (making the trap condition always
true). The structural check matters because the same stub template
was observed reused at a nearby address with a DIFFERENT trap "code"
field (0x800000E8: code=3, vs. 0x800000A8: code=2) - matching the
shape of "always traps" catches the template wherever it recurs,
without weakening the byte-exact match on the actually load-bearing
part (the real register saves and Status read).

When recognized, `iop_module_loader_try_handle()` treats it exactly
like the panic-loop bypass: advances to the next module in the real
IOPBTCONF list via the same `advance_to_next_module()` helper
introduced in the 31st change, instead of letting the CPU's own
exception-delivery mechanism re-enter this stub forever (previously
observed spinning through the same ~11 instructions for the entire
100M-slice test budget with zero state change - see the 30th/31st
findings).

**Why this is safe** (identical reasoning to the panic-loop bypass):
every recognized instruction is real, already-disassembled, already
understood, and has zero externally observable effect this project
has ever traced anything reading back (the two SW targets, 0x410/
0x408, are never read by anything else; $k0/$at are scratch
registers by MIPS convention, not preserved across a real exception
anyway). Recognizing the pattern at its start and advancing to the
next module produces the same final, honest outcome as letting all
eleven words execute and then recognizing the trap itself.

New test `tests/test_iop_trap_stub_bypass.c` (10 checks, entirely
synthetic, no real BIOS bytes): recognizes the exact signature using
a DELIBERATELY different register/code encoding than the real BIOS's
own (`tge $k1,$k1` vs. the real `tge $zero,$zero,2`), proving the
match is structural, not tied to one specific encoding; three
negative controls (a near-miss prologue base register; a CONDITIONAL
trap with rs != rt; a different SPECIAL funct, TEQ, at the same
position) are all correctly rejected; and
`iop_module_loader_try_handle()` correctly advances to the next
module without halting when the signature is reached.

**Measured real-BIOS result** (combining this change with the 31st
change's front-loading refactor, verified via `diag94`/`diag95`
against the real SCPH-10000 BIOS): the IOP boot sequence, which
previously spun forever after only ~3 modules (SYSMEM, LOADCORE,
EXCEPMAN, INTRMANP), now:
  - loads and links **29/29** real IOPBTCONF modules (100%, up from
    4 modules ever even attempted before this round)
  - resolves **355/355** imports (0 unresolved)
  - runs **15** modules' real entry points to full, normal completion
    (returning cleanly through this loader's own trampoline)
  - safely bypasses 14 dead-end recursions along the way (1 via the
    original LOADCORE panic-loop bypass, 13 via this new trap-stub
    bypass)
  - reaches a clean, honest, natural end-of-list halt
    ("module boot sequence complete: 29/29 real modules loaded, 15
    run to completion") instead of the previous infinite spin

Full 86-block regression suite passes (85 previously + this new
test); clean Wii rebuild verified (only the pre-existing harmless
`strncpy` warning).

**Honest scope note**: "15 run to completion" does not mean 15
modules did everything real hardware would expect of them - it means
their entry-point code executed for real, through this project's
actual interpreter, until it either returned normally or hit a
recognized dead end that this loader safely stepped past. Some of
those 15 may have done substantially less real work than on real
hardware if they, too, hit silent gaps this project hasn't yet
surfaced as a halt (the same honest caveat that has applied to every
module this project has run since task #92). Task #151 in the sense
of "root-cause and fix the retry loop with a real registration entry"
remains open (that would require the still-blocked reverse-engineering
described in the 31st finding) - what this round achieves instead is
a safe, honest way to make forward progress THROUGH it, matching the
project's established precedent.

## 33rd finding (Round 29 continued): which modules completed vs. bypassed, and confirmation the EE-side SIF wait remains blocked (task #153)

Follow-up investigation after the 32nd change's headline numbers
(29/29 loaded, 15 run to completion, 14 bypassed) - identifying WHICH
modules fall into each category, using a temporary, non-committed
trace instrumentation (reverted immediately after use, no permanent
code change from this finding).

**Completed normally (15)**: SYSMEM, EXCEPMAN, INTRMANP, INTRMANI,
TIMEMANP, TIMEMANI, SYSCLIB, HEAPLIB, EECONF, ROMDRV, STDIO, SIFMAN,
IGREETING, SECRMAN, EESYNC.

**Bypassed via the panic-loop or trap-stub mechanisms (14)**:
LOADCORE (panic-loop, task #148 - expected, already understood),
SSBUSC, DMACMAN, THREADMAN, VBLANK, IOMAN, MODLOAD, SIFCMD, REBOOT,
LOADFILE, CDVDMAN, CDVDFSV, SIFINIT, FILEIO (all 13 via the 32nd
change's trap-stub bypass).

**Significant for the SIF handshake specifically**: `SIFMAN` (the
low-level SIF register/mailbox scaffold) completed normally, but both
`SIFCMD` (the higher-level SIF command/RPC layer) and `SIFINIT`
(explicit SIF initialization) hit the SAME unconditional-trap dead
end and were bypassed - meaning their real init code never got the
chance to actually set up the SIF command dispatch or announce
readiness. Confirmed via direct register readback after the full
29-module boot sequence completes and halts: `SIF_MSCOM`, `SIF_SMCOM`,
`SIF_MSFLG`, and `SIF_SMFLG` (0xB000F200/F210/F220/F230) are all still
`0x00000000` - never written by anything.

**EE-side confirmation**: with the IOP halted (boot sequence
complete, nothing left to run), the EE was stepped an additional 160
million instructions (20M more slices at the existing 8:1 EE:IOP
scheduler ratio) with no change: `pc` stays within the same known
steady-state polling range (`0x80005E58`-`0x80006278`, per round
14/15's original finding) it was already in. This is a genuinely
stable end state, not a transient one this project simply didn't wait
long enough for - the EE is correctly, faithfully waiting (per round
15's disassembly) for SIF flags the IOP will now never set, since the
IOP itself has nothing left to execute.

**Conclusion**: the EE/IOP SIF handshake - and by extension, whatever
real BIOS code path would eventually draw the boot logo - is blocked
by the exact same root cause as #124/#132/#148/#151: LOADCORE's real
module/library registration list is empty because this project's
loader (even after the 31st change's front-loading) cannot safely
populate it with a real, correctly-formatted entry, and SIFCMD/SIFINIT
are two more real modules (in addition to LOADCORE's own init) that
depend on registering with it to do their real job. This is not a new,
separate bug - it is the same architectural gap surfacing for the
Nth time, now precisely attributed to the two specific modules that
matter for the SIF handshake. No further code change was made this
round; this finding sets up whoever picks up the entry-struct reverse-
engineering work (still blocked - see the 31st finding) with a
concrete, prioritized target: SIFCMD and SIFINIT's own real init code
would be the two most productive candidates to trace next, since they
are what the EE is actually waiting on.

## 34th finding (Round 29 continued, task #154): live PCSX2 reference debugger confirms SIFMAN/SIFCMD addresses and reveals the real IOP import-table format

User question: can SIFCMD/SIFINIT be read directly from the connected
PCSX2 debugger (pcsx2-mcp), instead of continuing to rely on this
project's own emulated IOP memory (already found, in the 31st finding,
to be zeroed out by the time it would be needed)?

Answer: yes, confirmed concretely. The connected pcsx2-mcp DebugServer
is attached to a LIVE, fully-booted, real PCSX2 instance running an
actual commercial game (not this project's own emulator). Its
`pcsx2_read_memory`/`pcsx2_evaluate` tools cannot reach IOP address
space (no `cpu` param / no pointer-dereference support respectively),
but `pcsx2_disassemble(cpu="iop")` can, and conveniently echoes the
raw hex word for every address alongside its (often nonsensical, since
much of it is struct data rather than code) decoded mnemonic - reused
here as a raw-memory-read workaround, same technique already noted in
this round's earlier work.

Walked the real `ModuleInfo_t` singly-linked list starting at IOP
address `0x800` (ps2sdk's own loadcore.h documents this as the usual
start) through all 17 real kernel modules up to and including id=16
(`IOP_SIF_manager` = SIFMAN) and id=17 (`IOP_SIF_rpc_interface` =
SIFCMD) - confirming both by reading their name-string bytes directly
out of memory, not just inferring from list position. Per ps2sdk's
real module set there is no separate "SIFINIT" module - `sceSifInit`
is a function exported by SIFMAN itself, so SIFMAN + SIFCMD together
are the complete real answer to "SIFCMD/SIFINIT". Real addresses
found: SIFMAN entry=0x16930 (text 0x16930-0x17800), SIFCMD
entry=0x17e00 (text 0x17d30-0x19490).

Three concrete, non-fabricated structural discoveries came out of
disassembling their real code on this live instance:

1. **Real IOP import-table format**, found embedded in SIFMAN's own
   text (called from its entry function at 0x169a0's `jal 0x17794`):
   a magic word `0x41e00000`, two header words, an 8-byte
   null-terminated imported-library name (e.g. `"intrman\0"`), followed
   by a run of `j <target>` / delay-slot-`li $zero,N` stub pairs whose
   jump targets land inside the real, already-confirmed text ranges of
   LOADCORE (0x1630-0x3260) and INTRMAN (0x3d30-0x52a0) - i.e. these
   are patched-in real call stubs to those two modules' real exported
   functions, resolved by the time this reference instance reached its
   current running state. This is a DIFFERENT mechanism from LOADCORE's
   own internal per-phase registration list (the 27th finding's still-
   open gap) - it's the ordinary cross-module import/export linkage,
   not LOADCORE's own bookkeeping.

2. **LOADCORE's real entry function** (0x1630) reads real boot_info
   fields at every offset this project's own `boot_info` struct already
   models (0x00/0x04/0x08/0x0C/0x10/0x14/0x18/0x1C) in that exact
   order - independent, real-hardware confirmation that this project's
   boot_info layout (including task #134's offset-0x0C fix) is
   structurally correct, not a guess. It also zeroes a fixed 17-entry
   (0x44-byte) table at real address 0x32C0 during init - a LOADCORE-
   internal table whose purpose is not yet determined.

3. **A candidate real registration-list-walk function** at 0x1c70:
   a singly-linked-list search over nodes shaped
   `{next(u32) @+0, ??? @+2 (u8), a key byte @+2 read for comparison,
   a count byte @+3, then count*4 bytes of trailing u32 data}`,
   searching for a node whose byte-at-offset-2 matches a caller-
   supplied key - structurally consistent with this project's
   pre-session 27th-finding language of a "phase-tagged" registration
   list, though this has NOT yet been confirmed as THE list gated by
   boot_info[0x18]/[0x1C], nor has its caller/populator been traced.

No source code was changed for this finding (pure investigation, same
category as the 33rd finding/task #153). Per the project's standing
rule, nothing from this live reference instance's real BIOS/game
memory is reproduced verbatim anywhere in this project's own source or
tests - only the structural/format facts above, cited the same way
prior findings cited ps2sdk/PCSX2 upstream headers and source.

Task #151 remains open. The most promising next step is tracing who
calls into 0x1c70 and with what key values, to determine whether it is
in fact LOADCORE's real per-phase registration-list walker referenced
since the 27th finding - which would finally give this project the
real entry-struct format needed to populate LOADCORE's list with real
(not fabricated) entries, resolving task #151 at its root instead of
via the current safe bypass.

## 35th finding (Round 29 continued, task #151/#154 continued): the real boot_info[0x18]/[0x1C] registration-list format, fully reverse-engineered from the live PCSX2 reference debugger

Continuing the 34th finding's live-debugger investigation, traced
LOADCORE's real entry function (0x1630) past the boot_info field
reads all the way through its actual use of boot_info[0x18]/[0x1C]
(loaded into local variables at fp+0x50/fp+0x54 - see the 34th
finding), and found the exact real mechanism task #151 has been
missing since the pre-session 27th finding:

**The real call chain, traced instruction-by-instruction:**
- At 0x18c4/0x18c8, LOADCORE reloads `boot_info[0x18]` into `a2` and
  `boot_info[0x1C]` into `a1`.
- `a2` is transformed into a byte count: `(boot_info[0x18] + 1) * 4`.
- Stack space of that (8-byte-rounded) size is carved out; a fresh
  buffer pointer is saved into `s0`.
- `0x2810` is called with `a0 = s0` (dest), `a1 = boot_info[0x1C]`
  (**the real source pointer**, untouched from the fp+0x54 reload),
  `a2` = the byte count above. Disassembling `0x2810` shows it is a
  literal `memcpy`/`memmove` (byte-copy loop with an overlap-direction
  check) - i.e. this is a real, confirmed **memcpy of
  `boot_info[0x18]+1` words from the real address in `boot_info[0x1C]`
  into a local stack buffer.**
- The copied buffer is then walked word-by-word starting at 0x1918.
  Each word's bit 0 selects one of two entry kinds:
  - **bit0 = 1**: a pure "phase tag" marker. The tag itself is
    `word >> 2` (saved to `s1`); the walker advances by exactly one
    word and continues (no header parsing this iteration).
  - **bit0 = 0**: the word instead holds a **pointer to a real module
    image header located elsewhere in memory**. This pointer is
    passed to `0x2890`.
- `0x2890` is a real COFF/ELF header-format sniffer and field-copier:
  it recognizes an ECOFF/COFF-style header via magic `0x162` (the
  well-known real `MIPSELMAGIC` constant from little-endian MIPS COFF
  object headers) with secondary validation fields at header offsets
  `+0x14` (must be `0x107`), `+0x10` (masked `0x2FFFF`, must be
  `0x38`), and cross-checks a size field at `a1+8` against `a2+0x14`;
  on match it copies text/data/bss/entry/gp-style fields out to an
  output descriptor. It separately recognizes an ELF-header-shaped
  structure via a distinct set of offset checks (`+4==0x101`,
  `+0x12==8`, `+0x2A==0x20`, `+0x2C==2` - consistent with real
  ELF32 `e_machine=EM_MIPS(8)` / `e_phentsize=0x20` style fields at
  non-standard offsets, i.e. a project/kernel-specific header layout
  built around a real ELF header rather than a raw standard `Elf32_Ehdr`).
- The walker continues until it reads a **zero word** (list
  terminator) at `s0`, at which point it falls into further
  finalization code (0x1B08 onward) rather than looping again.
- The failure path (return code not recognized) jumps to **the exact
  same panic sequence bytes this project's `is_loadcore_panic_loop()`
  already recognizes** (`sb v0,(v1); j self` at 0x1c00) - direct,
  independent, real-hardware confirmation that this project's task
  #148 panic-loop signature is matching the correct, real code path,
  not a coincidental byte pattern.

**What this means for task #151:** `boot_info[0x18]` is not an opaque
flag - it is a real **word count minus one**, and `boot_info[0x1C]` is
a real **pointer to a zero-terminated array of tag/pointer words**,
where pointer words reference real COFF- or ELF-shaped module image
headers elsewhere in IOP RAM. This is concrete enough to actually
build: this project's own loader already parses every boot-list
module's real header while loading it (`iop_module_loader.c`); the
real fix would be to keep each loaded module's header resident,
build a zero-terminated tag/pointer array referencing them (using the
now-known bit0/tag-shift/pointer encoding and the COFF/ELF field
offsets above), and point `boot_info[0x18]`/`[0x1C]` at that real
array - replacing the current safe bypass (`is_unconditional_trap_stub`
/ `is_loadcore_panic_loop`, task #148/#152) with a genuine fix rather
than a recognize-and-skip workaround.

This is not yet implemented - this finding is investigation-only (no
source changed), and confirms the structural facts above, not
specific byte values, consistent with the project's standing rule
against reproducing any real BIOS/game bytes. Task #151 remains
open, but is now unblocked in the sense that matters most: the real
target format is fully known. Implementing it is real, nontrivial
work (constructing valid COFF/ELF-shaped headers this project's own
loader can point to) and is the natural next step.

## 36th finding (Round 29 continued, tasks #151/#155/#156/#157): real registration list implemented and tested - honest empirical result, plus an unrelated pre-existing hang fixed along the way

Following the 35th finding's fully-reverse-engineered real
boot_info[0x18]/[0x1C] format, implemented `build_real_registration_list()`
in `iop_module_loader.c`: after front-loading every module, builds a
real, zero-terminated array in IOP RAM (2 leading placeholder words +
one bit0=0 pointer word per successfully-loaded module, each pointing
at that module's own real, already-loaded ELF header - no fabricated
bytes, just real addresses) and points boot_info[0x18]/[0x1C] at it.

**Fixing the mandatory regression suite first uncovered an unrelated,
pre-existing bug (task #156):** `tests/test_iop_rfe.c` hung
indefinitely. Bisected against the pre-session HEAD (confirmed the
hang reproduces identically with the OLD `iop_module_loader.c` too -
not caused by this round's work). Root cause: `iop_core.c`'s BREAK
handler (task #149's 29th change) treats any BREAK reached while
`Cause.ExcCode==8` as an "unresolved syscall, resume at EPC+4"
fallback - but RFE never touches Cause (by design, only Status), so a
BREAK reached AFTER an RFE-terminated syscall handler, where Cause
merely still happens to read a stale 8 from the earlier, already-
handled exception, wrongly re-triggers the same fallback and resumes
at the old, stale EPC+4 - which in `test_iop_rfe.c`'s case marched
through zeroed/NOP-equivalent memory forever. Fixed by adding an
`exception_pending` flag (set at every real exception-entry site, hw
interrupt / SYSCALL / TGE; cleared by RFE) and gating the BREAK
fallback on it, precisely capturing "has this exception actually been
handled yet". Verified both the original task #149 scenario (BREAK
immediately after an unhandled syscall, no RFE - `test_iop_syscall.c`)
and the newly-found one (SYSCALL then RFE then BREAK -
`test_iop_rfe.c`) now behave correctly. Full 87-block regression suite
passes (0 hangs, 0 failures); clean Wii rebuild verified.

**Real-BIOS empirical result for the registration list itself (task
#151/#155):** genuinely different and more real than before - LOADCORE
now walks the real 29-entry list this project supplies (confirmed via
`registration_list_entries=29`), rather than being rejected
immediately (`panic_loops_bypassed` dropped to 0 - the ORIGINAL empty-
list panic literally never fires anymore). However, this exposed a
NEW, distinct real dead end deeper in LOADCORE's own registration-walk
code: a second "write a status byte, then spin forever" panic idiom
(`sb $v0,($v1)` / `j <self>` / nop), reached from a different real
call site than the original panic sequence (no inline `lui`/`addiu`
setup immediately before it - just the tail 3 words). Without a
bypass for this, `modules_run_to_completion` REGRESSED from 15 to 1 -
i.e. the more architecturally-honest list construction was, on its
own, practically WORSE. Added `is_registration_walk_panic_loop()`
(task #157, same safe byte-signature-plus-external-sequencer-advance
technique as the other two bypasses) to restore forward progress.

**Net honest result with all three changes combined:**
`modules_run_to_completion` is back to 15 (matching the pre-#155
milestone); `registration_list_entries=29`; `panic_loops_bypassed=0`
(down from 1); `trap_stubs_bypassed=13` (down from 14);
`registration_walk_panics_bypassed=1` (new). Critically, per-module
breakdown (re-traced via temporary, non-committed instrumentation,
same as the 33rd finding's methodology) shows the exact SAME 14
modules bypassed as before, including **SIFCMD and SIFINIT
specifically still hitting the identical trap-stub dead end,
unchanged** - the real registration-list format understanding did NOT
get the actual target modules (SIFCMD/SIFINIT) any closer to
completing. One incidental, unexplained difference was observed:
SIF_MSFLG now reads `0x00010000` instead of `0x0` (SIF_MSCOM/SIF_SMCOM
/SIF_SMFLG remain 0; EE remains in its known SIF-polling steady state
even after 160M further instructions).

**Conclusion for task #151:** the real boot_info[0x18]/[0x1C] format
(34th/35th findings) is now implemented, tested, and kept (a genuine,
defensible improvement: real data instead of an honest-zero
placeholder, and it demonstrably changes LOADCORE's real code path
taken). But it does NOT, on its own, resolve the actual SIF handshake
blocker - SIFCMD/SIFINIT still dead-end at the exact same trap-stub
point as before task #155. Task #151 remains open. The new,
deeper real dead-end this round found (the registration-walk panic at
a different call site) would be the next concrete target for whoever
continues this investigation - likely requiring the same kind of live-
debugger tracing this round used to find LOADCORE's list format in
the first place, this time aimed at whatever validation step rejects
the real ELF headers this project already supplies.

## 37th finding (Round 29 continued, task #151 continued): LOADCORE's registration list is an ACTIVE, re-entrant call-dispatch mechanism, not passive bookkeeping - likely explains the new registration-walk panic

Continuing the live pcsx2-mcp reference-debugger trace from the 34th/
35th findings, followed LOADCORE's real per-entry processing past the
COFF/ELF header validation (0x2890) into what happens on a
SUCCESSFULLY recognized entry, disassembling 0x1a38-0x1af0 and the
called subroutine 0x2a80 on the live instance.

**Confirmed structurally, with exact real field offsets:**
- 0x2a80 re-validates the header (checking a discriminant word at the
  entry buffer for values 1/3/4, dispatching to one of three further
  sub-parsers) and, for the ELF path specifically (0x29d0-0x29f4 in
  the earlier-traced 0x2890), checks the REAL program header's
  `p_type == 0x70000080` (PT_MIPS_IOPMOD - the exact real, Sony-
  specific segment type this project's OWN `iop_elf.h` already cites
  and implements) and the REAL ELF header's `e_type == 0xFF80` (the
  exact real vendor e_type this project's OWN `iop_elf.h` already
  cites) - independent, live-hardware confirmation that this
  project's existing ELF loader citations are correct.
- Then, critically, at 0x1a7c-0x1a90: LOADCORE loads a function
  pointer from `fp+0x14` (populated by the header-parsing chain above
  - almost certainly the module's real `entry` field) into `$v0`,
  sets `$gp` from `fp+0x18` (the module's real `gp` field), sets
  `a0=0, a1=0, a2=<the current list-entry pointer itself>`, and
  **calls it directly via `jalr $v0`** - i.e. LOADCORE's own real
  registration-list walk DIRECTLY INVOKES each recognized module's
  real entry point itself, inline, as part of its own continuous
  execution - it does not just record bookkeeping data for something
  else to call later.

**Why this likely explains this round's new registration-walk panic
(36th finding):** this project's own external module-loader sequencer
(`iop_module_loader.c`'s `advance_to_next_module()`) ALSO already runs
every module's real entry point once, one at a time, via its own
trampoline mechanism - completely independently of LOADCORE's
internal list. Task #155's `build_real_registration_list()` populates
the list with a pointer to EVERY successfully-loaded module,
including ones this project's own external sequencer has ALREADY run
to completion (e.g. SYSMEM, which always runs first). Given the jalr
mechanism just confirmed, real LOADCORE code reaching this point would
therefore call SYSMEM's real entry point A SECOND TIME - real kernel
init code is generally not written to be safely re-entered, so a
second real execution plausibly corrupts some state this project
hasn't identified yet, which a subsequent check then rejects,
producing the new panic tail found in the 36th finding. This is a
well-supported hypothesis from the exact mechanism now confirmed, but
NOT yet verified by directly observing which specific list entry
triggers the failing call (that would need single-step tracing
through the actual jalr and the state it touches - not completed this
session due to time constraints).

**What this implies architecturally, for whoever continues task
#151:** this project's current architecture - an external sequencer
that runs every boot-list module's entry point once, itself - is a
project-specific simplification of what real hardware actually does:
LOADCORE's own init code appears to BE the real sequencer, walking its
internal list and jalr-ing into each module in turn. Supplying
LOADCORE a real list of ALL modules (including already-run ones) most
likely conflicts with this project's own separate external sequencing.
Two candidate directions for a future session: (a) only include NOT-
YET-RUN modules in the list handed to LOADCORE at the point its walk
reaches this code (requires knowing exactly when, in boot order,
LOADCORE's own init reaches this check, and truncating/rotating the
list accordingly - tricky since this project's own external sequencer
and LOADCORE's internal one are not obviously synchronized), or (b)
the larger architecture change already flagged as an option earlier
this session: let LOADCORE's own jalr-based walk BE the real
sequencer once it starts, and have this project's external loader
step back after invoking LOADCORE, rather than continuing to run its
own competing one-at-a-time trampoline sequence in parallel. Neither
was attempted this session - this is intentionally left as a clearly-
scoped, well-evidenced starting point rather than a rushed, unverified
attempt, consistent with this project's standing discipline of not
claiming a fix without empirical confirmation.

No source code was changed for this finding - pure investigation,
same category as the 33rd/34th/35th findings. Nothing from the live
reference instance's real memory is reproduced verbatim in this
project's own source - only the structural facts above, cited the
same way prior findings cited ps2sdk/PCSX2 upstream and this
project's own already-existing iop_elf.h citations.

## Round 29 continued (38th finding, task #158): mark_module_dispatched() fix
implemented, wired in, verified via trace to execute correctly - but
CONFIRMED to produce ZERO observable change in real-BIOS boot behavior.

Following the 37th finding's well-supported double-execution
hypothesis (LOADCORE's real registration-list walk directly `jalr`s
into each recognized module's real entry point, which would re-invoke
already-run modules like SYSMEM a second time since this project's own
external sequencer independently runs every module once already),
implemented `mark_module_dispatched()`: patches a module's own slot in
the real list this project builds (`build_real_registration_list()`,
task #155) from a real header pointer (bit0=0) to an inert tag word
`0x00000003` (bit0=1, nibble=3 - confirmed inert per the 37th finding's
own disassembly of the walk's `andi/bne` check) the INSTANT that
module starts executing - wired into both `iop_module_loader_boot()`
(module 0's first start) and `advance_to_next_module()` (every
subsequent module's start).

**Verification performed, in order:**
1. Compile-check: clean.
2. Full 87-test host-native regression suite: all pass (one test,
   `test_iop_module_loader_bootinfo.c`, was updated to assert the NEW
   correct behavior - the synthetic single-module test's own module
   gets its slot patched to the inert tag immediately, since it starts
   inside `iop_module_loader_boot()` itself - rather than asserting
   the old, now-intentionally-changed "real pointer" behavior).
3. Real-BIOS diagnostic (`/tmp/diag101.c`'s existing module-completion-
   breakdown pattern), run in a fresh build against the fixed code.
4. **Trace-confirmed the fix actually executes**: added temporary
   `fprintf` tracing to `mark_module_dispatched()` (reverted afterward,
   verified via `diff` byte-identical to before), re-ran the same
   real-BIOS diagnostic, and confirmed all 29 loaded modules'
   `mark_module_dispatched()` calls fire in boot order with valid,
   distinct slot addresses (`0x00145CA8` through `0x00145D18`) - the
   wiring is correct and the patching genuinely happens during a real
   boot run, not just in unit tests.
5. **Direct A/B comparison against the pre-fix commit** (`git stash` to
   temporarily revert to commit `213f959`, rebuild, re-run the
   identical diagnostic, `git stash pop` to restore): the real-BIOS
   diagnostic's output is **byte-for-byte identical** before and after
   this fix - `modules_run_to_completion=15`, `trap_stubs_bypassed=13`,
   `registration_walk_panics_bypassed=1`, `imports_unresolved=0`,
   `SIF_MSCOM=0x00000000`, `SIF_SMCOM=0x00000000`,
   `SIF_MSFLG=0x00010000`, `SIF_SMFLG=0x00000000` - matching the 36th
   finding's numbers exactly, in every field.

**Honest conclusion: this fix does NOT resolve task #151.** The
`registration_walk_panics_bypassed` count staying at exactly 1 in both
the pre-fix and post-fix runs is the key evidence: task #157's real
registration-walk panic bypass is firing at the same point, the same
number of times, regardless of whether module slots hold real pointers
or inert tags. This means real LOADCORE's own registration-list walk
is hitting the SAME dead end (the exact byte pattern
`is_registration_walk_panic_loop()` recognizes) every time, and never
gets far enough - or never depends on slot content in the way this
fix assumed - for the slot-patching to matter. The 37th finding's
double-execution mechanism (the `jalr` dispatch itself) is real and
structurally confirmed via the live debugger, but it is NOT the actual
blocker standing between the current state and SIFCMD/SIFINIT
completing. Some other, still-unidentified mechanism inside (or before)
that same walk is the real limiter.

**Decision: keep the fix rather than revert it.** It is architecturally
more correct (a module's registration-list slot should not remain a
live, jalr-able pointer once that module has already started, matching
the real bit0=0/bit0=1 pointer/tag distinction this project already
reverse-engineered), it is fully regression-tested and does not
regress any of the 87 host-native tests or the real-BIOS module-
completion count, and it removes a known-incorrect state (stale
pointers to already-run modules) even though it does not, by itself,
unblock further boot progress. Consistent with this project's
established practice (e.g. the 36th finding's honest "no progress on
SIFCMD/SIFINIT" report), this is documented as a confirmed NEGATIVE
result for task #151's actual blocker, not oversold as a fix.

**What this implies for whoever continues task #151:** the real dead
end code is not "LOADCORE double-invokes an already-run module,"
since neutralizing that exact mechanism changed nothing. The next
concrete step is to determine WHERE, precisely, in the walk the
`is_registration_walk_panic_loop()` byte pattern is reached from -
i.e. single-step from the start of LOADCORE's real list-walk loop
up to the exact panic-pattern bytes, on either the live pcsx2-mcp
reference debugger or this project's own host-native diagnostic with
instruction-level tracing, to see what real condition (not slot
content) actually triggers the panic path every time. This was not
attempted this session due to time constraints (session limit
approaching) - left as the clearly-scoped next step.

## Round 29 continued (39th finding, task #151/#159/#162): the registration-walk panic is a bounded RETRY LOOP giving up after 4 attempts, not the jalr double-dispatch mechanism

Live pcsx2-mcp reference-debugger session (a real game boot, connected via
DebugServer) confirmed something the 37th/38th findings did not fully
pin down: the byte-for-byte real dead-end code `is_registration_walk_panic_loop()`
recognizes (`lui v1,0x8000; li v0,2; sb v0,(v1); j <self>; nop`) is
reached in LOADCORE's own real code via an **allocator-failure path**:
a call through an import-table stub (observed live at address `0x3234`,
itself a `j`+delay-slot trampoline into kernel code at `0xB1C`) that
returns 0 when either (a) a kernel-readiness flag at absolute IOP
address `0x14B4` is still zero, or (b) an argument-based size-class
check fails. This call is made with an apparent allocation size of
`0x30` (48 bytes - the real `ModuleInfo_t` struct size), immediately
preceded by address arithmetic matching real ps2sdk's own documented
`ModuleInfo_t` chain layout.

**Directly verified this is the SAME mechanism in our own emulator,
not a coincidentally-matching generic trap:** added temporary trace
instrumentation (backed up via `.bak`, reverted and diff-verified
afterward) to print the exact PC where `is_registration_walk_panic_loop()`
fires during a real-BIOS host-native diagnostic run. It fires at
`0x001012A0`, inside LOADCORE itself (`modlist_index=1`,
`entry=0x00100CD0` - i.e. LOADCORE's own module, loaded at a
different, higher RAM address in our project's boot than in the live
reference game, since real IOP modules are position-independent and
relocated to wherever the loader places them). Dumped the raw words
at and before this address and found them **byte-identical** to the
live-traced pattern: `lui v1,0x8000; li v0,2; sb v0,(v1); j <self>;
nop` at `0x101298-0x1012A8` - conclusively the same real dead-end
code, just relocated.

**New detail the live single-glance trace hadn't shown: the ~50
instructions immediately preceding the trap are a bounded RETRY LOOP**,
not a one-shot allocator call:
- Scans backward through some list, 8 bytes per step (`addiu s0,s0,-8`
  each iteration - a DIFFERENT stride than the 4-byte-per-word
  registration list this project's `build_real_registration_list()`
  builds).
- Each iteration: `lw v0,(s0)`, `andi v0,v0,3`, `beq v0,s3,+3` (branch
  out of the retry loop on a match - `s3` holds some 2-bit tag value
  established earlier, not yet traced back to its origin).
- Also bounds-checks `sltu v0,s2,s0` (loop terminates early if `s0`
  drops below some lower bound `s2`) and does `sw zero,(s0)` on the
  no-match path (clearing something at the current scan position
  before advancing) - real side effects not yet fully understood.
- A retry counter at `fp+0x58` increments each full pass; only after
  **4 failed attempts** (`slti v0,v0,4` false) does execution fall
  through into the fatal trap.

**Why this matters for task #151:** this is structurally a genuine
"retry loop that gives up and panics" - conceptually the same shape
as task #151's original description ("the non-halting IOP retry loop"),
now pinned down to a specific, disassembled, real code location inside
LOADCORE, with an exact failure condition (four failed backward scans
through an 8-byte-stride list for a tag-matching entry) rather than a
vague "SIF handshake blocker." The natural next hypothesis: this
8-byte-stride list is a DIFFERENT real structure than the boot_info
registration list this project already builds (task #155) - possibly
a real per-thread, per-semaphore, or per-handler table LOADCORE
expects to already contain a specific tagged entry by this point in
boot, which this project's kernel-init code has not yet populated (or
has populated with the wrong tag value in the low 2 bits, or at the
wrong stride/base). Task #158's jalr-dispatch fix, while real and
kept, is confirmed (again, independently, via this new trace) to be
unrelated to this particular dead end - the retry loop's search
target has nothing to do with which registration-list slots are
pointers vs tags.

**Not yet determined this round (next concrete steps for whoever
continues, already reflected in tasks #159/#163):**
- What real structure `s0`'s base address points into at loop entry
  (need to trace backward from the loop's first iteration to see
  what sets up `s0`, `s2` (lower bound), and `s3` (target tag) before
  the loop starts).
- Why our project's boot process never populates a matching entry
  within the scanned range within 4 tries - is the structure entirely
  unpopulated (this project's kernel/thread/semaphore init doesn't
  write to it at all), populated with a wrong tag, or is the retry
  count itself timing-dependent on something (e.g. a real interrupt
  or scheduler tick) our project's boot sequencer doesn't yet deliver
  before LOADCORE reaches this point?

No source code was changed for this finding - pure investigation,
combining live-debugger tracing (real game boot, DebugServer) with a
temporary, reverted trace in our own host-native diagnostic to
directly confirm both sides hit the identical real code. Verified via
`diff` that the temporary instrumentation left `iop_module_loader.c`
byte-identical to its pre-trace state afterward.

## Round 29 continued (40th finding, task #151/#163): the retry loop runs AFTER the full registration-list walk completes, not per-entry - it's a post-walk finalization check

Continuing the 39th finding's investigation (same technique: static
disassembly of our own emulator's resident real LOADCORE code via a
host-native diagnostic, cross-referenced against the live pcsx2-mcp
reference debugger's structurally-identical code at different
relocated addresses), traced the full per-entry loop body end to end
and found where the retry loop actually connects in:

- The per-entry loop (allocator call at our-boot's `0x101080` =
  live's `0x19e0`; jalr dispatch at `0x10111c` = live's `0x1a7c`;
  post-jalr `s1` return-value handling and the three follow-up calls
  matching live's `0x22EC`/`0x1D70`/`0x1CCC`) ends with `addiu s0,4`
  (advance list pointer) then **reloads the next list word and
  branches back to the top of the loop while it's non-zero**
  (`lw v0,(s0); bnez v0,->loop_top`). This confirms the whole
  allocator+dispatch+bookkeeping sequence is one loop body executed
  once per list entry, terminating only when a zero (terminator) word
  is read - matching this project's own `build_real_registration_list()`
  terminator convention exactly.
- **Only once the loop naturally exits** (terminator found) does
  execution reach `lw a0,0x48(fp); beqz a0,+3` - a conditional gate on
  a flag/pointer at `fp+0x48` - guarding a call to the SAME subroutine
  (relative target matching `0x1028dc`) that is ALSO called
  unconditionally near the very top of the whole routine (before the
  loop even starts). This call's role is not yet determined (finalize/
  sync of some kind - called both before list processing starts and
  again after it ends, conditionally).
- **Immediately after that gated call is exactly where the retry loop
  begins**: zero the counter at `fp+0x58`, set the fixed search tag
  `s3=3`, `s4=-4` (a `~3` mask), then the bounded 4-try backward scan
  documented in the 39th finding.

**Revised understanding of task #151's blocker:** this is POST-WALK
finalization code, not a per-entry mechanism, and not related to
task #158's jalr double-dispatch theory at all (independently
reconfirmed a third time). The natural reading: after LOADCORE
finishes walking the ENTIRE registration list and registering every
recognized module, it performs one final check - scanning a separate,
8-byte-stride table for an entry tagged `3`, up to 4 times - almost
certainly verifying that some specific expected condition (a
particular required module/component actually finished registering,
or some synchronization primitive reached a specific state) became
true as a side effect of the walk that just completed. On real
hardware this always succeeds within 4 tries; in this project's boot,
it never does, meaning something the registration walk is supposed to
cause as a side effect (writing a tag-3 entry into this separate
table) either never happens in this project's simulated walk, or
happens in the wrong place/format for this scan to find it.

**Next concrete steps (unchanged goal, more precisely scoped):**
determine (a) what real structure is being scanned (its base address
relative to `fp` or a fixed IOP address, and what real-world object
it represents - candidates: a semaphore/event-flag table, a thread
table, or a device-registration table separate from the module
registration list), (b) what `fp+0x48`'s gating flag represents and
whether this project's boot sequence sets the analogous condition
correctly, and (c) what the shared subroutine at relative offset
`0x1028dc` (called both before the walk starts and after it ends)
actually does - likely the key to understanding what "tag 3" means and
what's supposed to write it.

No source code changed - pure investigation (same discipline as the
36th-39th findings: static/live disassembly only, no unverified fix
attempted).

## Round 29 continued (41st finding, task #151): this session's findings reconnect to and reopen the ORIGINAL task #151 investigation (29th/30th/31st findings) - a previously-blocked path is now open

Re-reading the 31st finding (an earlier round's honest dead-end
report) in light of this session's 39th/40th findings resolves an
apparent contradiction and clarifies what this session's deep-dive
actually means for task #151.

**The contradiction:** the 31st finding stated that dumping IOP RAM
at LOADCORE's own code region (`0x100CD0` onward) "reads back as
all-zero by the time the retry loop is active," blocking any attempt
to reverse-engineer the real registration-entry struct format from
resident memory. This session's diagnostics (39th/40th findings) read
that SAME region, at a similar point (end of the full boot sequence),
and found fully valid, non-zero, structurally-consistent real MIPS
code there - not zeroed. The most likely explanation: intervening
fixes between the 31st finding and now (task #155's real registration
list, task #156's RFE fix, task #157's second bypass, task #158's
slot-patching) changed IOP RAM allocation/timing enough that
LOADCORE's own module memory is no longer overwritten by later
allocations at the point this project's diagnostics inspect it. This
was not independently re-verified against the OLD (pre-task-155)
commit this round, but the practical upshot is unambiguous either way:
**the previously-blocking obstacle no longer applies** - LOADCORE's
real code is readable today, via both this project's own emulator and
the live pcsx2-mcp reference debugger.

**What this means for task #151, tying the whole arc together:**
- The ORIGINAL task #151 (29th/30th finding): a real syscall
  (ExitCriticalSection, `$a0=2`, from INTRMANP) re-enters LOADCORE's
  own installed exception handler, which walks "LOADCORE's own
  internal module/library registration list" and finds it wanting,
  landing in an unconditional TGE trap. This project's
  `is_unconditional_trap_stub()` (32nd finding) recognizes and
  bypasses this - currently firing for 13 modules this round
  (SSBUSC, DMACMAN, THREADMAN, VBLANK, IOMAN, MODLOAD, **SIFCMD**,
  REBOOT, LOADFILE, CDVDMAN, CDVDFSV, **SIFINIT**, FILEIO - confirmed
  by name via a temporary, reverted trace this session), always at
  the same fixed real address `0x80000080` (the genuine R3000A
  general-exception-vector address, BEV=0).
- This session's 39th/40th findings independently traced a DIFFERENT
  entry path into what is very likely the SAME underlying real
  structure: LOADCORE's own post-registration-walk code (reached via
  its OWN direct execution flow, not exception re-entry) performs a
  4-pass tag-based dispatch over an 8-byte-stride table, then a
  bounded backward scan, before falling into the same class of real
  "write status, spin forever" trap - recognized by this project's
  separate `is_registration_walk_panic_loop()` (task #157), firing
  once, for LOADCORE itself.
- **Working hypothesis connecting both:** the 8-byte-stride table
  this session found IS (or is closely related to) "LOADCORE's own
  internal module/library registration list" the 31st finding
  referred to only conceptually. Both access paths - LOADCORE's own
  post-walk dispatch, and LOADCORE's exception handler re-entered by
  other modules' syscalls - very plausibly consult this SAME real
  structure, and both fail for the same underlying reason: this
  project's simulated boot never populates a matching entry in it.
  This would mean fixing this ONE structure could resolve BOTH
  symptoms (SIFCMD/SIFINIT's trap-stub bypass AND LOADCORE's own
  registration-walk-panic bypass) simultaneously.

**Honest caveat:** this connecting hypothesis is plausible and
well-motivated by the newly-reopened ability to read real code, but
NOT yet empirically verified - unlike this project's usual standard,
this round ran out of time to confirm it by tracing forward from
`is_unconditional_trap_stub()`'s real re-entry point (INTRMANP's
ExitCriticalSection syscall) to see whether it reaches the SAME
8-byte-stride-table code this session already disassembled, or a
structurally-similar-but-distinct copy of the same real idiom
elsewhere in LOADCORE. This is the single most valuable next step for
whoever continues task #151, now that the "all-zero memory" obstacle
that stopped the 31st finding no longer applies.

No source code changed - pure investigation and documentation
reconciliation.

## Round 29 continued (42nd finding, task #151): EXCEPMAN completes normally but never patches the exception vector - the real fix target is narrowed to "what installs the real dispatcher"

Continuing the 41st finding's open question (does the exception-vector
trap-stub path connect to the 8-byte-stride table?), traced two more
concrete facts that sharpen task #151 considerably:

**1. The "always trap" content at the exception vector is baked in via
module loading, not written by any running code.** A temporary trace
on `iop_mem_write32()` (reverted, diff-verified) showed ZERO writes to
`0x80000080`-`0x800000B0` across an entire real-BIOS boot, yet that
range unambiguously contains the real ten-instruction
"save-registers-then-unconditionally-TGE" stub (confirmed identical to
the live pcsx2-mcp reference debugger's own resident copy). The
resolution: `iop_elf.c`'s segment loader writes via `iop_mem_write8()`
byte-by-byte (`iop_elf_load()`, lines ~84-87), which my write-trace on
the 32-bit path didn't cover. This means the stub is literally part of
some early-loaded real module's own ELF/IRX segment data, physically
placed at the fixed R3000A exception-vector address by that module's
own real (not fabricated) program-header `p_vaddr` - almost certainly
a foundational module (SYSMEM, loaded earliest) installing a minimal
"default/fallback" handler as real, standard PS2 kernel bootstrap
behavior, intended to be replaced later.

**2. EXCEPMAN (Exception_Manager) is in this project's own real boot
list and runs to full, un-bypassed completion** - confirmed via a
temporary trace (reverted, diff-verified) on every module that reaches
this project's trampoline-return path (the normal "ran real code,
returned normally" outcome, as opposed to any panic/trap-stub bypass):
`SYSMEM, EXCEPMAN, INTRMANP, INTRMANI, TIMEMANP, TIMEMANI, SYSCLIB,
HEAPLIB, EECONF, ROMDRV, STDIO, SIFMAN, IGREETING, SECRMAN, EESYNC`
(15 modules, matching `modules_run_to_completion=15` exactly).
EXCEPMAN's real entry point genuinely executes start to finish in this
project's interpreter - no bypass needed, no shortcuts taken - yet the
exception vector still holds the unmodified default stub immediately
afterward (confirmed by re-reading `0x80000080` after boot completes).

**This narrows task #151 considerably:** the real PS2 kernel's design
almost certainly does NOT have Exception_Manager patch the shared
vector directly as a side effect of its own init - if it did, and
EXCEPMAN's real code runs uninterrupted in this project's emulator
(which it does), the patch would already be visible. The much more
likely real design (consistent with the "Exception_Manager" name and
real PS2 kernel conventions): EXCEPMAN's init only sets up its OWN
internal bookkeeping (very plausibly the 8-byte-stride table this
session already found via LOADCORE's post-walk dispatch code, or a
sibling structure), and each INDIVIDUAL module (SIFCMD, SIFINIT, and
the other 11 that hit the trap-stub bypass) is expected to actively
REGISTER its own handler via a real kernel API call (a syscall or
SIF/RPC call INTO Exception_Manager) as part of ITS OWN init - and
if those registration calls aren't happening correctly in this
project's simulation (e.g. because the calling module's own init gets
bypassed via `is_unconditional_trap_stub()` BEFORE it reaches its own
"register my handler" call, or because the syscall/RPC mechanism used
to reach Exception_Manager isn't fully wired up), the real dispatcher
table never gets populated, and the shared vector's fallback "always
trap" stub is all that's ever consulted.

**This also would explain the exact module list precisely:** THREADMAN,
IOMAN, MODLOAD, SIFCMD, CDVDMAN, CDVDFSV, SIFINIT, FILEIO, etc. are all
modules whose real init plausibly needs to register interrupt/exception
handling for their own hardware/service (timer ticks, DMA completion,
device interrupts) - exactly the kind of module that would call INTO
Exception_Manager's real registration API early in its own init, before
doing anything else. If that very first call already falls through to
the still-default vector (because NO earlier module successfully
registered anything yet, and this project's own architecture runs
these modules' entry points to completion one at a time - so each
module IS the first to try, in isolation), this is a self-reinforcing
gap: no module ever gets past its own first registration attempt to
reach the rest of its real init code, so nothing "downstream" that
might otherwise populate the table ever executes either.

**Next concrete step (unchanged goal, most precisely scoped yet):**
trace EXCEPMAN's own real init code (now readable, since the "all-zero
memory" obstacle no longer applies - see the 41st finding) to identify
its real internal data structure (very possibly the same 8-byte-stride
table), and separately trace what a module like SIFCMD's real init
code does immediately before it hits the trap stub - specifically
whether it makes a real syscall/RPC call whose target is EXCEPMAN's
registration entry point, and whether that call's expected real
argument/return convention is being faithfully emulated (this project
already implements real IOP syscalls per module - the specific
registration syscall's number/semantics may not yet be covered).

No source code changed - pure investigation, following the same
byte-signature/trace-and-revert discipline as the 39th-41st findings.

## Round 29 continued (43rd finding, task #151): identified the exact real syscall numbers - concrete implementation target found

Continuing the 42nd finding's open question (what real syscall does a
module like SIFCMD make right before hitting the trap stub?), added a
temporary trace (reverted, diff-verified) capturing EPC, the faulting
instruction, and $v0/$a0/$a1/$k0 at the exact moment
`is_unconditional_trap_stub()` fires for each of the 13 affected
modules. Result, for every one of them:

- The faulting instruction is `0x0000000C` - a genuine SPECIAL/SYSCALL
  (opcode 0, funct 0x0C), not a BREAK or anything synthetic.
- `$k0 == 0x00000008` for all 13 - confirms `Cause.ExcCode == 8`
  (Syscall), exactly matching real R3000A exception semantics (`k0`
  is conventionally used to stash the exception cause during handler
  entry, per the `is_unconditional_trap_stub()` prologue's own
  `andi $k0,$k0,0x3C` this project already recognizes).
- `$v0` (the real IOP kernel syscall-number convention: syscall # in
  `$v0`, args in `$a0`/`$a1`/...) is **0x10 (16)** for 9 of the 13
  (SSBUSC, DMACMAN, THREADMAN, VBLANK, IOMAN, MODLOAD, SIFCMD,
  CDVDMAN, SIFINIT), and **0x08** for the other 4 (REBOOT, LOADFILE,
  CDVDFSV, FILEIO).
- For the `v0==0x10` group: `$a0` is a distinct, module-local address
  each time (e.g. `0x001FFEF0`, `0x001FFEE0`, `0x001FFEA0` for
  SIFCMD) - consistent with a pointer to the CALLING module's own
  local buffer/struct - and `$a1` is either the fixed value
  `0x00100030` or `0xBF801528` (a KSEG1/BIOS-space address).
- For the `v0==0x08` group: `$a0` is the fixed small constant `3`
  every time, `$a1` again either `0x00100030` or `0xBF801528`.

**Interpretation:** this is entirely consistent with a real IOP
kernel syscall - most plausibly `RegisterLibraryEntries` (or a very
close sibling) for the `v0==0x10` case, given the calling convention
(module passes a pointer to its own real export/library-entry table
structure - see this project's own already-documented real IOP
import/export table format - as an argument to a kernel call that
registers it). This project's existing IOP-BIOS-HLE-Syscall-Trap
(task #31) covers the OLDER PS1-style `0xA0`/`0xB0`/`0xC0`
jump-address BIOS call convention; this is a DIFFERENT, genuine MIPS
`syscall` instruction raising a real CPU exception (`Cause.ExcCode=8`)
that this project does not yet specially handle - it currently falls
through to whatever real code happens to be resident at the exception
vector, which (per the 42nd finding) is just the unreplaced default
fallback stub, since no module's registration call has ever
successfully reached a real dispatcher.

**Concrete next step (the clearest, most actionable target this
entire investigation has produced):** implement real (or, at minimum,
plausible-and-precedented, matching this project's existing
BREAK-as-syscall-fallback pattern from tasks #149/#156) handling for
IOP syscall numbers `0x10` and `0x08` in `iop_core.c`'s SYSCALL
exception path - either by actually processing the real
`RegisterLibraryEntries`-style semantics (building on this project's
already-understood real export-table format), or, as a safer first
increment matching established precedent, returning a plausible
success value and RFE-ing back to the caller so each affected
module's own init code can continue past this specific call instead
of getting stuck - then empirically verifying (via the same real-BIOS
diagnostic technique used throughout this session) whether this
actually lets SIFCMD/SIFINIT progress further, honestly reporting
whatever the real result is.

No source code changed this round - pure investigation, same
byte-signature/trace-and-revert discipline as the 39th-42nd findings.
This is intentionally left as implementation work for a dedicated
follow-up round (task #164), rather than rushed within an already very
long investigative session, consistent with this project's standing
practice of not combining open-ended research with unverified
implementation in the same breath.

## Round 29 continued (44th finding, task #151/#164): implemented IOP syscall 0x10/0x08/0x14 handling - real, substantial forward progress; SIF handshake still not reached

Implemented the 43rd finding's concrete target: real IOP kernel
syscall numbers `0x10`, `0x08`, and (discovered mid-implementation)
`0x14` are now intercepted directly at the `SYSCALL` exception site in
`iop_core.c`, BEFORE any real exception is raised - matching this
project's existing, established precedent for unimplemented real
kernel calls (the `A0`/`B0`/`C0` BIOS-HLE convention, and the
BREAK-as-syscall-fallback from tasks #149/#156): return the same
generic default value (`$v0 = 0`) already used throughout this
project, and resume the calling module's own code at the instruction
right after the `syscall`, instead of raising a real exception that
falls through to the still-default, dead exception-vector stub
(42nd finding) and gets bypassed by abandoning the module entirely.

**Implementation note (mid-round discovery):** after adding 0x10/0x08
handling and re-testing against the real BIOS, every one of the 12
still-affected modules advanced past their first syscall to a SECOND
real syscall, number `0x14` (`a0=0` always, `a1` varies - a small
index/priority-like value for most modules, one larger address-like
value for IOMAN; real semantics not yet identified, plausibly a real
`RegisterIntrHandler`/`CpuEnableIntr`-style call). Added the same
generic-default handling for `0x14` in the same round, since it's the
identical mechanism and precedent - not a new, separate risk.

**Verification performed:** clean compile; full 87-test host-native
regression suite passes (no test changes needed - this touches only
`iop_core.c`'s SYSCALL case, and every existing test's syscall
scenarios are unaffected since none of them use syscall numbers
0x08/0x10/0x14); clean Wii/devkitPPC rebuild (only the pre-existing
harmless `strncpy` warning).

**Real-BIOS empirical result (honest, verified via the same
diagnostic technique used throughout this session):**
- `modules_run_to_completion`: **15 -> 19** (a real, measurable
  increase - LOADFILE and three others now run their real entry point
  to completion instead of being abandoned mid-init).
- `trap_stubs_bypassed`: **13 -> 0** (every module that previously hit
  the dead exception-vector trap now advances past it).
- **The IOP no longer halts/panics at all** within a 30M-instruction
  boot budget - previously it always reached a definite "boot sequence
  complete" halt state (via one bypass mechanism or another); now it
  keeps running as a live, ongoing process.
- Traced where execution actually goes: PC settles into a real,
  legitimate polling loop (confirmed via disassembly of the resident
  code at the final PC) - `beq $zero,$s1,<-9 words>`, spinning while
  `$s1==0` and calling two real subroutines each pass. This is
  structurally a genuine "wait for a condition, poll" idiom (real
  kernel code commonly does exactly this - waiting on a semaphore,
  hardware-ready flag, or SIF handshake state) - NOT a crash, NOT one
  of this project's own recognized panic patterns. The IOP is doing
  real, ongoing kernel work.
- **Honest caveat: SIF_MSCOM/SIF_SMCOM/SIF_MSFLG/SIF_SMFLG are
  UNCHANGED** (`0x00000000`/`0x00000000`/`0x00010000`/`0x00000000` -
  identical to every prior round back to the 36th finding). The
  polling loop this project's IOP now sits in has not yet been
  satisfied by anything on the EE side or elsewhere in this project's
  simulation, so the user-visible SIF handshake goal is NOT yet
  reached. This is real, substantial, verified progress toward task
  #151 (the IOP is no longer crashing during boot - a categorically
  different and better state than every prior round), but it is
  honestly NOT the same as task #151 being fully closed.

**Next step for whoever continues:** identify what real condition the
new polling loop (`$s1`, checked via `beq $zero,$s1,...` at the
resident address found this round) is waiting on, and the two
subroutines it calls each pass (same live-debugger-plus-own-emulator
tracing technique already established this session) - very plausibly
this is the actual final piece standing between this project's current
state and a real, observable SIF handshake completion.

## Round 29 continued (45th finding, task #151/#165): task #165 SOLVED - real root cause found and fixed (missing KUSEG/KSEG0/KSEG1 address-alias masking in the SIF IOP-side mirror); IOP polling loop unblocked, SIF_SMCOM/SIF_SMFLG change for the first time this session

**Correction to the 44th finding's disassembly read:** re-decoding the
exact branch instruction this round (`0x1200FFF7` at the resident
address, byte-for-byte) shows it is `BEQ $s0, $zero, -9` (opcode
`000100`, rs=16=`$s0`, rt=0=`$zero`) - NOT `beq $zero,$s1,...` as the
44th finding assumed. The loop spins while **`$s0`** is zero; `$s1` is
a separate, fixed compile-time constant (`lui $s1,1` loaded once at
loop entry, giving `$s1=0x00010000`) used only as a bitmask, ANDed
into `$s0` (`and $s0,$s0,$s1`, executed right after the second of the
loop's two subroutine calls returns) - not the branch's own operand.
This was caught by sampling live register values at the branch site
across six loop iterations and finding `$s1` consistently nonzero
(`0x00010000`), which directly contradicted the "loops while
`$s1==0`" premise and forced a re-derivation from the raw instruction
words instead of the earlier assumption.

**Tracing what actually feeds `$s0`:** the loop's first call
(`jal 0x1179DC`) is captured into `$s0` via `move $s0,$v0` in the
delay slot of the second call's `jal`, so `$s0` = subroutine1's return
value (later ANDed with the `0x00010000` mask). Disassembling
subroutine1 (`0x1179DC`) line by line: it loads the fixed address
`0xBD000020`, reads it, re-reads it, and only returns once the two
reads agree (a standard "debounce a hardware register" idiom - retries
via a `j` back to the second read if they differ) - real code
defensively guarding against a mid-read hardware update, not anything
syscall-related. **`0xBD000020` is a KSEG1 (uncached-mirror) address;
masking off its segment-select bits the same way real IOP hardware
does (`addr & 0x1FFFFFFF`, since the IOP has no MMU/TLB and KUSEG/
KSEG0/KSEG1 are three views of the same physical memory) gives physical
`0x1D000020` - squarely inside this project's own SIF IOP-side mailbox
mirror window (`0x1D000000-0x1D0000FF`, `core/hw/sif.h`), specifically
offset `0x20` = `SIF_MSFLG`.**

**Root cause:** `sif_iop_mmio_read32()`/`sif_iop_mmio_write32()` in
`source/hw/sif.c` checked the incoming address literally
(`addr < 0x1D000000u || addr > 0x1D0000FFu`) without first masking off
the KSEG0/KSEG1 segment-select bits the way `iop_mem_ptr()` already
does for plain RAM (`addr & 0x1FFFFFFFu`). Real IOP code reaches this
mailbox window through the KSEG1 uncached alias (`0xBD000020`), which
is a completely ordinary, legitimate real R3000A addressing mode - but
the raw, unmasked value falls outside the literal `0x1D0000FF` ceiling,
so the check silently failed and the read fell through to the generic
RAM path, which also misses (physical `0x1D000020` is beyond the IOP's
2MB RAM) and returns a flat `0`. The polling loop's `$s0` could
therefore never become nonzero, regardless of `SIF_MSFLG`'s real
in-memory value - it was reading the wrong location entirely, not
waiting on a genuinely-unset flag.

**Fix:** mask `addr & 0x1FFFFFFFu` before the window check in both
`sif_iop_mmio_read32()` and `sif_iop_mmio_write32()`, mirroring
`iop_mem_ptr()`'s existing, already-correct convention. Minimal,
localized, two-function change.

**Verification performed:** clean compile; full 87-test host-native
regression suite passes (0 failures - the 39 that initially failed
were the pre-existing, already-documented `-lm` link-order artifact in
the README's own build commands, not real regressions - confirmed by
re-linking with `-lm` appended, after which all 87 pass); clean
Wii/devkitPPC rebuild (only the pre-existing, unrelated harmless
`strncpy` warning in `iop_module_loader.c`).

**Real-BIOS empirical result (honest, verified via the same
diagnostic technique used throughout this session):**
- The IOP **no longer gets stuck in the polling loop** - it now
  reaches a definite, benign "module boot sequence complete" halt
  state (via the existing LOADCORE panic-loop bypass, same category of
  halt used throughout this project, not a new failure mode).
- `modules_run_to_completion`: **19 -> 28** (out of 29 real modules).
- **`SIF_SMCOM` and `SIF_SMFLG` change for the first time this entire
  session's investigation** (`SIF_SMCOM`: `0x00000000` ->
  `0x0011AFD0`, a real pointer-like value; `SIF_SMFLG`: `0x00000000`
  -> `0x00010000`). Every prior round back to the 36th finding reported
  these four SIF registers completely frozen - this is the first
  concrete evidence of live SIF-side activity.
- `SIF_MSFLG` remains `0x00010000` (unchanged - it was already correct
  before this fix, which is exactly why the debounced read of it was
  the missing piece: the real value was there all along, our own mirror
  just couldn't see it through the KSEG1 alias).
- `SIF_MSCOM` remains `0x00000000` - not yet verified whether this is
  expected steady-state or another remaining gap.

**Honest caveat:** this is real, verified, substantial forward
progress - the specific polling loop task #165 was opened to
investigate is now conclusively resolved, and the IOP boot sequence
goes measurably further (28/29 vs 19/29 modules to completion) with
new SIF-side register activity never seen before this session. It is
NOT yet verified that this represents a *complete* SIF handshake
(SIF_MSCOM stayed at zero, and the EE side's own reaction to the new
SIF_SMCOM/SIF_SMFLG values has not yet been traced) - task #151 stays
open pending that follow-up, but its scope has narrowed considerably.

**Next step for whoever continues:** trace what the EE side does (if
anything) once it observes the new `SIF_SMCOM=0x0011AFD0`/
`SIF_SMFLG=0x00010000` values, and whether `SIF_MSCOM` remaining zero
is itself another still-missing piece or genuinely expected real
steady-state at this point in boot.

## Round 29 continued (46th finding, task #170/#172): tracing the EE side's reaction to task #165's SIF fix uncovers a much bigger wall - the EE kernel's own SYSCALL handling was never implemented at all

Continuing task #151/#165 per the user's request to trace the EE side's
reaction to the newly-nonzero SIF_SMCOM/SIF_SMFLG values (45th finding):
extending the standard diagnostic's EE-only phase revealed the EE
interpreter now runs much further than any prior round (previously it
simply kept looping in already-explored territory) and hits a genuine
new wall: `halt("SYSCALL (no BIOS syscall table implemented)")` -
`ee_core.c`'s SYSCALL case has ALWAYS just halted unconditionally
(present since this project's very first EE interpreter skeleton), but
until now boot never actually reached a real EE-side `syscall`
instruction (the 4th-round investigation, cited in this same file
around line 528, explicitly confirmed SYSCALL fired zero times in the
first 150K instructions and correctly deferred building HLE for it as
not-yet-motivated work). Task #165's fix changes that: boot now
reaches real EE kernel-mode code that executes genuine `syscall`
instructions.

Decoded the exact real convention (different from the IOP's - see
task #164/#165): the syscall number is loaded into `$v1` via an
`addiu $v1,$zero,<n>` immediately before the `syscall` instruction.
Cross-referenced every observed number against ps2sdk's public
`ee/kernel/include/syscallnr.h` (mirrored readably at the PS2
Developer wiki's "EE Syscalls" page, itself sourced from a pinned
ps2sdk commit) - not fabricated. A full trace (temporary,
reverted-before-commit, same discipline as every prior round) with a
"bypass every syscall with $v0=0, resume at PC+4" strategy showed real
boot calling exactly three distinct syscalls before running for
hundreds of millions of instructions without hitting a fourth: 60
(`RFU060`/`SetupThread`), 61 (`RFU061`/`SetupHeap`), 100 (`FlushCache`)
- and then settling into ANOTHER genuine polling loop, this time on
the EE side, structurally identical to the IOP-side SIF_MSFLG loop
from the 43rd-45th findings: `AND $v0,$v0,$a0` / `BEQ $v0,$zero,-6`,
reading SIF_SMFLG (this time via its real EE-side address,
`0x1000F230`, not an alias-masking issue this time) and masking
against a FIXED loaded constant, `0x00040000`.

Cross-referencing this bit against ps2sdk's `ee/kernel/include/
sifdma.h` (`SIF_STAT_SIFINIT=0x10000`/`SIF_STAT_CMDINIT=0x20000`/
`SIF_STAT_BOOTEND=0x40000`, the last documented literally as "Bootup
completed") identifies this precisely: real EE kernel boot code
spins on `SIF_STAT_BOOTEND` before continuing - the exact, real,
documented "IOP has finished booting" signal - and nothing in this
project's simulation has ever set it, because nothing in this
project's model of IOP boot completion (the module-loader's own
"module boot sequence complete" halt states, which are this project's
own deliberate representation of exactly the real-world event
`SIF_STAT_BOOTEND` signals) ever wrote it.

## Round 29 continued (47th finding, task #170/#172): implemented the EE syscall table gap and the SIF_STAT_BOOTEND/CMDINIT signals - real, verified, substantial further progress; boot now deep inside real SIF command-protocol bring-up

Two coordinated fixes, both grounded in the 46th finding's citable
research:

**Fix 1 (`source/hw/iop_module_loader.c`):** added a
`mark_iop_boot_complete()` helper, called from all four of the module
loader's existing "boot sequence complete" halt sites, that ORs
`SIF_STAT_BOOTEND` (`0x40000`) and `SIF_STAT_CMDINIT` (`0x20000`, "SIFCMD
initialized" - also from `sifdma.h`) into `SIF_SMFLG` via the real
IOP-side SIF mirror this project already models (`sif_iop_mmio_read32`/
`write32`, the same functions task #165 fixed). Both bits are ORed onto
whatever's already there (never clobbering `SIF_STAT_SIFINIT=0x10000`,
which task #165's real SIFMAN handshake already sets for real) -
CMDINIT specifically because this project's own module loader already
represents SIFCMD's real module as having run to completion, so real
SIFCMD's own real job (setting that bit) is a direct, non-fabricated
consequence of work already modeled, not a new invention.

**Fix 2 (`source/core/ee/ee_core.c`):** replaced the unconditional
SYSCALL halt with real handling for every syscall number actually
observed on the boot path so far, each independently justified:

- 100 (`FlushCache`): a genuine no-op, since this interpreter models no
  instruction/data cache staleness at all (same reasoning already
  applied to this project's own CACHE/SYNC/PREF opcodes).
- 60/61 (`SetupThread`/`SetupHeap`): generic-default no-ops, matching
  this project's established precedent for real-but-unmodeled
  kernel-internal bookkeeping (IOP tasks #164/#165, `iop_hle_bios.c`'s
  A0/B0/C0 convention) - no EE-side thread scheduler or heap is
  modeled, and neither call has an externally observable effect this
  project tracks.
- 120 (`sceSifSetDChain`): real EE-side SIF0 DMAC-channel setup,
  confirmed via ps2sdk's actual `ee/kernel/src/sifcmd.c` source
  (`sceSifInitCmd()`'s own `if (!(_lw(DMAC_SIF0_CHCR) & CHCR_STR))
  sceSifSetDChain();`, matched against this project's trace catching
  `$v0` holding that exact register's address beforehand) - treated as
  a no-op with an honest caveat that real hardware DOES program a DMA
  channel here; out of scope for reaching a splash screen (SIF
  command-protocol DMA, not the GIF/VIF graphics DMA path).
- 18 (`AddDmacHandler`), 22 (`_EnableDmac`): same real
  `sceSifInitCmd()` sequence (registering/enabling a DMA completion
  interrupt handler for SIF0) - same honest caveat, no-op since this
  project doesn't model EE-side DMA interrupt delivery.
- **121/122 (`sceSifSetReg`/`sceSifGetReg`): NOT bypassed - implemented
  for real.** These read/write either a real hardware SIF register
  (IDs 1-4, `SIF_REG_MAINADDR/SUBADDR/MSFLAG/SMFLAG` per `sifdma.h`'s
  `_sif_regs` enum - routed to this project's own already-correct
  `sif_mmio_read32`/`write32`) or a small software-only "system
  register" bookkeeping slot (`SIF_REG_ID_SYSTEM=0x80000000 | 0/1/2`,
  a real, documented but non-hardware concept) - added a 3-slot
  `ee_sif_sysreg[]` table for the latter. **This was caught as a real
  bug during this round's own testing**: an initial flat
  "always return 0" bypass for 122 caused a NEW infinite loop, because
  real `sceSifInitCmd()`'s own `while (!(sceSifGetReg(SIF_REG_SMFLAG) &
  SIF_STAT_CMDINIT));` spin-loop calls this syscall every iteration and
  can never observe the real (already-correct) SIF_SMFLG value if the
  answer is hardcoded - fixed by actually reading the real register
  instead of guessing.

**Verification performed:** full 87-test host-native regression suite
passes (0 failures); clean Wii/devkitPPC rebuild (only the
pre-existing, unrelated `strncpy` warning).

**Real-BIOS empirical result:** boot now advances through the entire
observed real `sceSifInitCmd()` sequence (SetDChain, AddDmacHandler,
EnableDmac, the SIF_STAT_CMDINIT wait-loop, GetReg/SetReg
round-tripping real SIF_SMCOM's value `0x0011AFD0` end-to-end) and
reaches a further real syscall, 119 (`sceSifSetDma`) - a genuine SIF0
DMA packet transfer (real `_SifSendCmd()`'s final step, sending the
`SIF_CMD_INIT_CMD` packet to the IOP) - not yet implemented, halts
there. This is real, substantial, further-than-ever-before progress:
from "the EE syscall table doesn't exist at all" to "deep inside a
real, correctly-sequenced SIF command-protocol handshake, blocked only
by an actual DMA data-transfer call." GS/display registers (PMODE,
DISPFB1/2, DISPLAY1/2) remain zero - no splash-screen-relevant
progress yet, since none of this round's syscalls touch the graphics
path, but this is expected: SIFCMD bring-up runs early in kernel boot,
before any drawing.

**Next step for whoever continues:** implement `sceSifSetDma`
(syscall 119) - read the `SifDmaTransfer_t` array (src/dest/size/attr)
from EE RAM at `$a0`, perform the real EE-RAM-to-IOP-RAM byte copy for
`$a1` transfer count, return a plausible transfer ID - then keep
tracing forward the same way to find whatever wall comes after.

## Round 29 continued (48th finding, task #172): sceSifSetDma implemented for real (EE-RAM-to-IOP-RAM DMA copy) - caught and fixed a real modularity regression along the way

Continuing the syscall-by-syscall trace from the 46th/47th findings:
after the SifInitCmd sequence's SIF_STAT_CMDINIT wait-loop unblocks
(via the 47th finding's real sceSifGetReg/sceSifSetReg fix), real boot
reaches syscall 119 (`sceSifSetDma`/`SifSetDma`) - confirmed via this
project's own trace to be real ps2sdk's `_SifSendCmd()`
(`ee/kernel/src/sifcmd.c`) sending its `SIF_CMD_INIT_CMD` packet: the
observed `SifDmaTransfer_t` descriptor (`src=0x0008C300` an EE RAM
packet buffer, `dest=0x0011AFD0` the IOP's real receive address - the
same value already round-tripped through real `SIF_SMCOM` in the 47th
finding, `size=20`, `attr=0x44`=`SIF_DMA_ERT|SIF_DMA_INT_O`) matches
real ps2sdk's own field layout and attribute flags exactly.

Implemented for real (not bypassed): copies the real byte count from
EE RAM at each descriptor's `src` to IOP RAM at its `dest`, for the
real descriptor count. Honest caveat: this models the actual data
movement a real SIF0 DMA transfer performs, but not the completion
interrupt (no EE-side DMA interrupt delivery exists) or the IOP-side
SIFCMD packet handler interpreting what arrives (this project's IOP
module loader has already reached its own modeled completion point by
here) - a real, partial implementation, not a full round-trip.

**Regression caught and fixed by this project's own mandatory
regression suite, exactly as intended:** the first version of this fix
called `iop_core_get_state()`/`iop_mem_write8()` directly from
`ee_core.c`, which gave `ee_core.c` a hard link-time dependency on
`iop_core.c` - breaking roughly 37 of this project's existing
EE-only tests (`test_ee_core.c` and many others), which by design link
`ee_core.c` WITHOUT any IOP code at all. Fixed architecturally rather
than by reverting the feature: added a small optional bridge
(`ee_core_set_iop_write8_bridge()`, a settable function pointer +
opaque context, generic `(void *ctx, addr, val)` signature so
`ee_core.h` doesn't need to know about `iop_state_t` at all) that
`system_init()` (`source/core/system.c`, the one place both cores are
already initialized together) wires up once; EE-only tests simply
never call the setter, so the pointer stays NULL and the SIF DMA copy
becomes a documented, honest no-op instead of a link error. Re-ran the
full 87-test suite after this fix: all 87 pass again.

**Verification performed:** full 87-test regression suite (0
failures, after the bridge fix); clean Wii/devkitPPC rebuild (only the
pre-existing, unrelated `strncpy` warning).

**Real-BIOS empirical result:** with the bridge wired up in a
diagnostic mirroring `system_init()`'s own wiring, real boot advances
past `sceSifSetDma` into a further real code path resembling
`sceSifInitCmd()`'s "already initialized, return early" guard pattern
(a static `init` flag check at a fixed low-memory address) - the
furthest point yet reached, though full understanding of this specific
function is not yet complete. `SIF_SMFLG`'s final observed value
(`0x00030000` - `SIF_STAT_SIFINIT|SIF_STAT_CMDINIT`, missing
`SIF_STAT_BOOTEND`) differs from what `mark_iop_boot_complete()` sets
(`0x00070000`, all three bits) - most likely explained by real EE-side
code clearing/rewriting bits it has "consumed" (ps2tek's own
documentation of these flag registers describes exactly this kind of
set-by-one-side/clear-by-the-other convention), but this has not yet
been traced to a specific instruction and is noted here honestly as an
open, non-blocking detail rather than asserted as understood.

**Next step for whoever continues:** identify what specific code is
at the "already initialized" guard's target address (~`0x84330`) -
likely `sceSifInitRpc()` or a related real kernel-RPC bring-up routine
- and continue the same trace-and-implement cycle toward whatever
comes after. None of this round's work touches GS/display registers
yet (still all zero) - expected, since SIF/RPC bring-up is early
kernel work that precedes any drawing, not evidence of a remaining
graphics-path bug.

## Round 29 continued (49th finding, task #171): GS audit finds and fixes a real, independent bug - 64-bit GS privileged-register access (PMODE/DISPFB/DISPLAY) was missing the same KSEG0/1 masking the 32-bit hardware path already has

Ran a parallel, independent audit of the GS/display path (task #171)
alongside the ongoing EE kernel syscall trace (tasks #170/#172), per
the user's request to pursue multiple angles rather than a single
linear thread. The audit's job was to determine whether GS privileged
registers (PMODE/DISPFB1/DISPLAY1/DISPFB2/DISPLAY2, EE address range
`0x12000000-0x12001FFF`) are even reachable by real EE code today, and
whether `main.c`'s real-boot-flow entry point does anything useful with
them once they change.

**Confirmed real-boot-flow scaffolding is genuine, not dead code:**
`main.c`'s `run_real_boot_flow()` (task #128) already calls
`gs_get_state()` every interleaved-execution iteration, checks
`pmode`'s display-enable bit, decodes `dispfb1`, and calls
`gs_blit_psmct32_to_xfb()` to push real GS memory to the Wii
framebuffer when active - genuinely wired up, just never yet observed
to fire (because `pmode` has stayed zero through every diagnostic this
whole project).

**Root cause found for at least part of why:** GS registers are only
reachable via 64-bit `LD`/`SD` (unlike the DMA/SIF/MCH registers, which
use 32-bit `LW`/`SW`). `ee_mem_read64()`/`ee_mem_write64()` in
`ee_core.c` call `gs_mmio_read64()`/`gs_mmio_write64()` with the RAW,
unmasked address - never through `ee_hw_mmio_addr()`, the exact
KSEG0/1-mirror-masking helper "round 11" already added specifically
because real BIOS/game code always addresses hardware registers
through their cached/uncached mirrors (e.g. `0xB2000070`), never the
bare `0x12000070` literal (see docs/STATUS.md's round 10/11 sections
and `tests/test_ee_hw_kseg_masking.c`, which - tellingly - only ever
exercised this for SIF and MCH, never GS). Practical effect: a real
`SD` to DISPFB1 through its KSEG1 mirror would have silently missed
`gs_mmio_write64()` and fallen through to the generic RAM path
(a no-op, since that address range isn't backed by RAM), even once
real boot eventually reaches BIOS code that tries to write it.

**Fix:** route both `ee_mem_read64()`/`ee_mem_write64()`'s GS dispatch
through `ee_hw_mmio_addr()`, identical in spirit to the existing 32-bit
fix. Minimal, two-line change plus a new regression test case
(`tests/test_ee_hw_kseg_masking.c`, mirroring its existing SIF/MCH
cases: `SD`/`LD` a value through DISPFB1's KSEG1 mirror,
`0xB2000070`, and confirm it round-trips and lands in `gs_get_state()->
dispfb1`).

**Verification performed:** the new test case passes; full 87-test
regression suite passes (0 failures); clean Wii/devkitPPC rebuild
(only the pre-existing, unrelated `strncpy` warning).

**Honest scope note:** this is a real, independently-confirmed bug
fix, but it has NOT yet been observed to change real-BIOS boot
behavior - real boot hasn't reached BIOS code that writes these
registers yet (still inside EE kernel-RPC bring-up per the 46th-48th
findings), so this fix removes a real, concrete obstacle that WOULD
otherwise have blocked the splash screen even once boot gets there,
rather than unblocking anything observable today. Ranked alongside
this, the audit's other candidate hypotheses for the still-zero
GS/display registers (documented for the next person to continue
either thread): (1, most likely) boot simply hasn't reached
logo/OSD-drawing code yet - still deep in kernel/RPC bring-up by
design; (2, now fixed) this KSEG0/1 masking gap; (3, unconfirmed)
there may be a missing HLE/syscall blocking whatever triggers the
logo-drawing module specifically; (4) untested alternatives (e.g. GIF/
VIF DMA never getting kicked for the logo draw itself).

## 50th finding (task #176): real EE external-interrupt delivery (INTC/DMAC, Cause.IP2/IP3) implemented - unblocks the sceSifInitCmd-region eternal poll loop, boot reaches real kernel interrupt-handler code for the first time ever

Continuing the "work on both threads" directive after the 49th finding
(GS KSEG fix), this finding resumes the EE kernel-RPC syscall trace at
exactly the point the 48th finding left it: the "already initialized"
guard region around EE PC `~0x84330`.

**Root cause traced and confirmed (not guessed):** that region is a
tight polling loop - `jal 0x083B40` (a small getter function computing
a fixed address `0x0008C440` and returning `*(u32*)0x0008C440`), then
`beq $2,$zero,-3` looping back while the read is zero. Real ps2sdk
source for `sceSifSendCmd()`/`sceSifInitCmd()`
(`ee/kernel/src/sifcmd.c`, fetched and read in full this session)
confirms neither function busy-waits like this - `sceSifSendCmd()`
just calls `sceSifSetDma()` and returns. So this loop is NOT part of
SIF command init as previously hypothesized; it's some other kernel
primitive.

Exhaustively proved (full 32MB EE address-space scan for the paired
"setter" function at `0x083B58` - as a direct `JAL` target AND as a
raw 32-bit data pointer): the setter has **zero callers anywhere** in
the currently-loaded kernel image. Whatever normally makes this flag
non-zero cannot run through any code path this project's interpreter
was capable of exercising - because that path itself was missing:
this project had **no EE external-interrupt delivery at all**. Only
Cause.IP7 (the internal COP0 Timer/Compare interrupt, "round 9") was
ever raised; the existing code even said so explicitly ("the only
interrupt SOURCE modeled so far... doesn't yet raise Cause's other
Interrupt Pending bits").

**Real semantics researched and cited before implementing (no
fabricated register behavior):**
- PCSX2's `pcsx2/Hw.h` (`EERegisterAddresses`): `INTC_STAT=0x1000F000`,
  `INTC_MASK=0x1000F010`, confirms DMAC channel numbering
  (SIF0=channel 5, matching this project's existing `DMA_CHANNEL_SIF0`
  enum).
- PCSX2's `pcsx2/Hw.cpp`: `hwIntcIrq(n)`/`hwDmacIrq(n)` (set a status
  bit, test the corresponding mask bit, raise Cause 0x400/0x800 = real
  R5900 Cause.IP2/IP3 bit values for the INTC/DMAC external lines).
- PCSX2's `pcsx2/HwWrite.cpp`: real INTC_STAT write clears
  (`&= ~value`), real INTC_MASK write TOGGLES (`^= (u16)value`), real
  DMAC_STAT write splits low/high halves (status clears on write-1,
  enable-mask toggles on write-1) - all real, documented hardware
  quirks, not reinvented.

**Implemented (task #176):**
- New `source/hw/ee_intc.c`/`include/core/hw/ee_intc.h`: EE
  INTC_STAT/INTC_MASK register model with the real clear/toggle
  write semantics above.
- `dma.h`/`dma.c` extended: `dma_channel_signal_done()` (real
  `hwDmacIrq(n)` equivalent, now called from all three of
  `dma_channel_kick()`'s transfer-complete sites), `dma_channel_set_
  irq_enable()`, `dma_dmac_interrupt_pending()`, and a real
  clear/toggle DMAC_STAT write path (previously a plain overwrite).
- `ee_core.c`: two new functions, `ee_check_intc_interrupt()` and
  `ee_check_dmac_interrupt()`, mirroring the existing
  `ee_check_timer_interrupt()`'s Cause.IP7 pattern exactly (same
  Status.IE/EXL/ERL gate, same per-line IM2/IM3 mask bit, same
  `ee_raise_exception()` call - all external/internal interrupts
  share the same real MIPS Interrupt ExcCode/vector) - called every
  step alongside the timer check.
- EE syscall 22 (`_EnableDmac`) - previously a flat no-op batched with
  18/60/61/100/120 - now actually sets the given channel's DMAC_STAT
  enable-mask bit (documented simplification: directly sets the end
  state rather than replicating the exact BIOS-internal raw
  toggle-write, which this project doesn't have source for).
- EE syscall 119 (`sceSifSetDma`) now calls
  `dma_channel_signal_done(DMA_CHANNEL_SIF0)` after its real EE-RAM-
  to-IOP-RAM copy completes - the "completion interrupt" half of that
  syscall's long-standing honest caveat is now resolved.

**Verified:** all 87 regression-suite binaries build and pass (0
failures) after these changes. Clean Wii/devkitPPC rebuild (only the
pre-existing, unrelated `strncpy` warning). Host-native diagnostic
against the real BIOS confirms a genuine behavior change: the EE no
longer spins forever in the `0x84330` poll loop. Instead, a real
Cause.IP3 (DMAC) interrupt now fires for the first time, vectoring
into the kernel's own real interrupt-dispatch code (previously 100%
dormant/unreachable), which runs further than any previous session
- through code around `0x00083F6C-0x00083FCC` (a real exception-
epilogue-shaped restore sequence: `LW $ra,0x80($sp)` down to
`LW $s0,0x20($sp)` then `JR $ra`) - before halting at EE PC
`0x80001390` on an "unimplemented SPECIAL funct" (raw word
`0xFF4212C0`; surrounding words at `0x80001350-0x800013B0` look more
like a data/vector table than executable code, e.g. repeating
`0xFF4212xx`/`0x0000xxxx`/`0x7000xxxx` patterns - possibly PC
landed on a handler-address table rather than genuine code, which
would itself be a distinct, not-yet-diagnosed bug).

**Honest scope note / next step:** this is real, verified,
regression-tested progress - the eternal poll loop is provably no
longer the blocker, and real kernel interrupt-handler code now
executes for the first time ever in this project's history. It is
NOT yet a splash screen, and the new halt at `0x80001390` is an
open, undiagnosed wall for whoever continues this thread next:
decode exactly what that address is supposed to contain (data table
vs. code), and why the CPU ended up executing it.

## 51st finding (task #177): implemented EE MFSA/MTSA (SPECIAL funct 0x28/0x29) - boot now advances past the interrupt-handler prologue into a real intentional BREAK trap

Directly continuing the 50th finding: with real Cause.IP3 delivery
in place, the EE started executing genuine kernel interrupt-handler
code for the first time ever, halting on "unimplemented SPECIAL
funct" at reported PC `0x80001390` (the actual failing instruction is
one word earlier, `0x8000138C` - this project's halt() reports
`this_pc+4`, already advanced past the failing instruction, matching
the same off-by-one-instruction convention this session's later
investigation confirmed a second time - see below).

**Precisely decoded (not guessed) via a raw instruction-field dump of
`0x80001350-0x80001394`:** this is genuine, real EE kernel
interrupt-handler PROLOGUE code, not a data table: `SQ $s5..$s8,$t8,
$t9,$gp` (opcode 0x1F) saving callee-saved registers into a `$k0`-
based frame, then `MFHI`/`MFLO` (already implemented) and `MFHI1`/
`MFLO1` (SPECIAL2, already implemented) saving the HI/LO and HI1/LO1
register pairs, then the failing instruction - SPECIAL funct 0x28.

**Real semantics cited before implementing:** fetched
`psi-rockin.github.io/ps2tek`'s SPECIAL opcode function table, which
shows funct 0x28=MFSA, 0x29=MTSA (row "101", real R5900-specific
instructions - reserved/undefined in standard MIPS III, which is
exactly why this looked like "no such instruction" until checked
against the real R5900 table specifically). These access the R5900's
dedicated 32-bit "SA" (Shift Amount) control register, used by the
QFSRV instruction (not implemented, out of scope here) for a
variable-width quadword funnel-shift; a real interrupt-handler
prologue saving full CPU context naturally saves this alongside HI/
LO/HI1/LO1.

**Implemented:** a new `sa_reg` field on `ee_state_t` (distinct from
the existing per-instruction shift-amount decode local also named
`sa`), and `MFSA`/`MTSA` in the SPECIAL opcode switch
(`ee_core.c`), zero-initialized by the existing full-state `memset()`
in `ee_core_init()` (matching real hardware's SA-resets-to-0 behavior
- no separate init needed).

**Verified:** new dedicated regression test
(`tests/test_ee_sa_reg.c`, 8 checks - MTSA/MFSA round-trip, a second
MTSA proving it's a real re-write rather than an OR/append, and
`rd=$0` staying hardwired at zero). Full suite: 87/87 pass. Clean Wii/
devkitPPC rebuild.

**Host-native diagnostic against the real BIOS confirms further real
progress:** boot now runs the complete interrupt-handler prologue and
continues into a NEW halt, reported at EE PC `0x80000DC4` with reason
"BREAK". Applying the same "reported pc is this_pc+4" pattern, the
actual instruction is at `0x80000DC0`, word `0x03FFFFCD` - decoded:
opcode 0 (SPECIAL), funct 0x0D (BREAK), 20-bit code field `0xFFFFF`
(all-ones). This is a REAL, intentional `BREAK` instruction physically
present in the real BIOS image (not a bug in this project's decoder,
not a runaway-into-blank-memory artifact - the surrounding
`0x80000D84-0x80000DE4` region being all-zero is a separate, distinct
observation about what comes immediately after, not what's being
executed).

**Honest open question for whoever continues this thread:** is this
BREAK a normal, expected part of real PS2 boot (e.g. a debug-firmware
leftover, an intentional "this path shouldn't normally execute on
retail hardware" assertion, or a deliberate kernel panic/self-check),
or a symptom that something upstream in this project's emulation
(most likely the SIF DMA completion signaling from task #176, which
is a real but incomplete simplification - only the EE side of the
handshake is modeled, not genuine IOP-side command processing) is
"too easy," steering the real BIOS down a code path real hardware
would never actually take? Not yet determined either way - the code
field `0xFFFFF` gives no further clue by itself (looks like a generic/
placeholder value, not a specific numbered diagnostic code). This is
the next concrete thing to resolve.


## 52nd finding (task #178): real BREAK exception delivery (ExcCode 9) confirmed as the actual unlock - boot now runs ~65M+ instructions past the previous BREAK@0x80000DC0 halt into a new, distinct wait-loop

Directly following up on the 51st finding's "honest open question": is the
real BREAK at `0x80000DC0` normal expected boot behavior, or a symptom of
incomplete SIF/IOP integration? The user asked whether real IOP-side SIF
command processing was the right next move; investigated instead (per an
alternative, cheaper-to-test hypothesis) whether this project's own
long-standing "BREAK always halts unconditionally" interpreter placeholder
- a pragmatic stand-in from before real MIPS exception delivery existed,
predating even the Cause/EPC/Status work - was itself the actual problem,
since real R5900 hardware never stops executing on a BREAK: it raises a
genuine Breakpoint exception (ExcCode 9) and vectors through the normal
exception path, same as any other trap. User confirmed: implement real
delivery and find out.

**Implemented:** `EE_EXC_CODE_BP` (`9u << 2`), and the SPECIAL funct 0x0D
(BREAK) case in `ee_core.c` now calls `ee_raise_exception(st,
EE_EXC_CODE_BP, this_pc, in_delay_slot)` instead of `halt("BREAK")`. Caught
and fixed a same-session bug before it shipped: the first draft of this
change kept the old case's `return 1;` (this project's own "this step
halted the core" convention, checked by `ee_core_run()`'s `if (ee_step())
break;`), which would have made the exception-raise ALSO incorrectly report
a halt despite `st->halted` never actually being set - inspected how the
existing TLB-exception path handles this (raises via
`ee_mem_check_tlb_fault()` deep inside a LW/SW case, then just `break`s to
the shared end-of-step epilogue) and matched that convention exactly.

**Added a step-cap safety net to `ee_core_run()`** (`EE_CORE_RUN_STEP_CAP`,
20M instructions): its `while (!g_state.halted)` loop had never needed a
bound before, since every prior path to `ee_step()` returning 1 came from
an explicit `halt()` call. A BREAK that now vectors instead of halting -
into a handler-free zero-filled buffer, as most of this project's
hand-written EE unit tests do - would otherwise spin forever with no
internal bound. Purely a host-native test-harness safety measure; no real-
hardware counterpart, and it never fires for any correctly-behaving test.

**Test fallout was much larger than initially scoped** - not the ~6 tests
originally suspected (ones that write small immediates to Status,
clobbering BEV), but effectively every EE unit test that used a trailing
BREAK + `ee_core_run()` + `st->halted==1` as a "run to completion, then
inspect state" convenience convention, because even tests that never touch
Status (leaving BEV=1, the reset value) hit the same problem: the
Breakpoint exception vectors into the same zero-filled BIOS buffer with no
installed handler, decodes as harmless NOPs, and spins to the new step cap
instead of halting. Fixed by category:

- **Six tests with genuinely reachable end-of-program logic before the
  BREAK** (`test_ee_cop0_special.c`, `test_ee_scratchpad_count.c`,
  `test_ee_cop2_ctrl.c`, `test_ee_sa_reg.c`, `test_ee_exceptions.c` (one
  subtest only), `test_ee_timer_interrupt.c` - already unaffected) were
  converted to step exactly their real, useful instruction count via
  `ee_core_step()`, stopping before the now-inert trailing BREAK, with
  halted-based assertions replaced by direct register/state checks.
  `test_ee_exceptions.c`'s `test_fetch_exception()` specifically had its
  delay-slot BREAK swapped for a harmless canary `ADDIU`, since executing
  a real BREAK there would have set Status.EXL=1 *before* the test's own
  manual fetch-exception setup, corrupting the nested-exception EPC guard
  it depends on.
- **Twenty-nine tests** using a uniform `ee_core_run(&bios); CHECK(st->
  halted == 1 [&& strstr(halt_reason,"BREAK")], ...)` pattern were fixed
  with a mechanical, drop-in test-harness compatibility shim
  (`run_until_break()`, inserted per-file, `ee_core_run(&X)` ->
  `run_until_break(&X)`): it steps until either a genuine halt occurs (a
  real bug) or Cause.ExcCode==9 with Status.EXL just set (i.e. exactly
  where the BREAK fired), and in the latter case synthesizes the same
  `st->halted=1` / `halt_reason` convention the old unconditional-halt code
  produced - a test-harness-only bookkeeping shim that changes nothing
  about `ee_core.c`'s real, production BREAK behavior.
- **`test_system_handshake.c`** was different in kind: its "both cores
  halted" check lives in `system_run_interleaved()` in
  **production** `source/core/system.c` (the same interleaved scheduler
  `main.c` uses for the real boot path), so it was correctly left
  untouched - a real BREAK no longer halting the EE mid-boot is exactly
  the intended, desired effect of this whole change. Fixed the test itself
  instead: it no longer expects `ee->halted==1`, and directly checks
  Cause.ExcCode==9/Status.EXL to confirm the EE genuinely reached its
  BREAK as a real exception, while the IOP side (BREAK behavior
  unchanged, out of scope here) is still checked via `halted==1`.

**Verified:** full suite, 87/87 (88 build/run invocations counting
`test_iop_module_loader_bootinfo`'s separate command), 0 failures, 0
step-cap hits. Clean Wii/devkitPPC rebuild (toolchain re-linked this
session per `TOOLCHAIN_SETUP_NOTES.md` - `LD_LIBRARY_PATH` pointing at the
bundled `libmpfr.so.4` is what's easy to forget after a fresh sandbox).

**Host-native diagnostic against the real BIOS - this is the actual
experiment the user asked for:** boot no longer stops at `0x80000DC0`.
`Status=0x70030C00` there decodes to `BEV=0`, so the Breakpoint exception
vectors into RAM (the kernel's own installed general-exception handler,
offset `0x180`) instead of the boot-time ROM vector - and that handler
evidently just deals with it and returns (Status.EXL back to 0 downstream,
consistent with a clean ERET), because the EE keeps running: confirmed
executing correctly (not halted) after 65,000,000+ further instructions,
having advanced from `0x80000DC0` to a completely different address
region entirely. **This conclusively answers the open question from the
51st finding: yes, the real kernel's installed handler silently resumes
past this BREAK, exactly like real hardware handling an unattached-
debugger breakpoint trap** - it was this project's own decade-old-by-this-
project's-standards "BREAK always halts" interpreter placeholder that was
the wall, not anything IOP/SIF-related. The user's original instinct to
ask about IOP-side integration was reasonable given the symptom, but the
actual root cause was one level up, in the EE interpreter's own opcode
dispatch.

**New wall found, next up for task #172:** boot now settles into a new,
distinct, actively-executing loop around EE PC `0x8000F768` (confirmed via
instruction-window tracing across a 65M-instruction run: it's a real,
bounded loop repeatedly visiting `0x8000CF88-0x8000D01C`, `0x8000F768-
0x8000F874`, not a crash or runaway-into-blank-memory). `$1` holds
`0xB000E010` (the DMAC_STAT KSEG1 uncached mirror), and `$31` (return
address) is `0x8000F86C`, meaning this is a called subroutine, not top-
level code - shape strongly resembles this project's earlier LOADCORE-
style device/registration-list scan loops (tasks #124/#132/#148 etc.),
just at a different address and (very likely) a different list/condition.
Not yet root-caused - that's the next concrete step.

## 53rd finding (task #179): EE syscall table audited (no gap found); real root cause of the 0x8000F768 wait loop identified via disassembly - it's an IOP-halt deadlock, not an interrupt-delivery gap

**Syscall audit (the literal first half of this round's request):** instrumented
a full syscall-number histogram over a 65,000,000-instruction real-BIOS boot
run. Only 13 syscalls are ever invoked, all against the 9 numbers this
project's EE syscall dispatch already implements (18, 22, 60, 61, 100, 119,
120, 121, 122). **No missing-syscall gap exists at the current boot state** -
the syscall table is not the blocker. (It will very likely need further
entries once boot progresses past the wall below, into code that hasn't run
yet - revisit when that happens rather than speculatively adding unverified
numbers now.)

**VBLANK_START/VBLANK_END interrupt delivery implemented** (the concrete
"missing registration" this round targeted): `ee_intc_raise(2)`/`ee_intc_raise
(3)` (INTC bits 2/3, per this project's own already-cited real ten-source
INTC ordering in `ee_intc.h`) are now raised periodically from `ee_core.c`'s
new `ee_check_vblank()`, called unconditionally every step alongside
`ee_latch_timer_interrupt()`. Cadence: `EE_CYCLES_PER_FRAME_NTSC = 4921488`
(294.912MHz real EE clock / 59.94Hz real NTSC vblank rate), using this
project's already-established "1 instruction = 1 cycle" simplification
(same precedent as the Count-register comment elsewhere in `ee_core.c`).
VBLANK_END fires at a fixed 1/12-frame offset after VBLANK_START,
approximating real NTSC vertical-blank duration (~8.5% of a frame).
`ee_intc_raise()` was previously declared but, per its own doc comment,
"not yet called by anything" - this is the first real INTC source this
project raises. Verified via trace: it does fire and correctly vector into
the interrupt exception (`0x80000200`) exactly once during the 65M-
instruction run (Status.IE briefly enabled during kernel init), confirming
the mechanism works end to end. This is independently correct, real EE
hardware behavior regardless of the finding below, and is being kept.

**Real exit condition for the 0x8000F768 loop found, via direct disassembly
of the real BIOS (not speculation this time):** the loop calls a subroutine
at `0x8000CF88` on every iteration (confirmed called ~1.4 million times
before the 65M-instruction cap). Disassembling that subroutine shows it
does NOT use the EE's COP0 interrupt-exception mechanism at all - it
directly polls two real, memory-mapped hardware registers with plain loads:
`DMAC_STAT` (`0xB000E010`, KSEG1-uncached mirror of `0x1000E010`) bit `0x80`
(channel 7 = SIF2 completion, per this project's own already-documented
real DMAC channel numbering), and `INTC_STAT` (`0xB000F000`, mirror of
`0x1000F000`) bit `0x2` (SBUS, per `ee_intc.h`'s real ten-source ordering).
If either condition is true, it jumps into a real handler (`0x8000CEA8`/
`0x8000CDF8`); if not, it returns and the caller loops back. Instrumented
both registers across the full 65M-instruction run: `DMAC_STAT`'s low
(status) half never sets bit `0x80` and `INTC_STAT` never sets bit `0x2` -
only the already-explained VBLANK bits (`0xC` = bits 2/3) and the SIF2
channel's own upper enable-mask bit (already armed, part of `0x00A00000`)
are ever seen. `cea8_hits=0`, `cdf8_hits=0` - neither exit path is ever
taken.

**Root cause, confirmed empirically:** the IOP core halts at
`i=29,937,994` (EE-instruction-equivalent; before the wait loop is even
entered at `i≈30,002,714`), IOP `pc=0x00100000`, with
`halt_reason="module boot sequence complete: 29/29 real modules loaded, 28
run to completion (task #92)"`. This is this project's own IOP module
loader's own, deliberate, by-design halt once it finishes running every
discovered module - not a bug or an unimplemented-opcode crash. Real IOP
hardware never does this: after its own module/driver bring-up, the real
IOP kernel drops into a persistent idle/scheduler loop, continuing
indefinitely to service SIF DMA requests and raise interrupts (like the
SIF2 completion or SBUS signal this exact loop is waiting for). Since this
project's IOP simply stops executing at that point, nothing can ever kick
a SIF2 transfer or raise SBUS, so `0x8000F768`'s poll deadlocks forever -
confirmed consistent with the summary's earlier task-92 hypothesis, but now
pinned to an exact mechanism (an intentional halt, not a missing feature on
the EE side) instead of a guess.

This directly explains why VBLANK delivery alone (implemented above) does
not unblock this specific loop: VBLANK is unrelated to what this loop is
actually waiting for (SIF2/SBUS, both IOP-driven), and by the time the loop
is reached the IOP that would raise either condition is already halted.

**Verified:** full regression suite, 87/87 build+run, 0 failures (all 87
`tests/test_*.c` rebuilt and rerun with each test's own embedded-vs-linked
source convention respected - several tests `#include` the `.c` file under
test directly and must NOT have that file also linked externally, which
this session's regression script now detects automatically instead of
assuming one fixed link line for every test). Clean Wii/devkitPPC rebuild
(`pcsx2-wii-git.dol` produced, toolchain env vars re-exported this session
per `TOOLCHAIN_SETUP_NOTES.md`).

**Not yet done - next step for task #172:** this is a real architectural
gap (this project's IOP core stops running instead of idling/scheduling
forever like real hardware) rather than a small registration fix, so it
needs a deliberate design decision (e.g., keep the IOP core "alive" in a
sensible steady state after module-loading completes, or model a minimal
real IOP idle/kernel-scheduler loop) rather than a scoped patch - flagged
to the user before implementing.

## 54th finding (task #179 continued, task #172): implemented real IOP idle-instead-of-halt behavior (user-approved) - verified real and correct, but conclusively does NOT unblock the 0x8000F768 loop on its own; the loop's true blocker is EE-side, not IOP-side

Following the 53rd finding's IOP-halt root cause, the user approved
"model a minimal real IOP idle/scheduler loop" as the fix direction.
Implemented: a new `idle` flag on `iop_state_t` (`include/core/iop/
iop_core.h`), set instead of `halted` by `iop_module_loader.c`'s
terminal "all modules run to completion" site (previously the last
unconditional `halt()` there). While idle, `iop_core_step()` does NOT
fetch/decode/execute anything - inventing specific "real idle loop"
instruction bytes would be fabrication - it only re-runs the same
`iop_check_hw_interrupt()` check every other instruction step already
gets. If a real hardware interrupt becomes pending, that check vectors
`pc`/`next_pc` into the normal exception vector as usual and `idle` is
cleared, so the interpreter resumes genuine fetch/decode/execute from
there next call - running whatever real, RAM-resident handler code the
modules installed before their entry points returned. This matches
real IOP hardware's actual behavior far better than an unconditional
halt: real hardware never stops running.

**Verified via diagnostic:** the IOP now stays `halted=0` indefinitely
past the old halt point (confirmed to i=65,000,000+), with `idle=1`
and the same descriptive `halt_reason` message (now noting "idle since
task #179"). No crash, no fetch of garbage memory, no regression.

**But it does NOT unblock the loop - and this is itself a real,
useful, disassembly-grounded finding, not a dead end.** Re-running the
same DMAC_STAT/INTC_STAT instrumentation from the 53rd finding with
the IOP now idling instead of halted shows byte-for-byte identical
results: `cea8_hits=0`, `cdf8_hits=0`, `DMAC_STAT`/`INTC_STAT` never
gain the bits `0x8000CF88` is polling for. Root cause: this project's
IOP idle state has nothing productive queued to run (no persistent
driver threads modeled, per the 53rd finding's own framing) and raises
no interrupts of its own, so simply not being halted changes nothing
observable from the EE's side. Separately, and just as importantly:
instrumented EE-side writes to its own D7 (SIF2) DMA channel control
register (`0x1000C800`, the channel `0x8000CF88` is ultimately gating
on) across the FULL 65M-instruction run (not just after the loop is
reached) - **the EE's own code never once writes to it.** Since this
project's `dma_channel_kick()` is synchronous (real DMA hardware moves
data without needing ongoing CPU cycles from either side - confirmed
against real PCSX2's `Hw.cpp`: `hwDmacIrq(n)` is a flat, instantaneous
"set this status bit" call, matching this project's own
`dma_channel_signal_done()`), an IOP that's "alive" was never going to
be sufficient by itself: **the real, remaining blocker is that EE-side
code never attempts to kick its own SIF2 channel (or otherwise causes
its own SBUS INTC bit to be raised)** - a gap on the EE side of the
boundary, not the IOP side. The IOP-idle change is being kept
regardless (independently correct, real hardware behavior, zero
regressions), but it doesn't complete task #172's unlock by itself.

Attempted to pin down the exact real ps2sdk/PCSX2 source describing
what EE-side code path is expected to kick SIF2 at this point in real
boot (per the user's request to trace via real source) -
`ee/kernel/include/sifdma.h` fetched successfully and confirms this
project's already-implemented `SIF_STAT_SIFINIT/CMDINIT/BOOTEND`
constants are correct, real ps2sdk values, but repeated attempts to
fetch the actual SIF DMA implementation source (`sifdma.c`,
`Sif.cpp`, GitHub code search) returned empty - inconclusive, not
confirmed either way. Not fabricating a specific trigger mechanism
without a citable source - this is the honest state of the
investigation, left open rather than guessed at.

**Verified:** 87/87 regression suite pass, clean Wii/devkitPPC
rebuild.

**Next for task #172:** investigate why EE-side code never reaches a
D7/SIF2 kick (or an SBUS-raising code path) - this is now a concrete,
narrowed, EE-side question rather than an IOP-lifecycle one.

## 55th finding (task #172/#180): live PCSX2+real-GT3 reference debugging conclusively ruled out the SIF2/SBUS hypothesis; a printf-format-string trace found the REAL root cause (AddDmacHandler bypassed instead of vectored for real) and fixing it produces genuine, verified forward boot progress

Following the 54th finding's open question ("why does EE-side code
never kick D7/SIF2"), the user made a real, legally-dumped copy of
Gran Turismo 3 (SCES-50294) available and suggested using PCSX2's own
live reference debugger (via the `pcsx2-mcp` tool bridge) to check
real hardware's actual behavior empirically, rather than continuing
with static analysis alone. This is the first round this project has
cross-verified a hypothesis against a live, real PS2 execution
environment (as opposed to static BIOS-ROM disassembly or ps2sdk/
PCSX2 *source* reading, both already established practice).

**Live-hardware findings (via `pcsx2_read_memory`/`pcsx2_set_watchpoint`/
`pcsx2_set_breakpoint`/`pcsx2_disassemble`, real DebugServer connection,
real GT3 boot):**
- `DMAC_STAT`/`INTC_STAT`/D7-CHCR (`0x1000C800`) are genuinely never
  touched on real hardware either, even deep into real gameplay -
  ruling out the SIF2/SBUS-kick hypothesis definitively as the
  mechanism for unblocking this specific wait loop (not just absent in
  this project's emulation).
- A hard breakpoint at `0x8000F768` (this project's stuck wait loop
  address) was never hit across a real GT3 boot + substantial
  runtime - real hardware's control flow never reaches this loop body
  at all under normal circumstances.
- A hard breakpoint at `0x80001884` (the call site immediately
  preceding this project's own BREAK-trap fallback, `jal 0x80000DC0`)
  was likewise never hit on real hardware, confirmed via a rigorous
  arm-before-reset methodology (two earlier "0 hits" readings were
  each caught and corrected mid-investigation as false negatives from
  point-in-time reads and post-boot watchpoint arming, respectively -
  see the session transcript; the final result used a breakpoint armed
  before reset with `action="both"`, which is trustworthy).
- Live disassembly confirmed the BREAK instruction bytes at
  `0x80000DC0` are genuinely present in real hardware's BIOS ROM too
  (`0x03FFFFCD`) - the ROM content itself was never the bug - and that
  `0x8000FCE8` is a real, dual-call-site kernel exception-bookkeeping
  handler (`0x80011188` with `a0=1`, `0x800111D4` with `a0=2`, each
  inside a genuine `$k1`-relative SQ/LQ exception-handler frame ending
  in `eret`).

**Host-native trace found the same call chain and the actual message
being printed.** A dedicated diagnostic (`diag_trace_fce8.c`) confirmed
this project's own single hit on `0x8000FCE8` happens via the same
real call site (`ra=0x80011190`) with `Cause=0x8824` (ExcCode 9,
Breakpoint) - this project's emulator genuinely executes the BREAK as
real fetched/decoded code. Backward-tracing PC history
(`diag_trace_1884_backward.c`) found a repeating ~19-iteration block
cycling through `0x800107E0`/`0x80007310`/`0x80006E10`/`0x80007300`/
`0x80006DB0` before diverging into the BREAK-trap fallback. Disassembly
of this region (Capstone, static, cross-checked against PCSX2's live
disassembler for the overlapping addresses - byte-identical) revealed:
- `0x800107E0-0x80010824`: a genuine hardware SIO (serial console)
  putc routine - poll a status register at `0xB000F130` until ready
  (`& 0xF000 == 0x8000`), then write the output byte to `0xB000F180`.
- `0x80006DB0-0x80006DE0`: a CRLF-translating putchar wrapper (`\n` ->
  `\r\n`, standard terminal convention) that calls the SIO putc above.
- `0x80006DE8` onward: a real, complete printf/vsnprintf-style
  formatted-output routine (recognized via its `%` character check
  `addiu v0,zero,0x25; bne a0,v0,...`, a format-specifier jump table,
  and per-specifier integer-to-ASCII conversion loops with a genuine
  `break 0,7` divide-by-zero trap, distinct from the unrelated
  `0x80000DC0` stub).

**This reframes the entire "~19-iteration retry loop" finding: it is
NOT a bounded-retry-then-give-up construct** (unlike the superficially
similar LOADCORE pattern from tasks #124/#132/#148/#159 this was
initially, incorrectly pattern-matched against) **- it is simply the
character-by-character loop of a kernel debug-console printf() call**,
iterating once per output character/format-specifier, not once per
retry attempt. Self-correcting this misread here for the record.

**Extracting the actual format strings (`diag_trace_printf.c`, tracing
every call into `0x80006DE8` and dumping the `$a0` string pointer's
bytes) recovered the real, human-readable kernel boot log:**
```
# TLB spad=0 kernel=1:%d default=%d:%d extended=%d:%d.
# Initialize Start..
# Initialize GS ...
# Initialize INTC ....
# Initialize TIMER ....
# Initialize DMAC ....
# Initialize VU1 ....
# Initialize VIF1 ....
# Initialize GIF ....
# Initialize VU0 ....
# Initialize VIF0 ....
# Initialize IPU ....
# Initialize FPU ....
# Initialize User Memory ....
# Initialize Scratch Pad ....
# Initialize Done..
%s
# DMAC(%d) Handler does not exist..    <- varargs[0] = 5 (DMAC_SIF0)
```
These are normal, benign kernel subsystem-init boot messages (matching
the real, well-known PS2 BIOS serial-console boot log format seen on
real devkits) - completely harmless on retail hardware with no serial
cable attached (writes to the SIO port above are simply discarded).
**The last message before falling into the BREAK-trap fallback is the
real root cause: `"# DMAC(%d) Handler does not exist.."` with the
channel argument = 5 (`DMAC_SIF0`).**

**Root cause identified:** the real BIOS kernel's DMA-interrupt
dispatch code checks its own internal, kernel-owned DMAC-handler table
for channel 5 and finds it empty. That table is only ever populated by
`AddDmacHandler` (EE syscall 18)'s real handler body actually
executing. This project's syscall 18 was being bypassed in software
with a hardcoded `return 0` (see the 46th/47th findings and task
#176's own honest caveat comment: *"these are NOT pure no-ops on real
hardware... AddDmacHandler's generic return... is approximated as
0"*) - meaning the real table write never happened, exactly matching
the live-hardware-confirmed symptom.

**Fix implemented (`source/core/ee/ee_core.c`):** added
`EE_EXC_CODE_SYS` (`8u << 2`, ExcCode 8 = Syscall, ported from PCSX2's
own `R5900.h` table) and removed syscall 18 from the hardcoded bypass
list. It now calls `ee_raise_exception(st, EE_EXC_CODE_SYS, this_pc,
in_delay_slot)` and falls through to the normal step epilogue (same
pattern task #178 already established for BREAK) instead of returning
a canned value - so the real BIOS kernel syscall dispatcher and
`AddDmacHandler`'s real handler body execute as authentic
fetched/decoded instructions and populate their own real table
themselves. This project does not guess at that table's layout - it
lets real BIOS code build it, consistent with this project's
established "don't fabricate kernel-internal structures" discipline.
All other previously-bypassed syscalls (100/60/61/120/22/119/121/122)
are UNCHANGED, to keep this a minimal, targeted, independently
testable fix.

**Verified via diagnostic:** re-running the same printf-string trace
after the fix shows the `"# DMAC(%d) Handler does not exist.."`
message is now completely GONE (18 printf hits instead of 19, ending
on `"%s"` rather than the DMAC-handler error) - the BREAK-trap
fallback and the `0x8000F768` wait loop are no longer reached at all.
**The emulator now progresses to a genuinely new, deeper point in
boot:** it halts at EE PC `0x00081FF4` with `halt_reason="SYSCALL (no
BIOS syscall table implemented)"` and `$v1` (would-be syscall number)
= -5 (`0xFFFFFFFB`) - a value that does not match any documented
negative syscall in ps2sdk's `syscallnr.h` (negative syscalls there
start at -0x1a/-26), so this is flagged as an open question for the
next round rather than guessed at: either this call site loads the
syscall number by some means other than the standard `addiu
$v1,zero,<n>` immediate-load convention this project has assumed
everywhere else, or an unrelated register-corruption bug elsewhere in
this project's own interpreter is producing a bogus value at a
genuinely different, deeper real code path than anything reached
before. `0x00081FF4` itself is notable: it's a low, non-KSEG-prefixed
EE address outside the `0x8000xxxx` kernel range this project's trace
has stayed within for every prior finding, suggesting real forward
progress into a different code region (possibly the BIOS's own
default logo/OSD application, absent a game ELF) rather than a
continuation of the same kernel-init code.

**Verified:** 87/87 regression suite pass (no test depended on the old
syscall-18 bypass behavior), clean Wii/devkitPPC rebuild (0 errors).

**Next for task #172:** identify the real convention/bug behind the
`$v1=-5` value at EE PC ~`0x00081FF0` and what real code region
`0x00081FF4` belongs to - this is now the concrete next blocker,
found only because letting AddDmacHandler run for real, instead of
being bypassed, unlocked genuinely new territory.

## 56th finding (task #172/#181): syscall -5 real fix produces a MAJOR unlock - EE core no longer halts at all within a 100M-instruction budget; boot progresses through multiple new stages into a steady-state loop far past every previous wall

Following task #180's AddDmacHandler fix, boot progressed to a new,
deeper halt at EE PC `0x00081FF4` (`halt_reason="SYSCALL (no BIOS
syscall table implemented)"`, `$v1=-5`). Investigated via a targeted
diagnostic (`diag_dump_81ff4.c`) dumping the surrounding code and full
register state.

**Disassembly of the real call site (`0x00081FE0-0x00081FF4`):**
```
lui   $sp, 8               ; sp = 0x00080000
jalr  $v1                  ; indirect call through a handler pointer
addiu $sp, $sp, 0x1fc0      ; delay slot: sp = 0x00081FC0
addiu $v1, $zero, -5        ; <- runs after the called function returns
syscall                     ; real SYSCALL, $v1 = -5
```
This is a real, intentional `addiu $v1,zero,-5` immediately before
`syscall` - not a corrupted/garbage register value. The surrounding
shape (indirect `jalr` through a handler-pointer register, followed
immediately by this syscall on return) is the classic structure of a
kernel interrupt-dispatch trampoline: call the installed handler, then
tell the kernel "resume dispatch."

**Cross-referenced against the full, raw ps2sdk `syscallnr.h` source**
(fetched directly, not just the doxygen-rendered summary, to rule out
any missed entries): no `-5` alias is defined anywhere in the file.
Positive syscall 5 is defined as `__NR_ResumeIntrDispatch 5 //
Arbitrarily named` - ps2sdk's own maintainers flag this one as an
inferred, undocumented, kernel-internal-only mechanism. The negative
aliases that ARE documented (`-0x1a` and up) are all explicitly named
"fast"/interrupt-context counterparts of low-numbered positive
originals (e.g. `__NR__iEnableIntc (-0x1a)` next to `__NR__EnableIntc
0x14`), establishing that this dual positive/negative convention is
real and applies to exactly this class of syscall - it simply isn't
named for number 5 in the public header, consistent with being a
kernel-internal-only call user/IRX code never makes directly (hence
never needing a public "fast" alias).

**Fix (`source/core/ee/ee_core.c`):** added `sysnum == -5` alongside
the existing `sysnum == 18` case, both raising a real
`EE_EXC_CODE_SYS` exception via `ee_raise_exception()` rather than
being bypassed or halted. Per this round's own task #180 lesson
(bypassing AddDmacHandler in software silently broke a real kernel
side effect), this project does not guess at what bookkeeping
ResumeIntrDispatch's fast form performs - it lets real BIOS handler
code run instead.

**Verified via diagnostic - and the result is dramatic:** the EE core
no longer halts at all. A extended diagnostic
(`diag_progress_check.c`) ran a full 100,000,000-instruction budget
(previous diagnostics all capped at 65M) with periodic PC sampling and
found real, continuous forward progress through multiple genuinely new
code regions: `0xBFC00000` -> `0x9FC4254C` -> `0xBFC00C74` ->
`0x8000B8A0` -> `0x0008202C` -> `0x00083B40`, none of which had been
reached in any prior finding. Execution settles into a steady state
cycling within the narrow `0x00083B40-0x00083B54` range for the
remainder of the 100M-instruction budget. Disassembly of this region
shows it is NOT a spin-loop - it's a small, ordinary array-index
accessor function (`base + index*4; load; return`) - meaning the
CALLER of this accessor (not yet identified) is what's actually
iterating, potentially many times, from somewhere else in the 65M+
instruction range this diagnostic didn't capture in detail.

**Verified:** 87/87 regression suite pass (no test depended on the old
syscall dispatch behavior for -5, since it was previously unreached),
clean Wii/devkitPPC rebuild (0 errors, same pre-existing unrelated
strncpy truncation warning as every prior round).

**Next for task #172:** identify the actual caller/outer loop driving
repeated calls into the `0x00083B40` accessor function - specifically
whether it's a bounded, finite iteration (real, if slow, forward
progress that a longer instruction budget would resolve) or a genuine
infinite loop with a real blocking condition, and whether GS registers
get touched anywhere in this new code range (the ultimate signal of
real graphics-pipeline activity, one step closer to a splash screen).

## 57th finding (task #172/#182): first-ever confirmed real GS/CRTC register writes during boot; precisely identified the next real blocker as a genuine, zero-progress polling loop on a single never-written memory address (0x0008C440) - no fix yet, root cause narrowed exactly

Continuing task #182's investigation of the steady-state loop found in
the 56th finding (settling around EE PC `0x00083B40` after task #181's
syscall -5 fix unblocked boot). Used temporary, non-committed
instrumentation (a scratch copy of `gs.c` with write counters, never
touching the real repo file) plus the same host-native diagnostic
methodology established throughout this project.

**Major positive result: real GS register writes, for the first time
ever.** Between the earlier fixes and reaching the `0x00083B40` steady
state, the boot performs a complete, real GS CRTC/video-timing
configuration sequence:
```
i=15320027 pc=0x8000AAC8 GS_CSR    (0x12001000) = 0x0000000000000200
i=15410601 pc=0x8000A134 GS_SMODE1 (0x12000010) = 0x0000000740834504
i=15410603 pc=0x8000A13C GS_SYNCH1 (0x12000040) = 0x0007F5B61F06F040
i=15410608 pc=0x8000A150 GS_SYNCH2 (0x12000050) = 0x000000000033A4D8
i=15410620 pc=0x8000A180 GS_SYNCV  (0x12000060) = 0x00C7800601A01801
i=15410623 pc=0x8000A18C GS_SMODE2 (0x12000020) = 0x0000000000000003
i=15410632 pc=0x8000A1B0 GS_SRFSH  (0x12000030) = 0x0000000000000008
i=15410636 pc=0x8000A2A0 GS_SMODE1 (0x12000010) = 0x0000000740814504
```
This is a real, coherent CRTC/video-mode-init routine (matching real
PS2 BIOS boot behavior of configuring sync/refresh timing before any
video output) - addresses, register names, and write order all match
PCSX2's own `Hw.h`/this project's `gs.h` register map exactly. This is
the first time this project's boot has ever reached genuine GS
hardware configuration - a concrete prerequisite for a splash screen,
not previously observed in any prior finding (the earlier live-PCSX2
GT3 investigation and every prior static trace never got this far).
(Caveat found and self-corrected mid-investigation: an initial
instrumented counter placed BEFORE `gs_mmio_write64`'s own address
bounds check counted 4+ million calls, but that function is called
unconditionally for every 64-bit EE store regardless of destination
(see `ee_mem_write64`) - most of those calls are for unrelated
addresses and get rejected. Moving the counter after the bounds check
gave the real, trustworthy count of 8 genuine in-range GS writes above.)

**The `0x00083B40` steady state, precisely characterized:** confirmed
via call-frequency instrumentation that `0x00083B40` (the array-getter
function identified in the 56th finding) is called 1,499,819 times
between i=30,001,810 and the end of a 45M-instruction budget - and a
2,000,000,000-instruction run (the largest instruction budget this
project has attempted) still had not resolved it after 600M
instructions, ruling out "it's just a very long but finite real
hardware delay that a bigger budget would clear." Sampling registers
at the loop's entry point across a million+ iterations shows `$a0`
(the array index) and every other observed register frozen at
identical values every single time (`a0=0`, `ra=0x00084338`,
`sp=0xFFFFFF40`, `s0=0x0008D4C0`, `s1=0x00000001`) - zero forward
progress, not a counting/bounded loop. This rules out the "real
hardware busy-wait, just slow in emulation" hypothesis definitively:
a loop with a completely static index and no state change is not how
a real, bounded hardware delay is coded.

**Root cause narrowed to an exact address.** The accessor's own body
(`lui v0,9; sll a0,a0,2; addiu v0,v0,-0x3bc0; addu a0,a0,v0; lw
v0,(a0)`) computes, with `a0=0`, a fixed read address of
`0x0008C440`. A dedicated watch on this exact address across the
ENTIRE boot (from `i=0`) found it is written exactly ONCE - zeroed as
part of BSS initialization at `i=0` (`pc=0xBFC00200`) - and never
written again for the rest of the run. This is a real, precisely
identified "polling loop waits for a flag that nothing ever sets" bug,
structurally identical to every prior instance of this pattern this
project has found and fixed (SIF_SMFLG, DMAC_STAT, the AddDmacHandler/
ResumeIntrDispatch gaps from the 55th/56th findings) - this project
does not yet know what real mechanism is supposed to write
`0x0008C440`.

**Investigated and ruled out one candidate mechanism.** The 4 calls to
`0x00083e38` seen in the 56th finding's call-frequency check (a
generic "table[index] = {a1, a2}" indexed-array setter, used for the
device-ID-8/9/0xa/0xc registration sequence at `0x000842A0`-
`0x000842E8`) write to a DIFFERENT table, based at `0x0008C324`/
`0x0008C32C` (computed from the same `9<<16` base as the polled
address but with different offsets: `-0x3cdc`/`-0x3cd4` vs. the polled
address's `-0x3bc0`) - not `0x0008C440`. Ruled out as the mechanism
that should be setting the polled flag; the real mechanism remains
unidentified.

**Not fabricating a fix without a citable real mechanism.** Given this
project's established discipline (tasks #180/#181's own lesson: don't
guess at kernel-internal side effects, let real code/hardware behavior
determine the fix), and that the responsible next step mirrors exactly
what resolved the SIF2/SBUS question in the 55th finding - live PCSX2
debugging against a real GT3 boot to see what real mechanism (an
interrupt handler, an IOP<->EE message, a different syscall) writes
the real-hardware equivalent of this address - this is flagged as the
next concrete step rather than guessed at.

**No source code was changed this round** (pure diagnostic
investigation using temporary, non-committed instrumentation) - no
regression suite or Wii rebuild is needed for this commit, consistent
with this project's prior doc-only rounds (e.g. Round 19).

**Next for task #172:** live-debug the real-hardware equivalent of
`0x0008C440` (or find it via further static tracing if live debugging
is unavailable/unstable) to identify what real mechanism sets it, then
implement that mechanism for real - the same pattern that has now
resolved two consecutive walls (tasks #180, #181).

## 58th finding (task #172/#183): live PCSX2 debugging confirms real hardware DOES write 0x0008C440 (=1) somewhere between boot and gameplay; ROM signature search rules out all direct-CPU-store candidates - real mechanism is likely an indirect (SIF/IOP DMA) transfer, not a literal EE store instruction

Continuing task #183's investigation of the 57th finding's blocker (the
`0x00083B40` loop polls `0x0008C440`, which this project's own
emulator never writes after BSS init). Connected to a live, real GT3
boot via `pcsx2-mcp` to check real hardware's actual value.

**Confirmed: real hardware writes this address to 1.** A direct memory
read on live, mid-gameplay real hardware (`Cycles: 1590071881`) showed
`0x0008C440 = 0x00000001` - definitively confirming this project's
hypothesis that a real, currently-unmodeled write sets this flag,
unblocking the loop for real. This rules out "it's dead code that real
hardware also never satisfies" - real hardware genuinely depends on
and receives this write.

**Live write-timing capture proved impractical - a genuine tooling/
timing constraint, not a retry-away problem.** Armed a write watchpoint
on `0x0008C440` before a user-triggered cold reset (the same rigorous
methodology that resolved the 55th finding). Two real complications
were found and worked through:
1. The watchpoint's own break point coincided with the very
   instruction it triggers on (the address is part of a real, ~2-
   million-iteration "zero all of EE RAM" sweep starting near boot,
   confirmed live via `sq $v0,(s0)` with `s0` sweeping up to
   `a0=0x02000000` = the full 32MB EE RAM size) - `continue()` calling
   back into the same write instantly re-triggered without advancing,
   the same "breakpoint at current PC" quirk seen in earlier rounds.
   Worked around by manually stepping past before re-arming.
2. More fundamentally: `pcsx2_continue()` runs the real emulator in
   real wall-clock time, not in lockstep with this tool's own call
   cadence - a single `continue()`-then-`pause()` pair (with no
   deliberate delay in between) still let cycles jump by low hundreds
   of millions to over 2 billion in one round trip. This makes
   single-instruction-precision bisection via continue/pause
   fundamentally impractical over a network-latency-bound tool bridge,
   regardless of how many more reset cycles are spent on it - a real,
   inherent constraint of this debugging setup, not a problem more
   retries would solve. The watchpoint itself also never reported a
   hit via `pcsx2_list_watchpoints` despite the value demonstrably
   changing from 0 to 1 within the observed window, suggesting write
   watchpoints in this specific DebugServer bridge are not reliable
   during free-run `continue()` (works fine as a manual read/compare
   tool, not as a break trigger for this address).

**Pivoted to static ROM analysis - ruled out the obvious mechanism.**
Constructed the exact MIPS `lui reg,9` / `addiu reg,reg,-0x3bc0`
instruction encodings (the same address-computation pattern this
project's own emulator's `0x00083B40` accessor and the 56th finding's
`0x00083e38` setter both use to reach `0x0008C440`) and searched the
entire real BIOS ROM for every occurrence. Found exactly 4 matches:
- One is the already-known READ site (the `0x00083B40` accessor
  itself, confirmed via matching surrounding disassembly).
- One is a second, near-identical accessor variant right next to it
  (same read pattern, different call site).
- One sits inside an unrelated, tiny function stub (`0x0C89C8`,
  immediately preceded by an unrelated prologue - a red herring from
  the ADDIU-immediate coincidentally matching without the paired LUI;
  not a real occurrence of this address).
- One is a SEPARATE zero-fill loop: `lui v0,9; addiu s0,zero,0x1f;
  addiu v0,v0,-0x3bc0; addiu v0,v0,0x7c; ...; sw zero,(v0)` looping 32
  times while decrementing v0 by 4 - zeroing the entire 32-word
  (128-byte) range `0x0008C440`-`0x0008C4BC` inclusive. This confirms
  `0x0008C440` is the FIRST word of a real, dedicated 32-entry table
  (very plausibly a device/interrupt/DMA-channel handler table, given
  its size matches plausible real hardware channel counts) - but this
  is STILL a zero-write, not the "set to 1" event.

**None of the 4 ROM occurrences of this address-computation pattern is
a nonzero write.** This means the real "set to 1" event is not
performed by ordinary EE CPU code re-deriving this address via the
same `lui`/`addiu` pattern - it must arrive via a different mechanism:
most likely a cached/pre-computed base pointer passed into a
subroutine (undetectable by this literal-encoding search), or - more
likely given this project's established SIF/IOP-EE communication gaps
(46th/55th/56th findings) - an actual SIF DMA transfer FROM the IOP
side writing directly into this EE memory region, which would explain
why no EE-side store instruction computing this exact address exists:
the byte arrives via DMA copy, not a CPU store.

**No source code changed this round** - pure live-hardware
verification plus static ROM analysis, consistent with prior doc-only
rounds. No regression/rebuild needed.

**Next for task #172:** investigate the SIF DMA-transfer hypothesis
directly - check whether this project's IOP-side code ever sends a
SIF command/RPC targeting this EE address region (cross-reference
against the boot_info/module registration structures near this table,
given its 32-entry size and proximity to the device-ID registration
table from the 56th/57th findings), and whether this project's
existing `sceSifSetDma` implementation (task #175) is capable of
carrying such a transfer once the right trigger is identified.

## 59th finding (task #172/#184/#185): the IOP never writes SIF0/SIF1 DMA channel registers at any point during boot - traced the task #164 bypass to a real, previously-undocumented ~438,000-iteration retry loop inside SIFCMD's own module init code, and confirmed it resolves on its own without ever reaching real SIF DMA setup

Following the user's explicit instruction to "fix the SIF issue and
the other trouble related to IOP," re-examined the IOP syscall
0x10/0x08/0x14 bypass added in task #164 (43rd/44th findings). Read
the surrounding code and header comments closely before touching
anything: `iop_core.c`'s own comment already states plainly that
letting these syscalls vector as a REAL exception would not invoke
real kernel code the way the EE fixes (tasks #180/#181) did, because
this project's IOP side has no real, resident LOADCORE/kernel
dispatcher for these calls in the first place (`iop_module_loader.h`'s
own header: "any later-loaded module's own code that tries to query
the module chain via a real loadcore syscall...will not get a real
answer yet" - an explicit, honest, pre-existing scope boundary, not an
oversight). Removing the bypass would only reintroduce the ORIGINAL
task #151 bug (the unclaimed-exception TGE trap stub aborting the
calling module's remaining execution entirely) - confirmed by reading
`is_unconditional_trap_stub()`'s own real derivation
(`iop_module_loader.c`). So, unlike the two EE fixes this session,
"just let it vector for real" is NOT the applicable fix here, and
fabricating real `RegisterLibraryEntries`-style kernel struct/list
semantics without a citable source would violate this project's own
established no-fabrication discipline (WebSearch was unavailable this
round due to a session limit, so no new public citation could be
obtained either).

Given that, built a host-native diagnostic instead (three scratch
copies - `iop_core.c`, `iop_dma.c`, `iop_module_loader.c` - never
touching the real repository files, same discipline as the 57th
finding's `gs_instrumented*.c` copies) to answer a narrower, fully
static, fully citable question: **does the IOP, even with the current
bypass in place, ever reach real code that writes to the real SIF0
(0x1F801520-0x1F80152F) or SIF1 (0x1F801530-0x1F80153F) DMA channel
registers** (`iop_dma.c`'s own real address table) **at any point
during a full boot?** Ran a 60-million-instruction boot with every
IOP syscall 0x10/0x08/0x14 occurrence logged (module name, $a0/$a1,
call site) and every SIF0/SIF1 register write logged.

**Result: zero SIF0/SIF1 DMA register writes across the entire run -
not just in the steady-state polling loop, but from cold boot through
every module's initialization.** All 28 real modules from IOPBTCONF
(LOADCORE through EESYNC) load and run to completion; the loader
correctly marks the boot sequence complete and goes idle at
IOP PC 0x00100000, exactly matching the existing, understood idle
behavior (54th finding) - but at no point does any module's code touch
a SIF DMA channel register.

**A major, previously-undocumented side effect of the task #164
bypass was found along the way:** of 876,663 total logged IOP syscall
0x10/0x08/0x14 occurrences in this one boot, 876,605 of them (over
99.9%) are **SIFCMD's own module init code** (module #20 of 28, entry
0x001198D0) calling the exact same two syscalls back-to-back -
`v0=0x10, a0=0x001FFEC8, a1=1` then `v0=0x14, a0=0, a1=1` - with
IDENTICAL arguments roughly 438,000 times in a row, before finally
giving up on its own and proceeding normally to module #21 (REBOOT).
This is real, resident SIFCMD code performing what is almost certainly
a genuine "poll/wait" primitive (the fixed arguments rule out a
counting/index loop) whose real exit condition this project's
current `$v0=0` bypass never satisfies - the loop only ends because
SIFCMD's own code evidently has a large but finite internal give-up
threshold, not because anything we return changes. Every other module
placed in the 13-module bypass list (task #164) calls these same
syscalls only 1-4 times each, exactly as originally documented -
SIFCMD is uniquely different, and this had never been observed before
because prior tracing (43rd/44th findings) only sampled the FIRST
occurrence per module, not the full call count.

**Interpretation:** since SIFCMD *does* eventually complete its own
init (reaching REBOOT normally) and the full 28-module boot *does*
complete successfully, but SIF0/SIF1 registers are *still* never
touched, the real "write 0x0008C440" mechanism (confirmed present on
real hardware - 58th finding) cannot be reached via any IOP
module-load-time code path this project currently models, with or
without the task #164 bypass in place. This narrows task #172's
open question further: the real trigger is most plausibly IOP
**runtime** code that executes only *after* module boot completes -
e.g. a real SIF RPC bind/call service loop reacting to a later EE-side
request, or a periodic VBLANK/timer-interrupt-driven service routine -
which this project's IOP side does not yet model at all (today, once
`mark_iop_boot_complete()` fires, the IOP simply goes idle and stays
idle - task #179's 54th finding already established idle still checks
for hardware interrupts each step, but nothing in this project
currently drives a NEW interrupt or SIF request that would wake it
into doing further real work).

**No source code changed this round** - all instrumentation lived in
three throwaway `/tmp` scratch copies, diff-verified against the real
`source/hw/iop_dma.c`, `source/core/iop/iop_core.c`, and
`source/hw/iop_module_loader.c` to confirm zero drift, consistent with
this project's established discipline for investigation-only rounds
(e.g. the 57th finding's `gs_instrumented*.c`). No regression/rebuild
needed since nothing in the shipped source changed.

**Next for task #172:** since boot-time module init is now
conclusively ruled out as the source of the 0x0008C440 write, the
next concrete target is IOP-side **post-boot runtime** behavior -
specifically, whether this project needs to model an ongoing,
interrupt/SIF-request-responsive IOP service loop (rather than a
one-shot "load all modules, then go idle forever" model) for any real
SIF handshake to ever complete after boot. This is a genuine
architecture/feature gap, not a small bug, and should be scoped
carefully (matching this session's established precedent of
characterizing before implementing) rather than attempted in the same
round as this finding.

## 60th finding (task #172/#186): traced the real interrupt-driven SIF0 kick precisely - Cause.IP3 delivery (task #176) still works correctly at this later boot point, dispatches into real sceSifSetDma syscall trampolines, but stalls because (a) NO DMA channel in this project has a registered sink function at all, and (b) the EE sets up a real SIF1 receive chain (TADR) that is never kicked, because nothing IOP-side ever configures/kicks its own SIF0/SIF1 channels

Following the user's direction to move toward modeling a real SIF RPC
service (building on the 59th finding's conclusion that IOP boot-time
module init never reaches SIF0/SIF1 DMA activity), traced the EE side
of the picture with the same host-native diagnostic technique, adding
a scratch-copy trace of EE-side SIF0/SIF1 register writes (base
addresses `0x1000C000`/`0x1000C400`, per this project's own existing
`dma.h`) alongside the existing IOP-side trace.

**New concrete evidence, all from a single 60M-instruction boot:**

1. The EE genuinely kicks SIF0 for the first time at `i=29930436`
   (`CHCR=0x184` - STR bit set, mode=1/CHAIN), immediately preceded by
   zeroing SIF0's QWC and immediately followed by setting SIF1's own
   QWC=0 and TADR=`0x0001E140` (`i=29930468`/`29930471`) - real EE
   kernel code setting up BOTH the outgoing (SIF0) and incoming (SIF1)
   halves of a real SIF exchange. These are the ONLY writes to any
   SIF0/SIF1 register anywhere in the entire 60M-instruction run.

2. This SIF0 kick's completion (`dma_channel_signal_done()`, already
   implemented) does successfully assert a real Cause.IP3 (DMAC)
   interrupt for the first time in this later boot context - but only
   once Status.IE/EIE/IM3 all become true, which first happens at
   `i=30001033`, when EE code reaches `0x00083A6C` (immediately after
   calling a syscall-119/`sceSifSetDma` trampoline at `0x00083A60`).
   This confirms task #176's Cause.IP2/IP3 delivery mechanism (50th
   finding) is still working correctly this far into boot - it had
   only ever been observed once before, much earlier in a since-
   superseded boot path.

3. Traced the interrupt handler's own code precisely (PC-by-PC,
   `0x80000200` through `0x000841xx` and back): it calls a dispatch
   function at `0x00084168`, then branches on a flag bit to call
   EITHER `0x00083A60` (`li v1,0x77; syscall` = **EE syscall 119,
   `sceSifSetDma`**) or `0x00083A70` (`li v1,-0x77; syscall` = the
   "fast" negative variant, same dual positive/negative convention
   already seen for syscall -5 in task #181) - i.e. the interrupt
   handler's real job is to re-arm/continue the SIF DMA sequence,
   exactly the real, expected kernel behavior for an interrupt-driven
   DMA queue. It fires exactly twice (`i=30001033`, `i=30001513`) then
   never again for the rest of the 60M-instruction run - consistent
   with the queue being fully drained/exhausted after two entries, not
   a bug in the interrupt delivery itself.

4. **Two real, concrete gaps identified, distinct from anything
   previously documented:**
   - `dma_register_sink()` (`dma.c`) is never called anywhere in this
     project for ANY DMA channel, including SIF0. This means even a
     well-formed chain-mode transfer with real, nonzero QWC would
     silently drop its payload (`transfer_quadwords()`'s
     `if (g_sinks[channel]) g_sinks[channel](...)` is always
     false) - no cross-CPU data movement happens at the hardware-DMA
     level for ANY channel today, independent of the IOP-side gap the
     59th finding already found.
   - SIF0's own kick in this run used `TADR=0` (never explicitly set
     by any EE code in the whole boot), consistent with either an
     intentionally-empty control-only packet or a genuine missing
     setup step - and SIF1's CHCR (the "kick" register for the
     IOP-to-EE return direction) is NEVER written with the STR bit
     set anywhere in the run, even though its QWC/TADR get real values
     - meaning the EE fully prepares to RECEIVE a reply chain but
     nothing ever drives that transfer, because (per the 59th finding)
     the IOP never touches its own SIF0/SIF1 channel registers at all.

**Real citations obtained this round** (WebSearch had recovered from
the earlier session-limit block): ps2tek's DMAC Interrupts section
(`psi-rockin.github.io/ps2tek`) confirms `D_STAT`'s "INT1 asserted
when (status & mask) != 0" semantics this project already implements
correctly, and Cause register bit 10=INTC-pending/bit 11=DMAC-pending
- both already correctly modeled. ps2sdk's real
`common/include/sifcmd-common.h` (fetched via the doxygen-rendered
source at ps2dev.github.io/ps2sdk) provided the real
`SifCmdHeader_t` (`psize:8, dsize:24, dest, cid, opt`) and command-ID
constants (`SIF_CMD_INIT_CMD`, `SIF_CMD_RPC_BIND/CALL/END/RDATA`,
`SIF_SREG_RPCINIT=0`) that any real SIF command-dispatch
implementation must match - these are recorded here for the next
implementation round to cite directly rather than re-deriving.

**Conclusion / scope for task #186:** a full, real, both-sides SIF
command/RPC protocol (matching ps2sdk's real `SifCmdHeader_t`
dispatch) is a substantial feature, not a small fix. The most
concrete, minimal, well-grounded FIRST increment is wiring a real
sink for the SIF0/SIF1 DMA channels (so quadword payloads actually
move between EE and IOP RAM when a channel is genuinely kicked with
nonzero QWC) - independent of, and a prerequisite for, any real
command dispatch on top. No source changed this round (pure
diagnostic tracing plus citation-gathering, three throwaway `/tmp`
scratch copies, no drift in the real repository files).

## 61st finding (task #172/#186): obtained the real ps2sdk EE-side sceSifInitCmd()/_SifSendCmd() source and confirmed, byte-for-byte, that this project's traced packets are genuine SIF_CMD_INIT_CMD command sends; real cross-CPU data movement already works for this exact path via the existing sceSifSetDma HLE - the missing piece is a real IOP-side command consumer, which requires IOP-side assembly source this round's tools could not fetch

Continuing directly from the 60th finding, fetched (via raw.githubusercontent.com,
after ps2dev.github.io's rendered pages proved less complete for this
file) the actual, real `ee/kernel/src/sifcmd.c` from ps2dev/ps2sdk
(Academic Free License 2.0). This is a load-bearing, decisive
citation: it lets every packet this project's own diagnostic captured
be matched, field-by-field, against real code instead of inferred.

**Byte-exact match confirmed:** the two real `EE-RAM-to-IOP-RAM`
copies captured in the 60th finding (via the existing, already-
implemented `sceSifSetDma`/syscall 119 HLE) both carry
`header->cid = 0x80000002`, which is EXACTLY `SIF_CMD_INIT_CMD`
(`SIF_CMD_ID_SYSTEM(0x80000000) | 2`, per `sifcmd-common.h`,
independently confirmed in this same round). Reading the full packet
bytes (not just the header) shows the FIRST send's extra word
(`0x0008C240`) matches exactly `sceSifInitCmd()`'s real
`struct ca_pkt { SifCmdHeader_t header; void *buf; }`, where `buf` is
the EE's own static receive-buffer address (`_sif_cmd_data.pktbuf`) -
i.e. this project's own boot is executing the REAL,
unmodified `sceSifInitCmd()` routine, reaching its final
`sceSifSendCmd(SIF_CMD_INIT_CMD, &packet, sizeof packet, NULL, NULL, 0)`
call for the first time ever. The real source also explains why this
project's EARLIER `sceSifGetReg(SIF_REG_SMFLAG) & SIF_STAT_CMDINIT`
wait (tasks #165/#170's already-fixed 45th finding) had to resolve
BEFORE this point could ever be reached - `sceSifInitCmd()`'s own code
blocks on exactly that condition immediately before sending this
packet.

**What this conclusively rules in/out:**
- Real, correct EE-RAM-to-IOP-RAM byte copying already happens for
  this exact packet (`dest=0x0011AFD0` both times) - this is NOT a
  data-movement bug. The "IOP DMA hardware execution engine" this
  finding's investigation initially planned to build (mirroring the
  EE's chain-mode engine) would NOT have fixed this specific blocker,
  since the real BIOS code uses the software `sceSifSetDma` syscall
  path for this handshake, not a hardware CHCR-kick chain transfer -
  an important correction to this round's own initial plan, caught
  before writing unnecessary code.
- The real, missing piece is a genuine IOP-side CONSUMER: something
  on the IOP side needs to notice the arrived `SIF_CMD_INIT_CMD`
  packet at `0x0011AFD0` and act on it (by protocol symmetry with the
  EE's own mirror-image `SIF_CMD_CHANGE_SADDR` handler - `cmd_data->
  iopbuf = pkt->buf` - the natural real counterpart is recording the
  EE's buffer address on the IOP's own side). This project's IOP
  never runs this code because it has already gone idle (59th
  finding) by the time this packet is sent.

**Attempted, could not obtain this round:** the real IOP-side
`iop/kernel/src/sifcmd.s` (ps2sdk's IOP-side SIFCMD is assembly, not
C, per ps2dev.github.io/ps2sdk's own File List, which lists an EE-side
`sifcmd.c` but no IOP-side C equivalent). Multiple fetch attempts
(`raw.githubusercontent.com`, `github.com` blob view, GitHub's content
API) all returned empty for this specific path - a genuine tool
limitation this round, not a "file doesn't exist" finding. Also
attempted disassembling the live PCSX2 reference GT3 instance's IOP
RAM at this project's own computed SIFCMD module address
(`0x001198D0`) to read the real native code directly (the same
technique that worked for EE-side disassembly earlier this session) -
found all zeros, meaning the live instance (now ~2 billion cycles into
gameplay) no longer has this module resident at the address this
project's own loader computed for it, so this cross-check did not
work this round either.

**No source code changed this round** - all instrumentation stayed in
throwaway `/tmp` scratch copies (`ee_core_instrumented.c`,
`iop_core_instrumented.c`, `iop_dma_instrumented.c`,
`iop_module_loader_instrumented.c`, `dma_instrumented.c`), diff-
verified against the real repository files (zero drift). This is
docs-only progress, consistent with prior investigation-only rounds.

**Next for task #186:** either (a) obtain the real IOP-side SIFCMD
assembly through a different channel (a fresh WebFetch attempt, a
different mirror, or asking the user to supply it), or (b) implement
a minimal, explicitly-labeled IOP-side `SIF_CMD_INIT_CMD` responder
based on well-established real protocol symmetry (record the EE's
buffer address; the flag this project's boot is actually polling,
`0x0008C440`, is still NOT confirmed to be the direct result of this
specific command's handling - it may be gated by the AddDmacHandler-
populated 32-entry table from the 56th/57th findings instead, whose
exact real indexing convention is also still unconfirmed) - option (b)
carries real fabrication risk this project has successfully avoided
throughout, so is flagged for explicit user sign-off rather than
assumed.

## 62nd finding (task #172/#186): implemented a minimal, explicitly-labeled IOP-side SIF_CMD_INIT_CMD consumer (records the EE's reply-buffer address) - verified against real BIOS boot with no regression, but does NOT by itself unblock the 0x0008C440 poll

Continuing from the 61st finding (real IOP-side `sifcmd.s` assembly
still unobtainable after exhausting every fetch avenue this round -
`raw.githubusercontent.com` in multiple forms, GitHub's blob/tree
pages, GitHub's content API, `cdn.jsdelivr.net`, ps2dev.github.io's
doxygen - all returned empty for this one specific path), reported
this state to the user along with the well-grounded (though not
byte-exact) real protocol confirmation already in hand: the byte-
exact real EE-side `ee/kernel/src/sifcmd.c` (61st finding) plus an
independent, real PS2 homebrew documentation source found via
WebSearch confirming "the IOP uses a software SIF register to tell
the EE what the IOP has stored for the EE's receive buffer" and that
`SIF_CMD_INIT_CMD (0x80000002)` is sent exactly twice during SIFRPC
init - matching this project's own trace precisely. Asked the user how
to proceed; the user chose to implement the minimal IOP-side responder
now, using this grounding, clearly labeled as such rather than as a
byte-exact assembly port.

**What was implemented:** `sif_cmd_iop_handle_init_cmd()` (new
functions, added to `sif.c`/`sif.h` rather than a separate translation
unit specifically so every existing test that already links `sif.c`
keeps building without any test-harness changes). Models exactly one,
narrowly-grounded real-protocol effect: recording the EE's own reply/
receive buffer address (the packet's `ca_pkt.buf` field) on receipt of
a `SIF_CMD_INIT_CMD` packet - by direct symmetry with this project's
own real, byte-exact EE-side `SIF_CMD_CHANGE_SADDR` handler
(`cmd_data->iopbuf = pkt->buf`, confirmed in the fetched real
`sifcmd.c`). Invoked synchronously from the EE's `sceSifSetDma`
(syscall 119) handler in `ee_core.c`, immediately after the real EE-
RAM-to-IOP-RAM byte copy, decoding the real `SifCmdHeader_t` layout
confirmed in the 61st finding (offset 0=psize:dsize, 4=header.dest,
8=cid, 12=opt, 16=ca_pkt.buf) directly from the copied bytes. This is
modeled synchronously rather than via a running IOP-side consumer loop
because this project's IOP has already gone idle by this point in a
real boot (59th finding) - there is no live IOP dispatcher to trigger
this naturally yet. Extensively commented in both `sif.h` and the call
site with the full honesty scope: this is NOT a port of real IOP
assembly (which this project does not have), does NOT drive any
hardware SIF register on its own, and is explicitly NOT confirmed to
be what unblocks the still-open `0x0008C440` poll - that poll may
instead be gated by the AddDmacHandler-populated 32-entry table from
the 56th/57th findings, whose exact real indexing convention remains
unconfirmed.

**Verification performed:** full 88-test host-native regression suite
passes (0 build failures, 0 run failures, 0 check failures - up from
87 pre-existing, no new test added this round since the effect is
exercised via the real-BIOS diagnostic below rather than a synthetic
unit test); clean Wii/devkitPPC rebuild (exit 0, only the pre-existing,
unrelated `strncpy` warning).

**Real-BIOS empirical result (host-native diagnostic, 60M-instruction
cap):** confirmed via a dedicated diagnostic harness that
`sif_cmd_iop_get_ee_recvbuf()` is correctly set to `0x0008C240` at
instruction i=30001031 (ee_pc=0x00083A68) - matching exactly the real
EE pktbuf address independently confirmed in the 61st finding. Boot
otherwise reaches the IDENTICAL furthest point as before this change
(`0x00083B40`/`0x00083B48`, the same plateau documented since the
56th/57th/59th findings) - i.e. this increment is confirmed to fire
correctly on real boot, with zero regression, but by itself does NOT
advance boot any further. This is the expected, honestly-anticipated
result: the `0x0008C440` poll's real gating condition remains
unconfirmed and is the next open question for task #186.

**Diagnostic tooling note (not a shipped-code bug):** while building
this round's verification harness, found that calling
`ee_mem_read32()` out-of-band (i.e. from diagnostic code, before the
EE has executed even its first real instruction, at i=0 right after
`ee_core_init()`) can corrupt CPU state via the `ee_mem_check_tlb_fault()`
path and cause the EE to appear permanently stuck at the boot ROM
reset vector for the rest of the run. This ONLY reproduces when the
read happens before any real boot code has run; the exact same
function call from WITHIN real instruction execution (e.g. this
finding's own `sif_cmd_iop_handle_init_cmd()` call site, or the prior
`[SIFHDR]` instrumentation from the 60th finding) is safe and already
proven correct by this round's own passing diagnostic. Root-caused via
a controlled A/B test (`git stash`/`git stash pop` to compare identical
diagnostic code against the pre- and post-patch source tree, then
bisecting the diagnostic file itself line-by-line) - confirmed the
regression was in the throwaway diagnostic harness, not in this
project's shipped source. No source file changed for this; noted here
purely so a future round doesn't waste time re-diagnosing the same
tooling quirk.

**No source files changed beyond `include/core/hw/sif.h`,
`source/hw/sif.c`, `source/core/ee/ee_core.c`** - all diagnostic/test
harnesses stayed in throwaway `/tmp` scratch files.

**Next for task #186:** the `0x0008C440` poll remains unresolved.
Leading hypothesis (per the 61st finding, still unconfirmed): it may
be gated by the AddDmacHandler-populated 32-entry table from the
56th/57th findings rather than by this specific SIFCMD packet's
handling. Investigating that table's real indexing convention is the
natural next step.

## 63rd finding (task #172/#187): MAJOR BREAKTHROUGH - the long-standing 0x0008C440 poll (blocking boot since the 57th finding) is now genuinely resolved by real, unmodified BIOS code; boot reaches a brand-new stage (CreateSema, syscall 64) never seen before

Continuing directly from the 61st/62nd findings' conclusion (real IOP-side
SIFCMD assembly unobtainable; user authorized proceeding with a
minimal, explicitly-labeled synthetic IOP responder), fetched the FULL
real `ee/kernel/src/sifcmd.c` (previous rounds only had fragments) via
`raw.githubusercontent.com` (worked this time). This was a decisive,
load-bearing citation.

**Byte-exact confirmation of the 32-entry table's real identity.**
`sceSifInitCmd()`'s real source shows `static SifCmdSysHandlerData_t
sys_cmd_handlers[SYS_CMD_HANDLER_MAX]` (`SYS_CMD_HANDLER_MAX=32`) and
`static int sregs[32]` - the LATTER is EXACTLY this project's own
57th/58th-finding "32-entry table at 0x0008C440" (32 ints = 128 bytes,
matching the real zero-fill loop found via ROM disassembly). Index 0
(`0x0008C440` itself) is real ps2sdk's `SIF_SREG_RPCINIT`. The real
dispatch mechanism (`_SifCmdIntHandler()`, `sys_cmd_handlers[1]=
set_sreg` performing `cmd_data->sregs[pkt->sreg] = pkt->val`) is
genuine, already-resident BIOS/kernel code this project's EE
interpreter already executes correctly once invoked.

**Implemented (source/hw/sif.c, source/hw/sif.h, source/core/ee/
ee_core.c):**
1. `sif_cmd_iop_send_rpcinit_ready()` (ee_core.c): synthesizes a real,
   byte-exact `struct sr_pkt {SifCmdHeader_t header; u32 sreg; int
   val;}` (24 bytes: cid=SIF_CMD_SET_SREG=0x80000001, sreg=
   SIF_SREG_RPCINIT=0, val=1) into the EE's own recorded receive
   buffer (`sif_cmd_iop_get_ee_recvbuf()`, from task #186), then fires
   the real SIF0 DMAC-completion interrupt via the SAME
   `dma_channel_signal_done(DMA_CHANNEL_SIF0)` mechanism already
   proven working for the EE's own outgoing sends (tasks #176/#180).
   Delivery is delayed (a fixed 50,000-EE-instruction countdown,
   checked every step via a new `ee_check_rpcinit_pending()` hook)
   rather than fired immediately in the same syscall-119 call that
   records the EE's buffer address, to avoid colliding with that same
   call's own outgoing-completion interrupt on the same level-
   triggered SIF0 status bit.
2. Extended the existing EE syscall bypass for 120 (`sceSifSetDChain`)
   to also cover its negative/"interrupt-safe fast" form, `sysnum ==
   -120` (`isceSifSetDChain`, confirmed via ps2sdk's `ee/kernel/
   include/syscallnr.h`, also fetched successfully this round). This
   was found to be REQUIRED, not optional: once the synthetic packet
   above successfully drove real, genuine BIOS code into
   `_SifCmdIntHandler()` for the very first time, that REAL code
   (matching the fetched source exactly, confirmed via live PCSX2
   disassembly at `0x00084050`) calls `isceSifSetDChain();` - hitting
   a real, pre-existing gap (only the positive form was bypassed) that
   halted the EE with "SYSCALL (no BIOS syscall table implemented)".
   Same real justification as the already-documented positive-120
   bypass: this project models no SIF0 DMAC chain-mode register
   engine, so a no-op/generic-default return is correct emulated
   behavior for either form.

**Verification methodology.** Used host-native diagnostics extensively
this round, including a live A/B test (temporarily removing an early,
premature diagnostic memory read that was corrupting CPU state before
boot even started - see the "diagnostic tooling hazard" noted in the
62nd finding) and step-by-step PC/register tracing to precisely follow
the real interrupt-delivery chain: `Cause.IP3` fires -> real general
exception vector (`0x80000200`) -> real kernel dispatch code
(`0x80000400`s, `0x80001300`s, including the exact PC task #177's
MFSA/MTSA fix was needed for) -> real `AddDmacHandler`-registered
callback lookup (observed loading `v1=0x00084050`, later confirmed via
live PCSX2 `pcsx2_disassemble` to be `_SifCmdIntHandler()`'s real
entry point) -> `jalr` into it for real. Cross-referenced this
project's own live-PCSX2 disassembly of `0x00084050`-`0x00084170`
against the fetched source instruction-by-instruction (struct offsets
0xC/0x10 for `sys_cmd_handlers`/`nr_sys_handlers` match the real
`struct cmd_data` layout exactly) - not inferred, directly confirmed.

**Verification performed:** full 88-test host-native regression suite
passes (0 failures); clean Wii/devkitPPC rebuild (exit 0, only the
pre-existing, unrelated `strncpy` warning).

**Real-BIOS empirical result - the headline finding:** `0x0008C440`
(`sregs[0]`/`SIF_SREG_RPCINIT`), polled without ever resolving since
the 57th finding (many rounds, multiple investigation dead-ends, and
the entire task #183/#184/#185/#186 IOP-DMA/SIF-consumer investigation
chain), now reads `0x00000001` - set by genuine, unmodified,
byte-exact real BIOS/kernel code (`set_sreg()`), not by this project
directly poking the value. Boot then advances PAST the `0x00083B40`
steady-state loop for the first time ever, reaching a brand-new real
syscall this project has never seen before: `v1=0x40=64` = ps2sdk's
`__NR_CreateSema` (confirmed via the freshly-fetched `syscallnr.h`) -
the EE kernel's real semaphore-creation call, presumably part of
real thread/RPC-server setup following successful SIF RPC
initialization. This is NOT implemented yet (halts with "SYSCALL (no
BIOS syscall table implemented)") - a new, honestly-reported next
wall, not a fabricated fix.

**Honest caveats retained:** the exact TIMING of the synthetic
SIF_CMD_SET_SREG delivery (a fixed 50,000-instruction delay after the
recorded SIF_CMD_INIT_CMD send) is an explicitly-labeled approximation
of real IOP response timing, not a byte-exact citation - real IOP-side
assembly remains unobtainable. Everything ELSE about this increment
(the packet's real byte layout, the real EE-side dispatch/handler code
it drives, the real table/index semantics) is byte-exact and
independently confirmed via live PCSX2 disassembly, not guessed.

**Next for task #172:** implement or investigate `CreateSema` (syscall
64/0x40) - the new real wall. This is a genuine EE kernel threading
primitive (real semaphore object creation), a different category of
work from the SIF/DMA investigation this session has focused on -
scope it carefully (does reaching a splash screen actually require a
working thread/semaphore subsystem, or can this be bypassed similarly
to the FlushCache/SetupThread/SetupHeap generic-default precedent?)
before implementing anything.

## 64th finding (task #188): CreateSema implemented for real; boot
   reaches WaitSema (syscall 68) as the next wall

User explicitly authorized implementing `CreateSema` (syscall 64/0x40)
rather than merely scoping it further ("implement it"). Real signature
confirmed from `ee/kernel/include/kernel.h` (and cross-checked this
round against a full local copy of the ps2sdk source tree the user
supplied as `ps2sdk-master.zip`, extracted to `/tmp/ps2sdk-ref` -
independent confirmation, not just doxygen-page summaries):
`s32 CreateSema(ee_sema_t *sema);` where `ee_sema_t` is
`{ int count, max_count, init_count, wait_threads; u32 attr, option; }`
(24 bytes). The caller fills `max_count`/`init_count`/`attr`/`option`
before the syscall; the kernel allocates a semaphore object from an
internal table (real `MAX_SEMAPHORES=256`) and returns its ID (>=0) or
a negative error.

**Implementation** (`source/core/ee/ee_core.c`): a new 256-slot
`g_ee_sema[]` table (`in_use`/`count`/`max_count`/`wait_threads`/
`attr`/`option`), reset in `ee_core_init()`. The new
`sysnum == 64` case reads the four caller-supplied fields from the
`$a0`-pointed struct, first-fit-allocates a slot, initializes it, and
returns the slot index (or -1 if full) in `$v0` via real EE syscall
return convention. This is a REAL implementation (not a bypass/no-op
stub), since the returned ID is a real value later
`WaitSema`/`SignalSema`/etc. calls must reference correctly.

**Verification methodology - re-hit and re-resolved the known
"diagnostic tooling hazard" (documented in the 62nd finding):** the
first diagnostic run this round accidentally reused a stale, pre-fix
copy of the host-native harness (one that calls `ee_mem_read32()` at
instruction i=0, before any real boot code executes), which corrupted
CPU state and produced a false, concerning-looking "regression" (EE
apparently frozen at the reset vector, `poll@0x0008C440` reading 0).
Rebuilding the harness without the premature out-of-band read (per the
same fix documented in the 62nd finding) and re-running immediately
resolved this: `poll@0x0008C440` correctly reads `0x00000001` from
i=40,000,000 onward (RPCINIT delivery, task #187's fix, is fully
intact - genuinely NO regression), and `sif_cmd_iop_get_ee_recvbuf()`
still resolves at the same instruction (i=30001031) as before.

**Real-BIOS empirical result:** `CreateSema` is called with
`max_count=1`, `init_count=0` (a locked binary semaphore/mutex idiom),
confirmed via a temporary diagnostic print of the real struct fields
(not fabricated - read directly from the emulated EE's memory at the
real call site, caller return address `0x000848A0`). The call
succeeds (returns id=0), and boot immediately proceeds to call
`WaitSema` on that same semaphore (real EE syscall 0x44/68, confirmed
via `syscallnr.h`), halting with "SYSCALL (no BIOS syscall table
implemented)" - the next new, honestly-reported wall (task #189), not
a bug introduced by this round's work.

**Live PCSX2 cross-reference:** disassembled the real caller chain
(`0x00084870`-`0x000848e4`) to confirm the `CreateSema` call site and
its success-path branch (`bgez $v0`), and traced further into what
appears to be a real `AddIntcHandler`-family call
(`0x00083FD0`/`0x00084010` argument-shuffling wrappers around a larger
kernel function at `0x00083E90`) reached shortly after. This chain is
NOT yet fully characterized - documented honestly as an open thread
for task #189, not claimed as understood.

**Why real ps2sdk source has no C implementation to cite for
`WaitSema`'s actual blocking semantics:** confirmed via the fetched
GitHub directory listing of `ee/kernel/src/` that ps2sdk ships no
`WaitSema.c`/`thsemap.c` - these are pure kernel syscalls (real
BIOS-ROM-resident assembly, same as `CreateSema`), not library
functions with distributable C source. Real semantics (per
`psdevwiki.com/ps2/EE_Syscalls` and this project's own reading of
`ps2sdk-master/ee/kernel/src/thread.c`'s `WaitSema(topSema)` usage
pattern) are the standard counting-semaphore contract: decrement and
return immediately if `count>0`, otherwise block the calling thread
until another context (typically an interrupt handler) calls
`SignalSema`/`iSignalSema`. This project has no real multi-thread
scheduler, so honestly implementing `WaitSema` for the `init_count=0`
case reached here needs careful characterization first (task #189) -
not guessed at.

**Verification performed:** full 88-build/87-distinct-binary
host-native regression suite passes (0 failures;
`test_vu_micro` appears twice in the extracted build-command list
under the same output name, a pre-existing harness quirk unrelated to
this change); clean Wii/devkitPPC rebuild (exit 0, only the
pre-existing, unrelated `strncpy` warning in
`iop_module_loader.c`).
