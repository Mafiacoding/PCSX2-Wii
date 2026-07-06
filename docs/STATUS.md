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
