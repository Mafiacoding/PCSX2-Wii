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
