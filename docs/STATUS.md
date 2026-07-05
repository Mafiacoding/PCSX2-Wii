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
