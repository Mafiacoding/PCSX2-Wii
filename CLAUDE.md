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

## Current frontier: the "EE JALR investigation" (rounds 1-14, and counting) + GS rasterizer work

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
- **Round 8 - Scratchpad RAM + COP0 Count fixed via a second live
  trace**: round 7's "wired TLB entry" guess (below, kept for context)
  turned out to be more specific than that. A live PCSX2 trace (same
  bridge as round 5) showed the fault address (`$sp=0x70003FC0`) is
  inside the R5900's real Scratchpad RAM (SPR) - a fixed 16KB window
  (`0x70000000-0x70003FFF`) that real hardware bypasses the TLB for
  *entirely*, confirmed against PCSX2's own source (`Memory.cpp`'s
  "scratch pad" comment, `MemoryTypes.h`'s 16KB `Ps2MemSize::Scratch`,
  `COP0.cpp`'s `isSPR()`-gated direct-buffer mapping). Fixed by
  intercepting this fixed range in `ee_mem_ptr()` *before* any TLB
  lookup, routing to a new `scratch[16*1024]` buffer. A second wall
  immediately followed: COP0 Count (`cop0[9]`) never advanced on its
  own, hanging a real BIOS delay loop - fixed by incrementing it by 1
  per instruction (a real, working free-running counter, just without
  precise bus-clock-rate timing fidelity this project has no cycle-
  accurate model to derive). With both fixes, an 800M-instruction run
  raises **zero exceptions** (down from 2-then-stuck-forever) and
  reaches a new region, `pc=0xBFC0092C` - a real `j $` self-loop right
  after a `Compare=1` timer setup: a genuine "wait for interrupt" idle
  pattern, not a bug.
- **Round 9 - real EE Timer (Count==Compare) interrupt delivery
  implemented**: `ee_latch_timer_interrupt()`/`ee_check_timer_interrupt()`
  in `ee_core.c`, ported from PCSX2's `_cpuTestTIMR()`/
  `cpuTestTIMRInts()`. Cause.IP7 latches on `Count>=Compare` (NOT `==` -
  a live real-BIOS instruction sequence at `pc=0xBFC0081C-0xBFC00824`
  proved exact equality gets silently overshot: `MTC0 Count,0` then,
  two instructions later, `MTC0 Compare,1` - Count is already past 1 by
  the time Compare is written, since Count advances every instruction
  including the epilogues in between). Sticky until an explicit Compare
  write acks it (real, documented MIPS behavior). Taking the interrupt
  (not just latching it) is deferred across branch/delay-slot
  boundaries - splitting these two concerns was itself a real bug fix
  during this round (an exact-equality-only draft could silently lose
  a match that landed on a taken branch's own step). 32 host-native
  checks (`tests/test_ee_timer_interrupt.c`), 0 regressions across the
  full 34-file suite, clean Wii rebuild.
- **Round 10 - idle loop confirmed dead code on real hardware; root
  cause traced to a subroutine returning `v0=-1` instead of real
  hardware's `0x08028020`** at `pc=0x9FC410E8` (a new `pcsx2-mcp` MCP
  connector gave direct live PCSX2 access - breakpoints/registers/
  disasm/memory - see the tool note below). Live-traced 3+ levels deep
  and initially attributed to a "SIO baud-calibration loop" polling
  `0x1000F430`/`0x1000F440` - see docs/STATUS.md's "round 10" section
  for the full trace (this framing was itself corrected in round 11
  below).
- **Round 11 - FIXED**: verifying round 10's register addresses against
  a citable reference (PCSX2's own `Hw.h` + PS2Tek) showed they're
  **MCH_RICM/MCH_DRD** (RDRAM auto-init), not SIO. Implemented per that
  reference (`source/hw/mch.c`). Also found and fixed a deeper bug:
  hardware-register MMIO dispatch compared the raw unmasked address
  against physical-style constants, so it never matched real KSEG0/1-
  addressed accesses - added `ee_hw_mmio_addr()` to mask first.
  **Live-verified**: `0x9FC410E8` now returns `v0=0x08028020` exactly
  matching real hardware, `pc=0xBFC0088C` shows `v0=0x02000000` exactly
  matching the round 6 report, `pc=0xBFC0092C`'s idle loop is never
  reached. Step count to the fix (14.93M) closely matches round 10's
  live-traced ~14.9M real cycles. Also added `DADDI`/`DADDIU`. 29 new
  checks, 37-file/0-failure regression. New wall: unimplemented COP2
  opcode at `pc=0x8000B1FC`.
- **Round 12 - COP2 (VU0 macro mode) control-register transfers
  implemented**: the round 11 wall was `cfc2 v0,FBRST` / `ori v0,0x200`
  / `ctc2 v0,FBRST` - a real BIOS read-modify-write resetting VU1
  (FBRST bits verified against PCSX2's own `VU0.cpp` `CTC2()`: 0x1/0x2
  = VU0 break/reset, 0x100/0x200 = VU1 break/reset). Implemented a new
  `cop2_ctrl[32]` register file plus `MFC2`/`CFC2`/`MTC2`/`CTC2`
  dispatch (primary opcode 0x12) as plain storage - no VU0/VU1
  execution state exists yet to act on the real reset/break side
  effects (same honest-simplification pattern as SIF's CTRL register).
  **Live-verified**: correctly handles a second, different real use
  right after (VU0 integer register 1, same CFC2/CTC2 family) with no
  issues - not a lucky FBRST-only match. **New, confirmed-genuine
  wall**: `viswr`, a real VU0 vector-datapath instruction (dispatched
  via the 6-bit `funct` field once `rs`'s top bit is set, like COP0/
  COP1's own "CO"-format) - a separate, much larger subsystem (32x
  128-bit VF registers, 16 VI registers, the full VU macro arithmetic
  family) intentionally left unimplemented rather than half-done. 4
  new checks, 38-file/0-failure regression. See `docs/STATUS.md`'s
  "round 12" section for the full trace (harnesses in `/tmp/diag/` on
  the machine that did this work - throwaway, not committed).
- **Round 13 - VU0 vector datapath implemented**: cleared round 12's
  `viswr` wall for good. Added the real VF register file
  (`vu0_vf[32][4]`, 128 bits each as 4 raw 32-bit lanes) and VU0's 4KB
  local data memory (`vu0_mem[4096]`), plus `QMFC2`/`QMTC2` (128-bit
  GPR<->VF transfers), `VSUB` (vector float subtract with per-lane dest
  masking), `VISWR`/`VSQI` (VU0-mem stores - dispatched through a
  second-level "SPECIAL2" sub-table whose index formula and fixed `fd`
  opcode-select values were derived from a live PCSX2 disassembly and
  cross-checked against PCSX2's own `R5900OpcodeTables.cpp`), and
  `VIADD`/`VISUB`/`VIAND`/`VIOR` (VI integer ALU) for the bulk
  register-clear routine right after. VF00/VI0 hardwired like real
  hardware (writes discarded). **New wall found immediately after**:
  `unimplemented primary opcode 0x1A` (LDL) - added `LDL`/`LDR`/`SDL`/
  `SDR` (the 64-bit doubleword analog of this project's existing
  LWL/LWR/SWL/SWR, standard MIPS III, no live verification needed).
  **Live-verified**: the interpreter now runs past 300M steps with no
  halt at all (was ~15.4M before this round), settling into a bounded
  ~0x420-byte loop that a live PCSX2 disassembly confirms is an
  ordinary `SIF_SMFLG` (0x1000F230) polling/debounce pattern - an
  honest steady state given this project's minimal IOP/SIF HLE model
  on a disc-less boot, not a new bug. 16 new checks across two test
  files, 40-file/0-failure regression. See `docs/STATUS.md`'s "round
  13" section for the full trace.
- **Round 14 - IOP-side investigation**: running the EE+IOP
  interleaved (not EE-only, like round 13's own diagnostic) confirms
  round 13's EE steady state is real, and finds the IOP hits its own
  genuine wall almost immediately: a live-traced `JALR $ra,$s1` with
  `$s1` holding `0x03400008` - an address only a real IOP module/IRX
  loader would ever populate (this project's `iop_hle_modules.c` is an
  explicit scaffold, not a real loader - not fabricated further, same
  policy as ever). Added a PC fetch-sanity guard to `iop_step()`
  (`source/core/iop/iop_core.c`): any fetch address outside real IOP
  RAM/BIOS ROM now halts immediately with a clear diagnostic naming
  the exact address, instead of silently reading back 0 (a NOP)
  forever and "wandering" through unmapped memory for tens of millions
  of steps before halting confusingly far away. 7 new checks
  (`tests/test_iop_pc_guard.c`), 44-file/0-failure regression, clean
  Wii rebuild.
- **GS: Gouraud shading for triangles** (task #78): PRIM's real IIP
  bit (bit 3, confirmed against a live fetch of PCSX2's own
  `GS/GSRegs.h`) now switches TRIANGLE/TRIANGLE_STRIP/TRIANGLE_FAN
  between flat shading and genuine per-vertex Gouraud interpolation.
  Added `tri_rgba[3]` color tracking alongside the existing
  `tri_x`/`tri_y` vertex-position buffers in `gif_state_t`;
  `rasterize_triangle()` (`source/hw/gif.c`) blends per-pixel via
  barycentric weights derived from the existing edge-function values -
  plain affine (screen-space) interpolation, honestly noted as NOT the
  real GS's perspective-corrected (1/Q) interpolation. 9 new checks
  (`tests/test_gif_gouraud.c`), 45-file/0-failure regression, clean
  Wii rebuild.
- **Wired a real GIF packet through DMA in main.c's on-device demo**:
  previously the "pixels reach the screen" milestone wrote GS memory
  directly, bypassing DMA/GIF. Now builds a real A+D-mode GIF packet,
  copies it into EE RAM, and calls `dma_channel_kick()` on the GIF
  channel (the real `dma_set_sink`/`gif_process_quadwords` pipeline
  wired up in `ee_core_init()`), drawing a Gouraud triangle below the
  color bars. Promoted `GIF_REG_*`/`GS_REG_*`/`PRIM_TYPE_*`/
  `PRIM_IIP_MASK` from `gif.c`-private to public in `gif.h` for this.
  Caught a real NLOOP/buffer-size bug in the process (`3 + n_verts`
  instead of `3 + 2*n_verts` - each vertex needs 2 register entries,
  RGBAQ and XYZ2, not 1) via the Wii target compiler's
  `-Warray-bounds` warning on the first build attempt. 11 new checks
  (`tests/test_dma_gif_demo.c`, mirrors main.c's exact packet logic
  host-natively), 46-file/0-failure regression, clean Wii rebuild.
- **Clock-rate-aware EE:IOP scheduler (8:1, was 1:1)**:
  `system_run_interleaved()` (`source/core/system.c`) now steps the EE
  `EE_IOP_STEP_RATIO` (8) times per 1 IOP step per slice, approximating
  real hardware's ~294MHz EE vs ~36MHz IOP clock ratio instead of the
  previous naive 1:1 round-robin - still explicitly NOT cycle-accurate
  (per-instruction cycle costs aren't modeled), an honest ratio-aware
  approximation only. `tests/test_system_handshake.c` (the only
  existing consumer) exercises the new ratio automatically via its
  generous slice cap and passes unchanged - no new test file needed.
  46-file/0-failure regression, clean Wii rebuild.
- **VIF0/VIF1 passthrough (first increment)**: new `source/hw/vif.c`
  parses real VIFcode tag streams (CMD/NUM/IMM fields, cross-checked
  against a live fetch of PCSX2's `Vif_Codes.cpp`/`Vif.h`), registered
  as the DMA sink for `DMA_CHANNEL_VIF0`/`VIF1`. Implements NOP/
  STCYCL/OFFSET/BASE/ITOP/STMOD/MARK (register stores), FLUSHE/FLUSH/
  FLUSHA/MSCAL/MSCNT/MSCALF (correct no-ops - no VU microcode
  interpreter exists yet), STMASK/STROW/STCOL (stores), MPG (data span
  skipped correctly, counted unsupported), and DIRECT/DIRECTHL
  (VIF1-only - forwards data straight to `gif_process_quadwords()`,
  the one command that actually draws pixels this round - a real,
  common BIOS/game pathway to the GS). UNPACK (0x60-0x7F) is
  explicitly NOT implemented (needs VU data memory this project
  doesn't have) - hitting it stops that transfer's processing cleanly
  rather than misparsing what follows. 24 new checks
  (`tests/test_vif.c`), including a real DIRECT-forwarded SPRITE
  packet proven by reading back its drawn pixel color. 47-file/0-
  failure regression (every existing EE-core-linking test needed
  `source/hw/vif.c` added to its link line - same transitive-
  dependency pattern as always when `ee_core.c` gains a new hardware
  call), clean Wii rebuild.
- **Texturing for the triangle rasterizer** (task #85): PRIM's real
  TME bit and TEX0's TBP0/TBW/TFX fields (cross-checked against
  PCSX2's own `GS/GSRegs.h`) now drive real texture mapping on
  TRIANGLE/TRIANGLE_STRIP/TRIANGLE_FAN. Nearest-neighbor PSMCT32
  sampling, UV-only "FST=1" coordinates (no ST+Q perspective-correct
  path), DECAL and MODULATE TFX modes (HIGHLIGHT/HIGHLIGHT2 simplified
  to behave like MODULATE), no CLAMP/wrap modeling. Texture-coordinate
  interpolation reuses the same barycentric weights as Gouraud color
  (real hardware always interpolates UV when texturing is on,
  independent of IIP). 10 new checks (`tests/test_gif_texture.c`):
  DECAL replacing vertex color entirely, MODULATE's exact per-channel
  blend math verified against hand-computed values, real per-pixel UV
  interpolation across a gradient texture (sampled exactly at each
  vertex's own coordinate, where barycentric weights are exact - a
  merely-nearby sample point can snap to the wrong texel under
  nearest-neighbor sampling, unlike Gouraud's continuous color blend),
  and a TME=0 regression check. 48-file/0-failure regression, clean
  Wii rebuild.
- **IOP HLE: real A0-table BIOS calls implemented** (task #86,
  user's explicit top priority this round): psx-spx's public
  A0/B0/C0 "Function Summary" tables (same reference already used for
  InstallExceptionHandlers) unblocked implementing ~17 pure-
  computation A0-table calls for real in `source/hw/iop_hle_bios.c`:
  ABS/LABS, STRCAT/STRNCAT/STRCMP/STRNCMP/STRCPY/STRNCPY/STRLEN,
  BCOPY/BZERO (BCOPY's arg order is `(src,dst,len)`, reversed from
  MEMCPY's), MEMCPY/MEMSET/MEMMOVE (MEMMOVE deliberately reproduces
  psx-spx's documented ";Bugged" real-hardware behavior - a plain,
  NOT-overlap-safe forward copy, matching real hardware rather than
  "fixing" it), INITHEAP (bookkeeping only, no real allocator), 
  FLUSHCACHE (correct no-op), and EXIT/_EXIT (now halts the core with
  a descriptive reason instead of silently returning 0 past a call
  real hardware never returns from). Live-reconnected to the user's
  real SCPH-10000 PCSX2-MCP session mid-round: confirmed the real,
  full 16-module IOP boot list (System_Memory_Manager, Module_Manager,
  Exception_Manager, Interrupt_Manager, ssbus_service, dmacman,
  Timer_Manager, System_C_lib, Heap_lib, Multi_Thread_Manager,
  Vblank_service, IO/File_Manager, Moldule_File_loader,
  ROM_file_driver, Stdio, IOP_SIF_manager) and live-disassembled the
  real (self-installed) A0/B0 vector dispatcher at RAM
  0x000000A0-0x000000C4 - uses `$k0` internally, which does NOT
  contradict psx-spx's documented `$t1`/R9 caller convention since
  this project's HLE intercepts execution at the trap address itself,
  before any real vector code would run. The round-14 wall (a genuine
  `JALR $ra,$s1` needing a real IOP module/IRX loader) is **not**
  cleared by this round - still the same honest architectural
  boundary, no fabrication attempted. 26 new checks
  (`tests/test_iop_hle_bios_functions.c`), 49-file/0-failure
  regression, clean Wii rebuild.

- **VU0/VU1 micro-instruction memory + microcode interpreter
  control flow** (task #87, task 3): a COMPLETELY SEPARATE thing from
  round 13's VU0 macro-mode (COP2) work - real hardware's VU "micro
  mode" runs an asynchronous microprogram (uploaded via VIF's MPG,
  kicked by MSCAL/MSCNT/MSCALF) using a totally different, VU-native
  64-bit-per-instruction ISA. New `include/core/hw/vu.h`/
  `source/hw/vu.c` (VU1: full 16KB/16KB mem+micro, own VF/VI regs) and
  `ee_core.c` additions (`vu0_exec_micro`/`vu0_micro_write32`, reusing
  round 13's existing `vu0_vf`/`cop2_ctrl`/`vu0_mem` fields - real
  hardware shares one physical VU0 between macro and micro mode).
  Live-fetched 7 real PCSX2 source files (`VU.h`, `VUmicro.h`,
  `VUmicro.cpp`, `VUops.h`, `VUops.cpp`, `VUmicroMem.cpp`,
  `VU1micro.cpp`) to get byte-exact real control flow: 8-byte
  instruction pairs, the E-bit's genuine one-instruction "delay slot"
  (verified against the exact countdown arithmetic in
  `VU0microInterp.cpp`), the I-bit (loads VI[21]/REG_I), and a
  correct-but-currently-unused branch delay-slot mechanism. **Honest
  scope boundary**: despite fetching all 7 files, this project could
  not locate PCSX2's actual `VU0_LOWER_OPCODE[128]`/
  `VU0_UPPER_OPCODE[64]` opcode-number table - no per-instruction
  FMAC/integer/branch body is decoded (every instruction is a real
  fetch with real flags honored, but a logged no-op body). MPG
  (`vif.c`) now writes real microprogram bytes instead of skipping
  them; MSCAL/MSCNT/MSCALF now actually run the real fetch-execute-
  until-E-bit loop instead of being total no-ops - exactly task 3's
  own bar, without fabricating opcode semantics. Required converting
  `tests/test_vif.c` from self-contained `#include` style to proper
  multi-TU linking (vif.c now calls into `ee_core.c`/`vu.c`) and
  adding `source/hw/vu.c` to every test that already needed
  `source/hw/vif.c`. New `tests/test_vu_micro.c`, 14 checks. 50-file/
  0-failure regression, clean Wii rebuild.

- **Perspective-correct (ST+Q) texture coordinates + SPRITE
  texturing** (task #88, task 4): PRIM's real FST bit (bit 8) now
  selects UV (FST=1, unchanged from task #85) vs. genuine perspective-
  correct ST+Q (FST=0, the real GS default) interpolation on
  triangles - the standard 1/Q, S/Q, T/Q barycentric algorithm real
  GS hardware uses (those ratios are affine in screen space; true
  per-pixel S/T is recovered afterward by dividing back out the per-
  pixel Q). `GS_REG_ST` (real IEEE-754 floats, live-fetched from
  PCSX2's `GS/GSRegs.h`) and Q (bundled into `GS_REG_RGBAQ`'s high
  word on real hardware - previously discarded via `(void)data_hi`)
  are now decoded; TEX0's TW/TH fields (previously ignored) scale
  normalized S/T into texel space (TH is a real hardware oddity
  straddling the 64-bit register's word boundary - 2 bits from word0's
  top + 2 bits from word1's bottom - confirmed against PCSX2's own
  overlapping-bitfield union). Verified with an exact-centroid test (a
  triangle's centroid always has barycentric weights of exactly
  1/3,1/3,1/3, for ANY triangle) using differing per-vertex Q values to
  prove genuine 1/Q division happens (samples texel 4) rather than a
  plain-affine bypass (would wrongly give texel 3). SPRITE gained real
  texturing too via a new `rasterize_sprite()` - a simpler, explicitly-
  documented per-corner-then-linear approximation (exact when both
  corners share the same Q, the common real-content case). Caught a
  real test-construction bug along the way: `GIFRegUV` packs BOTH U
  and V into the FIRST A+D word alone (word1 is pure padding) - a test
  bug, not a `gif.c` bug (existing UV decode was already correct; the
  pre-existing texture test never exposed it because it always used
  V=0 in both slots). New `tests/test_gif_stq_sprite.c`, 5 checks;
  fixed `tests/test_gif_texture.c`'s 3 pre-existing textured-PRIM
  constructions to set FST explicitly (now that FST=0 is a real,
  meaningful default rather than an ignored bit). 51-file/0-failure
  regression, clean Wii rebuild.

- **Z-buffer / depth test for triangles + SPRITE** (task #89, task
  6, and the last of the user's requested "1 3 4 6 komplett" set):
  real ZBUF_1/TEST_1 A+D registers (`GIFRegZBUF`'s ZBP/ZMSK,
  `GIFRegTEST`'s ZTE/ZTST, live-fetched from PCSX2's GS/GSRegs.h) plus
  the 4 real `GS_ZTST` compare modes (NEVER/ALWAYS/GEQUAL/GREATER).
  Getting real per-vertex Z required investigation: PACKED-mode
  `GIFPackedXYZ2` genuinely has Z as its own full word (word2,
  previously read into a local var and discarded) - a real, zero-
  regression-risk path since no prior test in this codebase ever used
  PACKED-mode XYZ2. But real hardware's A+D-mode XYZ2 register is
  only 64 bits total (X:16+Y:16 packed together, Z:32 in the other
  word) - this project's PRE-EXISTING A+D XYZ2 convention (already
  baked into every other test file and main.c before this round: X
  gets the whole first word, Y the whole second) leaves no room for Z
  at all. Rather than rewrite every existing test/demo, this is an
  explicit, honestly-scoped gap: Z only flows through genuine PACKED-
  mode XYZ2 (Z=0 via A+D, harmless since Z-buffer access stays fully
  gated - see below). New `tests/test_z_buffer.c` hand-builds real
  PACKED-mode packets for its vertices, same style as
  `tests/test_vif.c`. Z interpolates barycentrically for triangles
  (screen-space-linear, no 1/Q correction needed - real hardware
  behavior, verified via task #88's centroid trick reapplied: 3
  distinct per-vertex Z values average to the exact expected centroid
  value) and uses a flat "second/completing vertex" Z for SPRITE
  (extending this file's existing "flat shading uses the last vertex"
  convention to Z). A new, project-own `zbuf_configured` safety gate
  (not a real hardware concept) keeps every pre-existing test/demo
  that never configures a Z buffer behaving byte-for-byte exactly as
  before this round - without it, ZBUF's real default (ZBP=0) would
  silently alias and corrupt the color framebuffer's own default
  address. 20 new checks, 52-file/0-failure regression, clean Wii
  rebuild.

**devkitPro toolchain**:

**devkitPro toolchain**: FULLY FIXED, clean Wii rebuild verified. This
sandbox's extraction was missing `base_rules`/`base_tools` (fetched
from `github.com/devkitPro/devkitppc-rules`), `libogc` (a complete
prebuilt copy was sitting unused at `outputs/build/libogc-src/` from an
earlier round - symlinked in), `cc1` (the plain-C GCC front end -
`pkg.devkitpro.org` blocks automated downloads via Cloudflare, but the
user supplied a working Linux-native `devkitPPC-r32-linux-debian-
stretch.tar.gz` that had it), a `liblto_plugin.so` symlink target
(missing, recovered from the same r32 archive), `libmpfr.so.4` (cc1
needs an older Debian Stretch-era mpfr than this Ubuntu 22.04 sandbox
ships - fetched directly from `archive.debian.org`, no Cloudflare
issue there), and `libfat` (built from source,
`github.com/devkitPro/libfat`, once real compilation worked). `make
clean && make` now completes with **0 warnings, 0 errors**, producing
a real `pcsx2-wii.elf`/`.dol` - retroactively confirms rounds 9-12's C
changes compile cleanly for the real target. Full setup documented in
`outputs/build/devkitpro/TOOLCHAIN_SETUP_NOTES.md` (persists through a
`/tmp` wipe) plus `docs/STATUS.md`'s toolchain sections. Required env
for any future build in this sandbox:
```
export DEVKITPRO=<path>/outputs/build/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
export PATH=$DEVKITPPC/bin:$PATH
export LD_LIBRARY_PATH=$DEVKITPPC/lib:$LD_LIBRARY_PATH
```

**Tool note**: `github.com/hkmodd/PCSX2-MCP` (third-party, not this
project's own code) gives live debugging access to a real, user-run
PCSX2 instance - breakpoints, memory dumps, register reads, disassembly
- across EE (R5900) and IOP (R3000) address spaces independently. This
was the breakthrough that unblocked round 5 after rounds 1-4 stalled for
lack of a ground-truth reference. As of round 10, this is available as a
**directly callable MCP connector** (`mcp__pcsx2-mcp__*` tools -
`pcsx2_connect`, `pcsx2_set_breakpoint`, `pcsx2_continue`,
`pcsx2_read_registers`, `pcsx2_read_memory`, `pcsx2_disassemble`,
`pcsx2_step`, `pcsx2_set_watchpoint`, etc., loaded via `ToolSearch` if
deferred) - no more relaying through user-written report files. Requires
the user to have a real PCSX2 instance running with the DebugServer
enabled; use `pcsx2_connect` first, `pcsx2_status`/`pcsx2_pause` to check
state, and note that if a game/disc is already running past boot,
BIOS-boot-time addresses need a fresh System Reset (ask the user) before
a breakpoint on them will ever hit again - the emulator doesn't replay
boot on its own. If stuck on a similarly opaque EE/IOP divergence again,
using this tool directly (or asking the user to reset if the live session
has moved past the relevant point) is a legitimate, repeatedly-proven-
useful move - just remember EE vs. IOP addresses live in numerically-
overlapping-but-unrelated spaces, and confirm whether a disc/game is
mounted before comparing against this project's disc-less BIOS-only
boot (round 10 found the SIF/hardware-status values genuinely differ
between "disc inserted" and "BIOS only, no disc" real hardware boots -
always ask for/verify the no-disc case explicitly).

- **Round 15 - the round-14 IOP wall genuinely bypassed via a real
  module/IRX loader; a real VU opcode table; an SPU2 register
  scaffold**: user directive was "fix all IOP errors, port the IOP/
  IRX loader, the VU microcode table, and SPU2 if there's time."
  Implemented a real ELF32/MIPS IRX loader (`source/hw/iop_elf.c`) and
  a real ROMDIR/IOPBTCONF-driven module loader with export/import
  linking (`source/hw/iop_module_loader.c`), reverse-engineered from
  the user's own real BIOS (never committed - see
  `include/core/hw/iop_module_loader.h`'s citation trail) and cross-
  checked against ps2dev/ps2sdk's public `irx.h`. Live-traced against
  the real BIOS: this genuinely loads and jumps to real SYSMEM's entry
  point past the round-14 wall for the first time. A SECOND real bug
  was found and fixed this way: module entries were launched without a
  valid `$sp`, so SYSMEM's own prologue corrupted its saved `$ra` via
  a stack write to garbage memory, looping back into the *exact same*
  original wall - fixed by seeding `$sp` to a documented top-of-RAM
  value before each module entry. A THIRD, deeper, honestly-documented
  boundary was found one level in and left for later (module-entry
  argument registers/boot-info block not modeled, so SYSMEM's own
  RAM-size read returns 0) - see docs/STATUS.md's "Round 15" section
  for the full trace. Separately, found and used the original Sony
  "PS2 Vector Unit Instruction Manual" (PCSX2's own VU opcode tables
  were never locatable in any fetched source) to implement a real
  upper/lower VU opcode table (`source/hw/vu_opcodes.h`, replacing the
  prior control-flow-only no-op decode) - real FMAC arithmetic,
  accumulator family, integer ALU, load/store, and branches, with 12
  new targeted correctness tests. Added a real SPU2 register scaffold
  (`source/hw/iop_spu2.c`) at the real base address with real 16-bit
  access, time permitting. 54-file/0-failure regression, clean Wii
  rebuild.

- **Round 15b - native Wii test menu added to main.c (user request)**:
  user asked, in German, for a "nice PCSX2-style PS2-layout" test menu
  built directly into `boot.dol` so the .dol's basic liveness/navigation
  could be verified before continuing deeper emulator work. Rewrote
  `source/main.c` (209 -> 454 lines) into a real interactive menu: direct
  XFB pixel drawing (gradient background, borders, highlight box) reusing
  the already-tested `gs_rgb8_pair_to_ycbcr()`, D-pad navigation between
  3 items (BIOS Boot Test / GS-GIF Demo / About), a heartbeat+frame-
  counter indicator drawn every loop iteration (proves the app is alive
  even mid-action), and a bounded (`DEMO_STEP_CAP=2,000,000`) real call
  into `system_run_interleaved()` for the BIOS Boot Test action so the UI
  never appears to hang. User confirmed via a real Dolphin screenshot that
  the menu, navigation, heartbeat, and the expected "no SD/USB storage
  found" no-BIOS error path all work correctly (59.93 FPS). Hit the
  Write-tool-truncation bug again on this file (see workflow notes below)
  - fixed the same way, via a bash heredoc instead of the Write tool.

- **Round 16 - user's own real BIOS validated for local Dolphin testing,
  third BIOS-filename candidate added**: user uploaded their own real,
  legally-owned SCPH-10000 BIOS dump for local testing. Added a one-line
  source change - a third filename candidate,
  `sd:/pcsx2/bios/SCPH10000.bin`, in `source/main.c`'s
  `action_bios_boot_test()` - so an SCPH-10000 dump works without
  renaming. No BIOS bytes were ever copied into this repo, committed, or
  pushed; the user's BIOS file was only inspected transiently in the
  sandbox (size/header sanity check) and a separate, non-repo
  `dolphin_sdcard/` folder was built directly in the outputs directory as
  a virtual-SD-card deliverable for the user's own local Dolphin setup.
  93-binary/0-failure regression, clean Wii rebuild.

- **Round 17 - first real on-device (Dolphin) BIOS-boot validation;
  new IOP SYSCALL-exception boundary precisely traced**: user ran the
  native test menu's BIOS Boot Test against their own real SCPH-10000
  BIOS in Dolphin (via an `mtools`-built virtual SD image, `sd_v2.raw`
  - a first `pyfatfs`-built image mounted but its BIOS file was never
  found by real `libfat`, root cause presumed a `pyfatfs` write-
  compliance gap, not a project bug; `libfat`'s own source was read to
  rule out a device-naming mismatch first). Real on-device result: EE
  reached 16,000,000 instructions (pc=0x8000B8AC), IOP reached
  2,000,000 (pc=0x001A44EC), neither halted within the test cap - and
  this matches host-native diagnostics exactly. Deep-dived the IOP's
  steady state further with `capstone`-disassembled traces: confirmed
  the EE's SIF-mailbox polling (round 14) is genuinely legitimate real-
  hardware-shaped code, and found that the IOP, after settling cleanly
  at pc=0x001A44EC, takes a real MIPS SYSCALL exception at exactly
  instruction 3,059,999 (deterministic regardless of diagnostic chunk
  size) whose handling leaves `$ra=0x00100000` (SYSMEM's own load base,
  not a sane return address) and re-enters SYSMEM's own 17-iteration
  init loop with an already-underflowed stack pointer
  (sp=0xFFFFFF40) - likely connected to round 15's already-documented
  missing module-entry-argument/boot-info gap, now reached via a new
  path (a real SYSCALL instruction, not the A0/B0/C0 jump-table
  convention). Not fixed this round - full precise trace is in
  docs/STATUS.md's "Round 17" section for whoever picks this up next.
  93/93 regression, clean Wii rebuild.

- **Round 18 - module-entry boot-info gap fixed for real**: fixed
  round 15/17's documented stack-pointer-underflow gap by setting $a0
  to a real 2MB-RAM boot-info word before every module entry jump (see
  `iop_module_loader.c`'s `BOOT_INFO_RAM_MB`) - SYSMEM's own real,
  disassembled `lw v0,(a0); sll sp,v0,0x14` now computes a sane stack
  pointer instead of collapsing to 0. Verified the underflow had been
  silently overwriting the real exception-vector trampoline at address
  0x80 (installed by task #42's InstallExceptionHandlers), which is why
  a later, perfectly normal SYSCALL exception used to vector into
  garbage. Fixed, the IOP now reaches a new resting state at
  pc=0x101270-0x101288 that disassembles as a genuine, deliberately-
  authored real BIOS panic loop (write error code 2 to address 0, spin
  forever) - most likely because this project has never implemented a
  real IOP kernel SYSCALL dispatch table (distinct from the existing
  A0/B0/C0 jump-table mechanism). Flagged as the next concrete target
  in ROADMAP.md, not attempted this round. 93/93 regression unchanged,
  clean Wii rebuild.
- **Round 19 - corrected Round 18's own hypothesis**: precise
  instruction-level tracing (`/tmp/diag20.c`-`diag28.c`, real `$a0=2`
  captured right before the trap, cross-checked against psx-spx's
  public kernel syscall reference) found the panic loop is NOT caused
  by a missing SYSCALL dispatch table. It's a genuine, ordinary
  `SYS(02h) ExitCriticalSection()` correctly vectoring into a real,
  disassembled BIOS exception dispatcher at 0x00000c80-0x00000e30,
  whose handler-chain lookup at RAM[0x100] finds no handler registered
  yet - falling through to the same "load SYSMEM" escape hatch Round
  15 already documented, instead of really returning from the syscall.
  Also confirmed the "panic loop" itself is a real 4-phase driver-
  dispatch pattern completing normally, and that Status.IEc (interrupt
  enable) stays 0 throughout - no interrupt could break the loop
  regardless of timer/INTC state. Two next targets identified, neither
  fixed yet: (a) real handler-chain default-fallback behavior at
  RAM[0x100], (b) why IEc never gets enabled. Pure investigation, no
  source changes, 93/93 regression unaffected. See docs/STATUS.md's
  "Round 19" section for the full trace; ROADMAP.md's item 1 wording
  corrected to match.
- **Round 20 - VIF UNPACK implemented (user's "1, 4, then 5" item 4)**:
  real UNPACK (VIFcode 0x60-0x7F) ported directly from a live fetch of
  PCSX2's own `Vif_Unpack.cpp`/`Vif_Unpack.h` - S/V2/V3/V4(-5) data
  decode, STCYCL CL/WL skip/fill cycles, STMASK/STROW/STCOL masking,
  STMOD row modes, all into new VU0/VU1 local DATA memory write paths
  (`vu0_mem_write32()`/`vu1_mem_write32()`, siblings of the existing
  MPG-facing micro-instruction-memory writers). V3's real "reads 1
  component past its own size" hardware quirk (PCSX2 cites Ape Escape
  3 depending on it) is reproduced faithfully. 12 new checks in
  tests/test_vif.c (23->35), full regression still 0-failure, clean
  Wii rebuild. See docs/STATUS.md's "Round 20" section for the full
  citation trail and a subtle fill-mode timing quirk that needed
  hand-verification before the test's own expectations were trustable.
- **Round 21 - POINT/LINE/LINE_STRIP rasterization (user's "1, 4,
  then 5" item 5)**: real GS POINT (type 0) and LINE/LINE_STRIP (types
  1/2) rasterization added to `source/hw/gif.c`, closing the "Lines/
  points are still open" gap flagged since the original GIF/VIF work.
  Ported from a live fetch of PCSX2's own `GS/GSRegs.h`/`GSState.cpp`/
  `GS/Renderers/SW/GSRasterizer.cpp`/`GSDrawScanline.cpp` - real
  per-pixel DDA line rasterization (major-axis walk, linear color/Z
  step), flat-shading's real "last vertex" convention, full Gouraud
  support for LINE, no texture mapping (real hardware doesn't support
  it for POINT/LINE either). 17 new checks in tests/test_gif_line.c
  (52->53 test binaries), full regression still 0-failure, clean Wii
  rebuild. Two real test-construction bugs found and fixed while
  writing the test (a GIFtag NLOOP/loop-count mismatch, and a ZBUF_1
  ZBP value that exceeded its real 9-bit field width) - both
  documented in tests/README.md. See docs/STATUS.md's "Round 21"
  section for the full trace.
- **Round 22 - user directive: "handle ALL IOP problems before
  anything else" (broadening past Round 19's narrower "fix RAM[0x100]
  + Status.IEc" framing)**: started with an inventory pass (ROADMAP.md
  section 2 has exactly one open bullet, bundling the two Round 19
  sub-items) then found, via direct code inspection, a THIRD, real,
  previously-undocumented bug: RFE (Restore From Exception, COP0
  CO-format funct=0x10) was completely unimplemented in `iop_core.c`'s
  COP0 dispatch (only MFC0/MTC0 were handled) - meaning any real
  exception handler that tried to RFE-then-return would have hit an
  "unimplemented COP0 sub-opcode" halt, which would likely have masked
  progress the moment the still-open RAM[0x100] fix let a real handler
  run to completion. **Fixed**, ported from PCSX2's `R3000A.cpp`
  `psxException()`: `Status = (Status & ~0xF) | ((Status & 0x3C) >>
  2)`. 3 new checks in `tests/test_iop_rfe.c` (53->54 test binaries),
  hand-verified bit-for-bit through a full exception-entry-then-RFE
  round trip, full regression still 0-failure, clean Wii rebuild.
  Also confirmed, and promoted to a new ROADMAP.md bullet, a related
  still-open gap: nothing in the IOP interpreter's step loop actually
  checks Status.IEc to decide whether to take a hardware interrupt
  (`iop_intc.c`'s own scope comment already flagged this) - so IEc
  remains a dead bit with no observable effect until that's wired up.
  Partial psx-spx research recovered the real `RAM[0x100]` "Table of
  Tables"/ExCB layout but not yet the deeper structure sections needed
  to implement a cited default-fallback handler.
  **Same round, continued**: that hardware-interrupt gap was then also
  fixed. Cited from psx-spx's interrupts page (explicitly confirmed to
  apply to the PS2 IOP): every peripheral IRQ routes through ONE
  single CPU line, Cause.bit10 (IP2, non-latching), taken once
  Cause.bit10, Status.bit10 (IM2), and Status.bit0 (IEc) are all set -
  vectored exactly like the existing SYSCALL exception. New
  `iop_check_hw_interrupt()` in `iop_core.c`. 8 new checks in `tests/
  test_iop_hw_interrupt.c` (54->55 test binaries): a real `SW` to
  I_MASK correctly preempts the very next instruction the instant
  `(I_STAT & I_MASK)` goes nonzero, and `IEc=0` correctly blocks
  delivery even with `I_STAT & I_MASK` already nonzero. Full
  regression still 0-failure, clean Wii rebuild. `Status.IEc` now has
  a real, observable, end-to-end effect for the first time in this
  project (RFE can restore it, and the interpreter now checks it).
  See docs/STATUS.md's "Round 22" section for the full trace.
  **Same round, continued once more**: the `RAM[0x100]` exception-
  chain mechanism itself was then also completed. The earlier
  rendered-HTML psx-spx fetch had been truncated before reaching the
  needed section - re-fetching the page's raw markdown source
  directly from GitHub instead got far enough to recover the full,
  complete reference: 4 real priority chains (0-3), each a singly-
  linked list of 16-byte nodes (`00h`=next-pointer, `04h`=second-
  function ptr, `08h`=first-function ptr, `0Ch`=unused);
  `RAM[0x100]`/`RAM[0x104]` point at a real 4-entry chain-head array.
  New `include/core/hw/iop_excb.h`/`source/hw/iop_excb.c` implement
  `C(02h) SysEnqIntRP`/`C(03h) SysDeqIntRP` byte-exactly, including
  SysDeqIntRP's documented real BUG (can only correctly remove a
  chain's first element - any other position is modeled as a safe
  no-op, since the real "garbage stack read" outcome isn't citable).
  Deliberately NOT implemented: the real default handler CONTENTS
  (EnqueueSyscallHandler/EnqueueTimerAndVblankIrqs/InitDefInt) - would
  need real BIOS-ROM machine code this project has no reference for;
  the chains correctly start all-empty instead, matching the exact
  scenario Round 19's trace hit. 18 new checks in `tests/
  test_iop_excb.c` (55->56 test binaries), full regression still
  0-failure, clean Wii rebuild. **This closes out the user's "all IOP
  problems" sweep**: three real, previously-undocumented/unimplemented
  gaps found and fixed in one session (RFE, hardware-interrupt
  delivery, the RAM[0x100] chain mechanism), each with a citable real-
  hardware reference and its own regression test, none fabricated.
  ROADMAP.md section 2 has no remaining open bullets after this round
  (the only honestly-open piece left - real default handler BODIES -
  isn't something this project has ever had a reference for and isn't
  being synthesized). See docs/STATUS.md's "Round 22" section for the
  full trace.

- **GS Round 23 (same session, continued)**: per the user's explicit
  "go back to GS, implement the complete port" directive, implemented
  the GS alpha unit - alpha test (`TEST_1`'s `ATE`/`ATST`/`AREF`/
  `AFAIL`) and alpha blending (`ALPHA_1`'s `A`/`B`/`C`/`D`/`FIX`, gated
  by a new `PRIM_ABE_MASK` bit), cross-checked against PCSX2's own
  GS/GSRegs.h and GSDrawScanline.cpp via a dedicated research
  subagent pass. All 8 `ATST` compare modes and all 4 `AFAIL` outcomes
  implemented (including `RGB_ONLY`'s old-alpha-byte preservation);
  the real blend equation `((A-B)*C)>>7+D` truncates rather than
  rounds and does NOT clamp its coefficient to [0,1] (real hardware
  allows "boosted" results) - only the final per-channel color is
  clamped to [0,255]. A new shared `gs_finish_pixel()` helper
  centralizes this logic across all 4 rasterizers (triangle/sprite/
  point/line), a deliberate, documented deviation from this file's
  established "duplicate the small block" pattern, justified by the
  new logic's size and primitive-independence. Two bugs found and
  fixed along the way: `FIX` was initially read from the wrong word
  (`data_lo` instead of `data_hi`'s low byte - caught by cross-
  checking against `ZBUF_1`'s own word0/word1 convention), and two
  more instances of this project's recurring literal-`_*/`-in-a-
  comment block-comment-termination bug (see below). 13 new checks in
  `tests/test_gs_alpha.c` (56->57 test binaries), full regression
  0-failure, clean Wii rebuild. See docs/STATUS.md's "GS Round 23"
  section for the full trace and citations, and ROADMAP.md section 6
  for the updated remaining-gaps list (CLUT/paletted textures, real
  block-swizzled addressing, REGLIST/IMAGE transfer modes, GS context
  2, mipmaps).

- **GS Round 24 (same session, continued)**: per the user's follow-up
  directive to complete CLUT, block-swizzled addressing, REGLIST/
  IMAGE modes, GS context 2, and mipmaps "step by step" in one go,
  this round implements CLUT/paletted textures (PSMT8/PSMT4). TEX0's
  PSM field is now parsed (was previously ignored); its CLUT fields
  (CBP/CPSM/CSA/CLD) are new. The CLUT is modeled as its own small
  gs_mem region at CBP, 16 entries/row, with CSA selecting a bank
  offset (so multiple PSMT4 palettes can share one CLUT region).
  PSMT8 implements the real CSM1 8-bit index swizzle (index bits 3/4
  swapped before lookup) - proven by two symmetric test cases in the
  new `tests/test_gs_clut.c` (6 checks, 58->59 test binaries, full
  regression 0-failure, clean Wii rebuild). **Important caveat for
  this round**: a dedicated research-subagent dispatch (the same
  technique that worked well for Round 23's alpha citations) hit this
  session's own usage/session limit before it could run - so this
  round's PSM/CLUT field layout and the CSM1 swizzle are implemented
  from established PS2 GS knowledge rather than a freshly-verified
  primary-source citation trail, explicitly flagged as such in the
  code comments and docs/STATUS.md's "GS Round 24" section. A future
  round should try the live research pass again if it becomes
  available, to strengthen this citation trail. See docs/STATUS.md's
  "GS Round 24" section for the full detail.

- **GS Round 25 (same session, continued)**: real PSMCT32 page/block-
  swizzled addressing, added as a NEW, ADDITIVE API
  (`gs_mem_swizzle_addr32()` + read/write wrappers) alongside the
  pre-existing simplified-linear `gs_mem` functions - deliberately
  NOT wired into the rendering pipeline this round, since doing so
  would require auditing/migrating every existing GS test's `bp`/`bw`
  picks (many use arbitrary large values like 5000/10200 that are
  only valid under linear addressing) to real-hardware-valid ranges -
  a substantially larger, separate undertaking left explicitly open.
  Real page (64x32px)/block (8x8px, 32/page, real 8x4 block-index
  grid) addressing is modeled; within-block column interleave is not
  (row-major instead) - an honest partial step. Same session-limited-
  research caveat as Round 24 applies to the block table itself,
  mitigated by a structural no-collision test property (all 2048
  pixels of a page map to distinct addresses) that would likely catch
  a subtly-wrong table. 10 new checks (`tests/test_gs_swizzle.c`,
  59->60 test binaries), full regression 0-failure (all 59 prior
  tests literally unmodified, since this is purely additive), clean
  Wii rebuild. See docs/STATUS.md's "GS Round 25" section.

- **GS Round 26 (same session, continued)**: REGLIST and IMAGE GIF
  transfer modes - previously ANY non-PACKED GIF tag was byte-skipped
  with zero interpretation. REGLIST now parses real 2-registers-per-
  qword packing, routing every register through the existing
  apply_ad_write(); also fixed a real byte-accounting bug found along
  the way (the old fallback assumed REGLIST's data span equaled NLOOP
  qwords, same as IMAGE - actually ceil(NLOOP*NREG/2), which would
  have desynced the GIF stream on any real REGLIST packet). IMAGE
  mode implements host-to-local (XDIR=0) PSMCT32 transfers driven by
  real BITBLTBUF/TRXPOS/TRXREG/TRXDIR registers - local-to-host/
  local-to-local parsed but not acted on (documented gap). Same
  session-limited-research caveat as Rounds 24-25 applies. 15 new
  checks (`tests/test_gs_reglist_image.c`, 60->61 test binaries),
  full regression 0-failure, clean Wii rebuild. See docs/STATUS.md's
  "GS Round 26" section.

- **GS Round 27 (same session, continued)**: GS Context 2 (dual-
  context support) - previously only context 1 existed, PRIM's CTXT
  bit was never parsed. Added genuinely separate per-context
  permanent storage (ctx1_xxx/ctx2_xxx fields) plus a single new
  `gs_activate_context()` called at the top of each of the 4
  rasterizers, refreshing the pre-existing "active" fields from
  whichever context is selected - deliberately non-invasive, requiring
  zero changes to gs_finish_pixel()/gs_sample_texel()/gs_sample_clut()
  or the rasterizers' own bodies (validated: the full pre-existing
  61-test suite passes completely unmodified). Also found and fixed a
  real Round 26 doc-drift bug along the way (a `tests/README.md`
  command for `test_gs_reglist_image.c` that would fail to link if
  used verbatim - it double-included/linked gif.c/gs_mem.c). 10 new
  checks (`tests/test_gs_context2.c`, 61->62 test binaries), full
  regression 0-failure, clean Wii rebuild. See docs/STATUS.md's "GS
  Round 27" section.

- **GS Round 28 (same session, continued)**: Mipmap support - the
  fifth and last item of the user's directed sweep. New TEX1_1/
  MIPTBP1_1/MIPTBP2_1 registers (LCM/MXL/MMAG/MMIN/MTBA/L/K, and
  per-level TBP/TBW for mip levels 1-6). `rasterize_sprite()` (SPRITE
  only - TRIANGLE mipmapping is a documented gap) performs
  per-primitive nearest-single-level LOD selection: LCM=0 computes
  `floor(log2(texture-size/screen-size))`, LCM=1 uses a fixed K value,
  either way clamped to MXL and falling back to level 0 whenever MMIN
  is below the mipmap threshold, the draw is a magnification, or
  MTBA=1 (auto address calculation - unimplemented, safely degrades
  to level 0). Implemented as a save/override/restore of tex_tbp0/
  tex_tbw around the existing pixel loop - zero changes to
  gs_sample_texel()/gs_sample_clut()/the other 3 rasterizers, same
  non-invasive pattern as Round 27. Same session-limited-research
  caveat as Rounds 24-27. 25 new checks (`tests/test_gs_mipmap.c`,
  62->63 test binaries), full regression 0-failure, clean Wii rebuild.
  See docs/STATUS.md's "GS Round 28" section. **This completes all
  five items from the user's directed sweep this session**, each
  individually committed/pushed/rsynced as its own checkpoint.

- **Round 29 (2026-07-07, same session)**: user asked "ist der GS Port
  fertig?" then, given a prioritized punch list toward a real BIOS-driven
  splash screen, chose "Track B" (pursue the real IOP exception-handler-
  body gap using real bytes/behavior from their own SCPH-10000 dump rather
  than a synthetic HLE stub). This round is pure diagnostic tracing (no
  code changes) - single-stepped the real interleaved EE/IOP scheduler
  against the user's real BIOS, confirmed byte-for-byte via live Capstone
  disassembly that the real exception dispatcher (genuine BIOS code) does
  an unconditional, null-check-free dereference of the priority-0 handler
  chain, and that `RAM[0x100]` (the chain's table address) is still 0 at
  the exact moment the known early `ExitCriticalSection` SYSCALL fires -
  independently reproducing and substantially deepening Round 19's
  account. Exhaustive JAL/JALR tracing (every IOP call from reset to this
  SYSCALL, ~3.05M instructions) found ZERO calls to the public `0xB0`/
  `0xC0` BIOS vectors before this point. Also found that this project's
  own real ELF/IRX loader already delivers real PCB/TCB size config into
  part of the same table by this point (via `iop_elf.c`'s segment-copy,
  not a CPU instruction - which is why an earlier instruction-level-only
  store trace missed it) - only the ExCB entry specifically stays
  unallocated. See docs/STATUS.md's "Round 29" section for the full
  trace and docs/ROADMAP.md section 2's new open bullet. Not yet fixed -
  next step is tracing backward from whichever module load sets PCB/TCB
  to find its trigger.

- **Round 29 continued (2026-07-07, same session)**: found and fixed the
  actual root cause - real BIOS ROM code calls `B(00h)
  alloc_kernel_memory(size)` via a thunk-table tail call (`jr`, invisible
  to the earlier JAL/JALR-only trace), always got the generic default
  ($v0=0) since this project had no real B0-function-0 case, so
  RAM[0x100] never got a valid address despite the real allocation code
  being genuinely present and running. Implemented a real bump allocator
  plus a companion dynamic-lookup fix in `iop_excb.c`. New test
  `tests/test_iop_kmem_alloc.c` (19 checks), 64/64 regression, clean Wii
  rebuild. Confirmed via live re-trace that RAM[0x100] now genuinely gets
  set mid-boot - but honestly, a direct A/B test shows this fix alone
  does not change how far boot progresses by 30M IOP instructions, since
  a separate real ROM routine re-clears the whole low-RAM table-of-tables
  region shortly afterward. See docs/STATUS.md's "Round 29 continued"
  section for the full story; next step is tracing forward from that
  clear loop.

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

There are 52 test files as of this writing, covering both CPU cores (EE
integer/MMI/FPU/unaligned-access/COP0-CO-format/LQ-SQ/VU0-vector-datapath,
IOP integer/unaligned/SYSCALL-exception/InstallExceptionHandlers/PC-fetch-
sanity-guard), every hardware register model (EE DMA + chain-mode transfer
engine, GS registers + local memory + Wii output blit, GIF packet parsing +
SPRITE/TRIANGLE rasterization with flat and Gouraud shading, EE-side and
IOP-side SIF mailbox, IOP INTC/DMA/timers, IOP HLE BIOS trap + module
registry), the BIOS ROMDIR loader, and the two-core interleaved scheduler
with a real SIF handshake. The EE side has grown a cluster of
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
  address ranges (after masking KSEG0/1 to physical form via
  `ee_hw_mmio_addr()` - round 11 found this masking step missing, which
  meant real KSEG0/1-addressed hardware-register access had never
  actually worked, only literal KUSEG-style test addresses) to
  `dma_mmio_read32/write32` (32-bit MMIO path), `gs_mmio_read64/write64`
  (64-bit path, GS registers are genuinely 64-bit on real hardware),
  `sif_mmio_read32/write32` (EE-side SIF mailbox), or `mch_mmio_read32/
  write32` (MCH_RICM/MCH_DRD RDRAM auto-init, round 11) before falling
  through to the RAM/BIOS pointer path. **Never use
  `memcpy` for guest memory access** - see "Known sharp edges" below for
  why this matters here specifically. COP0 support: MFC0/MTC0 (generic
  registers, Status, Config) plus the "CO"-format instructions RFE/ERET/
  EI/DI (dispatched via a 6-bit `funct` field once `rs`'s top bit is set -
  NOT via `rs` itself, matching PCSX2's `tbl_COP0_C0[64]` table), a real
  48-entry TLB (`TLBR`/`TLBWI`/`TLBWR`/`TLBP` + KUSEG address translation
  via `ee_tlb_translate()`), real exception delivery for KUSEG TLB
  misses (`ee_raise_exception()`/`ee_raise_tlb_exception()` - Cause/EPC/
  Status.EXL/BEV-dependent vectoring, correct Cause.BD for delay-slot
  faults), and a real Timer (Count==Compare) interrupt
  (`ee_latch_timer_interrupt()`/`ee_check_timer_interrupt()`, round 9 -
  Cause.IP7 latches on Count>=Compare, sticky until an explicit Compare
  write acks it, gated by Status.IE/EIE/IM7/EXL/ERL, deferred across
  branch delay slots). See "Current frontier" above for the full story
  and current state - this is the most actively-changing part of the
  codebase. Still missing: general/SYSCALL exception delivery through
  this same path (SYSCALL still uses its own separate hand-written
  trap, and RFE/ERET/EI/DI still only handle the exception-RETURN
  side), and any INTC/DMAC-driven interrupt source (Timer is the only
  one modeled so far).
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
  (filled axis-aligned rectangles) plus, as of the first GS round,
  flat-shaded `TRIANGLE`/`TRIANGLE_STRIP`/`TRIANGLE_FAN` (edge-function
  scanline fill, single color per triangle - no Gouraud/textures/Z-test)
  directly into GS memory. This is the current "how does data actually
  turn into pixels" path - see `docs/ROADMAP.md` section 4 for exactly
  which register/primitive/mode combinations are and are not handled.
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

## Session-resume checklist (for a fresh Claude/Cowork sandbox)

Each Cowork session runs in an isolated sandbox with nothing persisted
from prior sessions except: this GitHub repo (clone fresh each time),
and whatever files the user has separately uploaded (the devkitPro
toolchain archives and the user's own real BIOS dump - see below).
Everything below is the exact, tested recipe this project has used
repeatedly across many "fresh sandbox" session starts.

**1. Clone the repo:**
```sh
git clone https://github.com/Mafiacoding/PCSX2-Wii.git /tmp/pcsx2-wii-git
cd /tmp/pcsx2-wii-git
```
Do not `rm -rf` and recreate this directory casually if it already has
a `.git` - that destroys local history (harmless, since GitHub has it,
but wastes a round-trip). Always work out of a real `git clone`, never
just a plain file copy of the outputs mirror (which has no `.git` at
all - it's a deliberately git-free rsync target, see the workflow
section below).

**2. devkitPPC/libogc toolchain**: the user has previously uploaded
`devkitPPC-r32-linux-debian-stretch.tar.gz` and (if the vendored
`elf2dol` fallback is needed) `MinGW-powerpc-eabi-13.1.0.zip` and
`libogc-1.8.18.tar.bz2` - check the session's uploads directory first;
if they're there, extract them (paths below are what prior sessions
used, adjust as needed):
```sh
mkdir -p /tmp/dkp_root
tar xf <uploaded devkitPPC tarball> -C /tmp/dkp_root --strip-components=<N>  # inspect the tarball's top-level dir name first
# libogc similarly extracted under /tmp/dkp_root/libogc
export DEVKITPRO=/tmp/dkp_root
export DEVKITPPC=/tmp/dkp_root/devkitPPC
export PATH=$DEVKITPPC/bin:$PATH
```
If the user hasn't uploaded these in the current session, ask them to
re-upload (they're large, ~200-230MB each, not something to fetch from
the open internet given licensing).

**3. The recurring `libmpfr.so.4` gap**: devkitPPC's `cc1` often fails
with `error while loading shared libraries: libmpfr.so.4`. Fix (once
per session): extract a `libmpfr.so.4`-named library into a scratch dir
and export `LD_LIBRARY_PATH` to include it before every `make`:
```sh
export LD_LIBRARY_PATH=/tmp/mpfr_extract/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```
(A Debian `libmpfr4` package works; `apt-get download libmpfr4` +
`dpkg-deb -x` into `/tmp/mpfr_extract` needs no root and has worked
repeatedly in this sandbox.)

**4. Build**: `make TARGET=boot` from the repo root produces `boot.dol`
directly (plain `make` names the output after the containing
directory instead, e.g. `pcsx2-wii-git.dol` if the clone dir is named
that - either works, `TARGET=boot` is just more predictable across
differently-named clone directories).

**5. Regression tests**: a maintained list of every host-native test's
exact `gcc` build command lives in `tests/README.md`. A prior session
also built a flat, ready-to-run command list at one point
(`/tmp/reg_cmds_core2.txt` in that session's sandbox - NOT part of the
repo, won't exist in a fresh sandbox; regenerate from `tests/README.md`
if wanted, or just build+run each `tests/test_*.c` per its documented
command). As of Round 18 there are 93 passing host-native test
binaries, 0 failures.

**6. GitHub push**: this sandbox has no stored git credentials.
Ask the user for a fresh Personal Access Token each time a push is
needed (do not ask them to paste it more than once per session's
remaining lifetime - reuse it from the conversation for subsequent
pushes in the SAME session). Use it only transiently:
```sh
git remote set-url origin "https://<PAT>@github.com/Mafiacoding/PCSX2-Wii.git"
git push origin main
git remote set-url origin "https://github.com/Mafiacoding/PCSX2-Wii.git"   # strip it back out immediately
```
Never write the PAT to any file on disk (including scratch `/tmp`
files) - keep it in-memory/in-conversation only, per this project's
standing security rule.

**7. The user's real BIOS (SCPH-10000)**: for any diagnostic work
against real BIOS behavior (as opposed to synthetic test fixtures),
the user's own real, legally-owned BIOS dump is available in the
session's uploads directory (as `scph10000.bin` and/or
`scph10000.zip`, ~4MB, `rom_ver=0100JC20000117`). **Absolute, standing
rule, unchanged across every round**: this file (and any bytes derived
from it) must NEVER be committed to git, NEVER pushed to GitHub, and
NEVER copied into the outputs/rsync mirror. Use it only via `/tmp`-only
scratch diagnostic programs (see the many `/tmp/diagN.c` throwaway
tools referenced in docs/STATUS.md's Round 15-18 sections for the
established pattern) - copy it to a `/tmp` path, never into the repo
clone directory.

**8. Outputs/rsync mirror**: the user-visible deliverable copy lives
in the session's `outputs` folder (mounted, persists for the user
after the session ends - NOT the same as the git clone). Sync the repo
clone into it with (verified, working command from Round 15 onward):
```sh
rsync -a --delete --exclude '.git' --exclude 'test_*' \
      --include 'tests/test_*.c' --exclude '/build/' \
      /tmp/pcsx2-wii-git/ "$OUT/pcsx2-wii/"
# rsync's include/exclude ORDER means the 3 test_*.c source files
# still get excluded by the earlier --exclude 'test_*' rule (basename
# match, first-match-wins) - work around it by re-copying them directly:
for f in tests/test_iop_elf.c tests/test_iop_spu2.c tests/test_vu_micro.c tests/test_gif_line.c tests/test_iop_rfe.c tests/test_iop_hw_interrupt.c tests/test_iop_excb.c tests/test_gs_alpha.c tests/test_gs_clut.c tests/test_gs_swizzle.c tests/test_gs_reglist_image.c tests/test_gs_context2.c tests/test_gs_mipmap.c; do
  cp "/tmp/pcsx2-wii-git/$f" "$OUT/pcsx2-wii/$f"
done
```
Files already written to the outputs folder cannot be deleted/renamed
by Claude - stray old `.dol`/`.elf` files with mismatched names
sitting there from earlier rounds are harmless and expected; verify
with `diff -rq --exclude='.git' --exclude='build' --exclude='data'`
that nothing else differs, and confirm `find "$OUT" -iname '*.bin'`
turns up nothing (the BIOS-never-shared rule, self-checked every
round).

**9. Where to look first when resuming**: read this file's "Current
frontier" section above (kept as a running per-round log) and
docs/ROADMAP.md's "Suggested near-term order" section (kept short and
current, not a history log - docs/STATUS.md has the full history) for
the actual next task. As of Round 22, the user had explicitly
overridden the prior "1, 4, then 5" ordering with a broader standing
directive: **handle ALL IOP problems first, before any other task
(including GS) resumes.** Round 22 fixed THREE real, previously-
undocumented/unimplemented gaps found via direct code inspection and
completed research (not just re-reading prior round narratives - a
reminder to keep reading the actual code/sources during any future
sweep like this, since more undocumented gaps may still be hiding):
RFE was completely unimplemented; no hardware-interrupt delivery
existed in the IOP interpreter at all; and the real RAM[0x100]
exception-handler-chain mechanism (SysEnqIntRP/SysDeqIntRP, byte-exact
per a fully-recovered psx-spx reference - fetched as raw markdown
directly from GitHub after the rendered-HTML version kept truncating)
is now implemented. `Status.IEc` now has a real, observable
end-to-end effect for the first time in this project. **ROADMAP.md
section 2 (IOP) has NO remaining open bullets after Round 22** - the
user's "all IOP problems" sweep is genuinely done, modulo the one
honestly-unclosable gap (real default handler BODIES - actual BIOS-
ROM machine code for SyscallException/VblankIrq/etc. - which this
project has never had a reference for and correctly does not
fabricate). The next session should feel free to resume GS/CDVD/COP2
work, or pick a new priority with the user, now that this directive's
scope is exhausted.

**Update (GS Round 23, same session)**: the user then explicitly
redirected back to GS with "go back to GS and implement the complete
port." Round 23 implemented the GS alpha unit (TEST_1 alpha test +
ALPHA_1 blending, gated by PRIM's ABE bit) against real, cited
PCSX2-source semantics - see "Current frontier" above and
docs/STATUS.md's "GS Round 23" section. Per ROADMAP.md section 6, the
remaining open GS gaps are: CLUT/paletted textures, real
block-swizzled addressing, REGLIST/IMAGE GIF transfer modes, GS
context 2 (dual-context), and mipmaps - any of these is a reasonable
next increment if "complete GS port" work continues.

**Update (GS Round 24, same session)**: the user directed all four
remaining items (CLUT, block-swizzled addressing, REGLIST/IMAGE
modes, GS context 2, mipmaps) be completed immediately, step by step,
also asking for an explicit checkpoint given this session's own usage
limits - meaning: commit+push after EACH increment individually
(never batch multiple features into one uncommitted working state),
so a session cutoff mid-sweep never risks losing already-finished
work. Round 24 (CLUT/paletted textures) is the first of these five and
is complete, committed, and pushed - see "Current frontier" above.
Round 25 (real block-swizzled addressing, additive API) is the
second and is also complete/committed/pushed. Round 26 (REGLIST/IMAGE
transfer modes) is the third and is also complete/committed/pushed.
Round 27 (GS context 2) is the fourth and is also complete/committed/
pushed. Round 28 (mipmaps) is the fifth and last, and is also
complete/committed/pushed - see "Current frontier" above and
docs/STATUS.md's "GS Round 28" section. **All five items from the
user's directed sweep are now done**, each individually checkpointed
(committed + pushed + rsynced to outputs) as it finished, satisfying
the user's explicit session-limit/checkpoint request. The next
session should feel free to resume GS/CDVD/COP2 work, or pick a new
priority with the user - see docs/ROADMAP.md's "Suggested near-term
order" section for the open, not-user-directed items noted there
(wiring Round 25's swizzle addressing into the pipeline, extending
Round 27's dual-context and Round 28's mipmap support beyond their
current scope, and MTBA=1 auto mip addressing).

**Update (Round 29, same session)**: user then asked whether the GS port
is complete (answer: no - the 5 requested items are done, but real GS is
~114,500 lines vs. this project's deliberate subset), then asked for a
prioritized punch list toward a real, BIOS-driven splash screen. Chose
"Track B": pursue the real IOP exception-handler-body gap (docs/
ROADMAP.md section 2's newest open bullet) using real bytes/behavior from
their own dumped SCPH-10000 BIOS, explicitly declining a synthetic HLE
stub. This round made real, substantiated progress narrowing the root
cause (see docs/STATUS.md's "Round 29" section) but did NOT reach a fix -
the next session should resume by tracing backward from whichever module
load sets the PCB/TCB Table-of-Tables fields (confirmed real, via this
project's own ELF loader) to find what triggers it, since that path is
the most promising lead toward the real ExCB allocation+registration
mechanism. User also has a legally-owned demo DVD dump available for
future CDVD work, explicitly NOT needed for the current investigation
(this wall is pure BIOS-kernel bootstrap, before any disc read would
occur).

**Update (Round 29 continued, same session)**: user said "Ja" (continue)
plus asked to verify GitHub was fully up to date (confirmed clean via
`git diff origin/main main --stat` before resuming). Continued the trace
and found + fixed the actual root cause: real, genuinely-executing BIOS
ROM code calls `B(00h) alloc_kernel_memory(size)` via a thunk-table tail
call (`jr`, not `jal`/`jalr` - why the earlier exhaustive JAL/JALR trace
found "zero calls to 0xB0/0xC0"), and since this project had no real
case for B0 function 0, it always returned 0 ("alloc failed"), so
RAM[0x100] never got a valid address even though the real allocation
code is demonstrably present and running. Implemented a real bump
allocator (`IOP_HLE_B0_ALLOC_KERNEL_MEMORY` in `iop_hle_bios.c`, bounded
by a new `IOP_KMEM_REGION_SIZE`) plus a companion fix in `iop_excb.c`
(`chain_head_addr()` now reads RAM[0x100] dynamically instead of a
hardcoded constant). New test `tests/test_iop_kmem_alloc.c` (19 checks),
64/64 total regression, clean Wii rebuild. **Honest result**: verified
via live re-trace that RAM[0x100] now genuinely gets set to 0xE000
mid-boot (previously stayed 0 forever) - but a direct A/B test (git-
stash-toggled) to 30M IOP instructions shows this fix alone does NOT
change how far boot progresses; both builds land at the identical
steady-state PC, because a separate, genuinely-executing block of ROM
code unconditionally re-clears the whole low-RAM table-of-tables region
shortly after the allocator succeeds. See docs/STATUS.md's "Round 29
continued" section for the full story. Next step: trace forward from
that clear loop (ROM ~0xbfc4d2c8-0xbfc4d360) to find what comes after it
and whether/when the ExCB chain gets rebuilt a second time.

**Update (Round 29 continued, 2nd fix, same session)**: user said "real
implementieren" (implement it for real) after being shown that C(01h)/
C(0Ch) only had vague psx-spx documentation. Deep live disassembly fully
mapped out the real exception dispatcher (0xc80-0xe98) and
ReturnFromException (0xf30-0x1000) byte-for-byte, confirming the real
BIOS calls C(01h) EnqueueSyscallHandler and C(0Ch) InitDefInt right
after B(00h) succeeds. Implemented B(18h) ResetEntryInt (writes the
real, ROM-confirmed RAM[0x7520]=0x6C34 constant) and C(01h)
EnqueueSyscallHandler (installs a real, hand-assembled, position-
independent MIPS trampoline implementing psx-spx's exact SYS(01h)/
SYS(02h) semantics, ending at the real ReturnFromException address, via
the existing real SysEnqIntRP mechanism). New test
tests/test_iop_syscall_handler.c (26 checks, including full execution
of the installed bytes through the real IOP interpreter), 64/64 total
regression, clean Wii rebuild. **Honest result**: both fixes genuinely
execute on the real boot path, but do NOT clear the ultimate wall - the
SAME real ROM clear-loop found in the first fix this round wipes
RAM[0x100] ~2.8M instructions before the dispatcher ever runs, so the
new syscall-handler chain node (though correctly built) becomes
unreachable by the time it's needed. EE/IOP land at the identical
steady-state PCs as before. See docs/STATUS.md's "Round 29 continued
(2nd fix)" section. C(0Ch)/InitDefInt was deliberately left
unimplemented this round (DefaultInterruptHandler's real behavior is
far more complex and not well-evidenced enough to implement responsibly
yet). Next step: trace forward from the clear-loop's own return address
to find whether/how RAM[0x100]/RAM[0x108] get re-established before the
dispatcher runs.

**Update (Round 29 continued, 3rd finding, same session)**: user said
"mach weiter" (continue). Traced the clear-loop's own caller (return
address 0xbfc52b4c) and found - live-decoded straight from the ROM
bytes - it compares a name against the literal string "LOGO" (with a
"CD001" ISO9660 signature nearby), matching this project's own already-
understood ROMDIR mechanism (task #33), and calls into a loaded IRX
module at RAM 0x00030000 (task #92's real module loader placed it
there) via jalr. This is almost certainly the real BIOS's own logo-
decompression/rendering routine. It runs largely self-contained (no
further A0/B0/C0 calls) for ~2.8 million instructions - only 2
FlushCache calls - before the previously-chased dispatcher wall. This
reframes the investigation: the exception-handler gap sits AFTER the
real logo code chronologically, not before it, so it may not currently
block a splash screen from displaying at all. Getting a real splash
screen likely depends more on wiring the GS/display driver path into
the real boot flow (task #126) than on further exception-dispatcher
work. Not yet checked whether the logo module's output reaches
GS-visible memory - that's the natural next step. Pure diagnostic
finding, no code changes this round (see docs/STATUS.md's "Round 29
continued (3rd finding)" section).

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

**Update (Round 29 continued, 4th change, same session)**: user said
"mach die 126 und sorg dafuer das die main.c von demo auf echten boot
flow geht anders koennen wir die probleme nicht behebn, mach
checkpoints, sorg das der git immer up to date ist und fuehre nach
dieser aufgabe automatisch andere wichtige tasks durch" (do task #126:
switch main.c from demo mode to a real boot flow - "otherwise we can't
fix the problems" - make checkpoints, keep git always up to date, then
automatically continue with other important tasks). Implemented:
`run_real_boot_flow()` now runs automatically at startup instead of
being gated behind a capped, menu-only "BIOS Boot Test" demo action -
loops the real interleaved EE/IOP execution in chunks up to a much
higher total cap, polls the real `pmode` GS register each iteration,
decodes the real DISPFB1 hardware field layout when a display circuit
is active, and calls the real `gs_blit_psmct32_to_xfb()` blit instead
of only ever drawing a hardcoded test pattern. Verified: clean Wii
rebuild (exit 0), full 65-test host-native regression suite (0
failures). Honest caveat carried over from task #127's diag53 finding:
GS registers/memory were observed to stay at power-on-zero through the
traced instruction window, so this is correct real scaffolding, not
yet a proven splash screen - see docs/STATUS.md's "Round 29 continued
(4th change)" section and docs/ROADMAP.md's GS-Treiberpfad bullet.
Next step per the user's own standing instruction: proceed
automatically to the next most important open ROADMAP item once this
change is committed/pushed/rsynced.

**Update (Round 29 continued, 5th finding + fix, same session)**:
per the user's standing instruction to continue automatically with
other important tasks after the main.c change, picked up task #124
(the deprioritized IOP exception-chain wall) with a concrete new lead
from diag53 (GS registers never touched). Live call-tracing found the
real BIOS calls A(13h) setjmp(buf) and B(19h) HookEntryInt(addr)
back-to-back with the same address (0x8004fd50, confirmed live) -
neither was implemented, so RAM[0x7520] never got updated to the
BIOS's own intended fallback address. Implemented both for real;
verified via 18 new host-native checks (tests/test_iop_hook_entry_int.c)
and live re-tracing (RAM[0x7520] now correctly reads 0x8004fd50 after
this call sequence). This is a genuine, well-evidenced bug fix, but it
does NOT clear the ultimate wall - a re-run after the fix produced an
identical PC-for-PC trace. Direct disassembly of IOP RAM
0x101100-0x101288 (the code both cores are actually stuck in)
precisely pinpointed the real wall: a bounded 4-pass retry loop
resembling a device-driver table walker (matching psx-spx's
A(96h)-A(99h) AddCDROMDevice/AddMemCardDevice/etc, none of which this
project implements), falling into a literal "write status code 2 to
RAM[0], spin forever, SR=0" panic routine when all 4 passes fail. This
sharpens, but does not yet resolve, the same wall this project's Round
19 already found and task #124 already deprioritized. Verified: clean
Wii rebuild (exit 0), 66-test host-native regression suite (0 real
failures). See docs/STATUS.md's "Round 29 continued (5th finding +
fix)" section for the full trace and the concrete next step (chase
backward from IOP RAM 0x1011ac to find what condition each retry pass
tests).

**Update (Round 29 continued, 6th change, same session)**: user
requested (verbatim, translated): "looks like we first need to add
CDRomDevice and MemCardDevice, add both as active devices, not as a
demo". Implemented A(96h) AddCDROMDevice() and A(97h)
AddMemCardDevice() for real: both now genuinely, persistently flip a
registered flag in iop_hle_bios_state_t (cdrom_device_registered /
memcard_device_registered) plus call counters, idempotent on repeat
calls - real, queryable state, not a no-op stub. Deliberately did NOT
fabricate an in-RAM DCB (Device Control Block) struct write, since
psx-spx documents the DCB table's address/size but not a citable,
byte-exact per-entry layout - writing guessed bytes into real emulated
RAM would risk being worse than not implementing this at all. Verified
via 17 new host-native checks (tests/test_iop_device_registration.c).
Honest empirical result (checked, not assumed): live-tracing the real
BIOS for 10M IOP instructions shows neither function is ever called on
this no-disc, no-memory-card boot path - both counters stay 0, and the
5th finding's steady-state loop is unchanged. This is real, valuable,
user-requested BIOS coverage, but does not resolve task #124/#132's
wall by itself - that still needs backward disassembly from IOP RAM
0x1011ac. Verified: clean Wii rebuild (exit 0), 67-test host-native
regression suite (0 failures). See docs/STATUS.md's "Round 29
continued (6th change)" section.

**Update (Round 29 continued, 7th finding, same session)**: continued
task #124/#132 (per the user's "erledige alle Aufgaben soweit wie
moeglich" - do all tasks as far as possible - instruction) with full
dynamic instruction tracing instead of static disassembly guessing.
Corrected the 5th finding's "resembles a device table" guess: the
retry loop's list is confirmed genuinely empty via live trace (`lw
$v0,8($s0); beqz $v0,0x101188` - branch taken), traced one level
further back to a specific stack slot (`$fp+0x40`) that's zero/null
when a real IOP routine runs around IOP instruction 3.05M (deep inside
the LOGO-module window, not near the C(0Ch) InitDefInt call at
instruction 84868 - so probably NOT InitDefInt despite the tempting
docs match). No code change this round - pure diagnostic progress,
documented in docs/STATUS.md's "Round 29 continued (7th finding)"
section. Next step: trace backward from this routine's entry/call site
to find the real missing data source. Pausing this specific thread
here (diminishing returns without much more tracing) to pick up other
ROADMAP items per the user's "as far as possible" instruction.

**Update (Round 29 continued, 8th change, same session)**: continuing
per the user's "erledige alle Aufgaben soweit wie moeglich" (do all
tasks as far as possible) instruction, picked up docs/ROADMAP.md's own
long-standing "CDVD (disc) stub" near-term item. Added
source/hw/iop_cdvd.c + include/core/hw/iop_cdvd.h: a real CDVD
register block at IOP address 0x1F402000 (mirrored across its full
4KB page, matching real PCSX2's psxHw4Read8/Write8), with real
power-on-reset values ported directly from a freshly-cloned PCSX2
upstream source's pcsx2/CDVD/CDVD.cpp cdvdReset() - Status=tray-open,
Ready=drive-ready, DiscType=no-disc - the exact values a real PS2
reports on a diskless boot. NCMD writes are latched and trigger a
plausible completion IRQ instead of leaving BUSY forever, so a
diskless boot's polling loop won't spin indefinitely - real N-command
state machines are NOT modeled, matching this project's existing
iop_timers.c/iop_spu2.c scaffold pattern. Verified via 19 new
host-native checks (tests/test_iop_cdvd.c). Wired into
source/core/iop/iop_core.c's byte-level memory dispatch
(iop_mem_read8/write8). Honest empirical result: live-tracing the real
BIOS for 10M IOP instructions shows it never writes to CDVD registers
on this no-disc boot path either - consistent with the 6th change's
AddCDROMDevice/AddMemCardDevice finding. Real, ROADMAP-directed
coverage; does not change the steady-state wall from the 5th/7th
findings. Verified: clean Wii rebuild (exit 0, iop_cdvd.c confirmed
compiled in), 68-test host-native regression suite (0 failures). See
docs/STATUS.md's "Round 29 continued (8th change)" section.

**Update (Round 29 continued, 9th finding, same session)**: traced the
7th finding's empty `$fp+0x40` one level further. It's
`RAM[0x100010+0x08]` - a field inside SYSMEM module's OWN "boot info"
struct at its load address+0x10. `source/hw/iop_module_loader.c`'s
already-cited `BOOT_INFO_RAM_MB` work populates word 0 of this struct
correctly (confirmed: reads back 2, matching real hardware's RAM-MB
convention), but SYSMEM's own code (disassembled this round, entry at
0x100d00-0x100d54) reads SEVEN MORE words from the same struct
(offsets 0x04-0x18) that this project has never populated (all zero).
Offset 0x08 is exactly the field the panic loop traces back to.
Searched psx-spx, ps2tek, PCSX2 upstream source (freshly cloned this
session), and an independent detailed PS2-boot-process write-up
(Woon Yung's "Initializing the PS2/PSX") - none document SYSMEM's
specific internal boot-parameter struct beyond the single RAM-MB word
already implemented, so no fix was made (fabricating values without
evidence would violate this project's own standard - see the CDVD/DCB
decision in the 8th change). Next step for whoever continues: trace
SYSMEM's own use of offsets 0x08-0x1c to infer plausible real values
from behavior, the same approach that resolved B(00h)/B(18h)/B(19h)
earlier this session. Task #124/#132 now traced three levels deep from
where the 5th finding left off. Pausing this thread here - this is a
natural, well-documented stopping point after a full session of real,
verified, incremental progress (main.c real boot flow, setjmp/
HookEntryInt fix, AddCDROMDevice/AddMemCardDevice, CDVD register
scaffold, and this precise 3-level trace) with every change
individually committed, pushed, and rsynced as its own checkpoint per
the user's explicit "work until the limit runs out, don't forget
checkpoints" instruction.

## Update (Round 29 continued, further autonomous work per user's
"take care of the next 10 important tasks, don't ask permission, use
pcsx2-mcp in an emergency" instruction)

10th finding: the newly-available `pcsx2-mcp` tool suite (a live,
paused, real PCSX2 instance via DebugServer) was investigated as a
possible source of ground truth for the 9th finding's open question
(SYSMEM boot_info struct values). Conclusively unhelpful: that
instance's EE is deep in userland/game code (backtrace shows ordinary
0x0055xxxx-0x0061xxxx EE addresses), long past IOP boot, and IOP
RAM[0x100000] reads all zero there - any transient boot_info struct
has long since been reclaimed. Also newly clarified while
investigating: this project's own `iop_module_loader.c` only ever
`bump_alloc(4)`s and writes the FIRST word of this struct, so offsets
0x04-0x18 read zero because this project's HLE loader shortcut never
touches them - not because of a separate missing mechanism. Task
#124/#132 is formally deprioritized again; every readily-available
real reference source (psx-spx, ps2tek, PCSX2 upstream, an independent
write-up, and now a live reference instance) is exhausted without a
citable answer, and guessing further would be fabrication.

11th change: extended round 13's EE COP2 VU0 macro-mode vector
datapath with VADD/VMUL (funct 0x28/0x2A, same shape as the existing
VSUB) and VIADDI (funct 0x32, closing a previously-flagged gap next to
VIADD/VISUB/VIAND/VIOR). New test `tests/test_ee_cop2_arith2.c` (11
checks, all passing) also gives first-time coverage to VIADD/VISUB/
VIAND/VIOR themselves. Full 68-block regression suite passes; clean
Wii rebuild verified. Honest scope note: a fresh diagnostic confirms
the EE's current boot trace does not yet reach any of these ops (still
steady-state SIF-polling) - this is real, tested, roadmap-directed
readiness work (docs/ROADMAP.md section 5 item 3), not a fix that
advances the current observed boot state.

## Update (Round 29 continued, 12th change - real fix, per user's
"process the tasks causing the blockade, then continue chasing the
device-driver retry loop root cause" instruction)

Live-traced SYSMEM's real init disassembly (RAM 0x100D00-0x100D8C):
found it actively dereferences boot_info offset 0x0C as a pointer
(`sw zero,(a0)` where a0 = offset 0x0C's value) - previously 0 (this
project's loader only ever allocated the struct's first 4 bytes), so
that write's real target was RAM address 0. Fixed by allocating the
full 0x20-byte struct and pointing offset 0x0C at dedicated,
zero-initialized scratch (same honest "defensive value, not a
verified real constant" precedent as INITIAL_SP). Empirically verified
real forward progress: IOP steady-state pc moved from 0x00101284 to
0x001012A8 (a genuine function-pointer-table dispatch loop now
executes, not just the same wall at a new address) - the panic loop
is still eventually hit, so task #124/#132 stays open, but this is
real measurable progress, not a dead end. New test (7 checks, all
passing), full 70-block regression passes, clean Wii rebuild
verified, all docs updated.

## Update (Round 29 continued, 13th finding - precise characterization + honest negative result)

Traced the NEW wall (pc=0x001012A8, after the 12th change's real fix)
backward to its caller. Found: the retry loop's "empty list" check
reads a freshly stack-allocated buffer whose SIZE is computed directly
from boot_info offsets 0x10 and 0x1C (both currently 0 in this
project, real values still unknown). Empirically tested (throwaway
diagnostic, not committed) whether supplying small nonzero counts (1,
4, 8) for these fields changes anything: it does NOT - IOP pc stays
identical at 0x001012A8 in every case, since the buffer is explicitly
zero-filled regardless of its size and nothing else populates it in
the traced code. This rules out "these fields just need a specific
count" and correctly re-scopes the real gap: some actual device/
handler registration mechanism this project doesn't yet emulate is
what's supposed to populate these lists - not a missing magic number.
No code change made (deliberately - the tested hypothesis failed, and
guessing further would be fabrication with no evidence behind it).
Task #124/#132 remains open; documented precisely so whoever continues
next knows exactly what's been ruled out and what the real remaining
question is (find the real registration mechanism, likely in LOADCORE
or an earlier part of SYSMEM's own init, not in boot_info's raw
values).

## Update (Round 29 continued, 14th change - GS mipmap extended to TRIANGLE)

Extended Round 28's SPRITE-only mipmap LOD-selection to TRIANGLE too
(source/hw/gif.c's rasterize_triangle()), reusing the exact same
algorithm with the triangle's screen-space bounding box standing in
for SPRITE's well-defined width/height. New test (3 checks, all
passing) mirrors test_gs_mipmap.c's structure. Found and documented
two GIF-packet test-authoring bugs along the way (NLOOP undercounting,
splitting TEX1/MIPTBP1 across two separate packet calls instead of
one) - product code was fine, the test fixtures needed the fix. Full
71-block regression passes, clean Wii rebuild verified.

## Update (Round 29 continued, 15th change - GS TEX1/MIPTBP made per-context)

Closed part of Round 27's "CLAMP/TEX1/TEX2/SCISSOR/FBA/MIPTBP
unmodeled for either context" gap: TEX1/MIPTBP1/MIPTBP2 (Round 28's
mipmap registers) were context-1-only, meaning a context-2 draw
silently reused context 1's mip config. Added the real _2 register
addresses (base+1, same convention as every other pair), per-context
permanent storage, and wired into gs_activate_context() - same
established Round 27 pattern. New test (6 checks, all passing) proves
genuine independence: context 1 configured with mipmapping engaged
samples its own mip level; context 2 (TEX1_2 never written) correctly
uses the base level instead of inheriting context 1's. All existing
mipmap/context2 tests still pass unchanged. Full 72-block regression
passes, clean Wii rebuild verified. CLAMP/TEX2/SCISSOR/FBA remain
entirely unmodeled - a separate, bigger gap since those registers
don't exist in this codebase at all yet.

## Update (Round 29 continued, 16th change - EE COP2 VMAX/VMINI added)

Extended the VADD/VMUL/VSUB COP2 CO-format arithmetic row (Round 13's
VSUB, this round's earlier 10th change's VADD/VMUL) with VMAX (funct
0x2B) and VMINI (funct 0x2F) - same 3-operand full-vector shape, per
lane selected by destmask. Ported from PCSX2 upstream's own
VUops.cpp _vuMAX/_vuMINI (applyMinMax<fp_max>/applyMinMax<fp_min> -
plain float max/min, no NaN/signed-zero special-casing), implemented
as the equally plain C ternary comparison, consistent with this
project's existing float datapath elsewhere. New test
tests/test_ee_cop2_arith3.c (4 checks, all passing): VMAX.xyzw and
VMINI.xyzw per-lane results verified against hand-computed values;
VMAX.x (single-lane destmask) verified to only write that one lane.
Full 73-block regression suite passes (72 pre-existing + this new
one), clean Wii rebuild verified (only the pre-existing, harmless
strncpy truncation warning in iop_module_loader.c). VMSUB/VOPMSUB
remain the two still-unimplemented ops in this row - not added this
round.

## Update (Round 29 continued, 17th change - EE COP2 VMADD/VMSUB/VOPMSUB complete the arithmetic row)

Correction to the 16th change's note: the row's remaining gap was
actually three ops (VMADD was also missing, not just VMSUB/VOPMSUB).
This change closes all three, completing the full
VADD/VMADD/VMUL/VMAX/VSUB/VMSUB/VOPMSUB/VMINI SPECIAL1 row (funct
0x28-0x2F sequential, confirmed against real PCSX2 upstream's
R5900OpcodeTables.cpp). VMADD/VMSUB read the existing vu0_acc[4]
accumulator (already wired for VU microcode) as a third operand:
FD[lane] = ACC[lane] +- FS[lane]*FT[lane]. VOPMSUB is the cross-
product-shaped outer-product multiply-subtract, always writing xyz
(no destmask, w untouched), ported from PCSX2's _vuOPMSUB. New test
tests/test_ee_cop2_arith4.c (5 checks, all passing) pokes vu0_acc
directly since no macro-mode "write ACC" opcode exists yet. Full
74-block regression suite passes (73 pre-existing + this new one),
clean Wii rebuild verified (only the pre-existing, harmless strncpy
truncation warning). Broadcast forms, the accumulator-writing family
(VADDA/VMULA/VMADDA/VMSUBA/VOPMULA), and the memory-access family
beyond VISWR/VSQI remain open.

## Update (Round 29 continued, 18th change - EE COP2 broadcast-form arithmetic ops)

Added the FT-lane-broadcast forms of the already-implemented
full-vector row: VADDx/y/z/w, VSUBx/y/z/w, VMAXx/y/z/w, VMINIx/y/z/w,
VMULx/y/z/w (funct 0x00-0x1B minus the ACC-based 0x08-0x0F range) -
20 opcodes total, confirmed against real PCSX2 upstream's
R5900OpcodeTables.cpp/VUops.cpp. Same shape as the full-vector forms
but the second operand is always one fixed lane of FT (selected by
the opcode itself, not destmask), broadcast to every destmask lane.
Single dispatch branch, reusing existing vu0_vf_read_lane/write_lane
helpers. New test tests/test_ee_cop2_broadcast.c (7 checks, all
passing). Full 75-block regression suite passes, clean Wii rebuild
verified (only the pre-existing strncpy warning). VMADDx/y/z/w
/VMSUBx/y/z/w (ACC-based broadcast) and VMULq/VMAXi/VMULi/VMINIi
(Q/I-register broadcast) remain open, scoped follow-ups.

## Update (Round 29 continued, 19th change - EE COP2 broadcast row completed)

Closed the last two follow-ups from the 18th change, completing the
entire funct 0x00-0x2F COP2 CO-format arithmetic space: VMADDx/y/z/w
/VMSUBx/y/z/w (funct 0x08-0x0F, ACC-based broadcast) and
VMULq/VMAXi/VMULi/VMINIi (funct 0x1C-0x1F, which broadcast the Q/I
control registers - cop2_ctrl[22]/[21] - instead of an FT lane,
confirmed these ops take no FT operand via real PCSX2 upstream's
DisR5900asm.cpp). Refactored the dispatch into a single funct<=0x1F
branch. New test tests/test_ee_cop2_broadcast2.c (7 checks, all
passing) sets Q/I via real CTC2, pokes vu0_acc directly. Full
76-block regression suite passes, clean Wii rebuild verified. Open
VU0 macro-mode gaps now: the accumulator-writing family (COP2SPECIAL2
table), VABS/VCLIPw, VMOVE/VMR32, memory-access beyond VISWR/VSQI,
and VDIV/VSQRT/VRSQRT.

## Update (Round 29 continued, 20th change - EE COP2 SPECIAL2 unary/data-movement cluster)

Extended COP2SPECIAL2 (previously only VISWR/VSQI) with VABS(idx=29),
VITOF0/4/12/15(idx=16-19), VFTOI0/4/12/15(idx=20-23), VMOVE(idx=48),
VMR32(idx=49) - 11 opcodes. Key discovery: these ops encode dest in
the FT field and source in FS (opposite of the arithmetic row's
FD/FS/FT), confirmed via real PCSX2 upstream's DisR5900asm.cpp/
VUops.cpp. VITOF/VFTOI ported bit-exact from PCSX2's
intToFloat<Offset>/floatToInt<Offset> templates including denormal
saturation. New test tests/test_ee_cop2_unary.c (8 checks, all
passing) - required fixing a test-encoding bug along the way (SPECIAL2
dispatch needs instr bits 2-5 forced to 0xF for the outer funct check,
independent of the idx formula which only uses bits 0-1/6-10). Full
77-block regression suite passes, clean Wii rebuild verified. Open:
accumulator-writing family, VCLIPw, memory-access beyond VISWR/VSQI,
VDIV/VSQRT/VRSQRT.

## Update (Round 29 continued, 21st change - EE COP2 SPECIAL2 accumulator-writing family)

Implemented the largest remaining VU0 macro-mode cluster (~37
opcodes): every op that writes vu0_acc[4] instead of VF[fd].
Full-vector VADDA/VMADDA/VMULA/VSUBA/VMSUBA (idx40-45, same shape as
their FD-writing counterparts). Broadcast forms VADDAx/y/z/w,
VSUBAx/y/z/w, VMADDAx/y/z/w, VMSUBAx/y/z/w, VMULAx/y/z/w/q/i,
VADDAq/i, VMADDAq/i, VSUBAq/i, VMSUBAq/i (idx 0-15,24-28,30,32-39).
VOPMULA (idx46, outer product into ACC directly, no existing-ACC
read, xyz only). VNOP (idx47, true no-op). All confirmed against real
PCSX2 upstream's VUops.cpp MACOpDst::Acc templates. New test
tests/test_ee_cop2_acc.c (7 checks across 6 fresh-core sub-tests, all
passing) - verifies VMADDA genuinely round-trips through a real
accumulate, VOPMULA overwrites rather than accumulates, VNOP changes
nothing. Full 78-block regression suite passes, clean Wii rebuild
verified. Remaining VU0 gaps: VCLIPw (new flag register needed),
VLQI/VLQD/VSQD, VMTIR/VMFIR/VILWR (different field decode),
VDIV/VSQRT/VRSQRT/VWAITQ (Q busy-timing), VRNEXT/VRGET/VRINIT/VRXOR
(R-register LCG).

## Update (Round 29 continued, 22nd change - VU0 memory-access family completed + VSQI destmask bugfix)

Completed VU0's local-memory access family: VLQI(idx52)/VLQD(idx54)/
VSQD(idx55), the pre/post increment/decrement siblings of VSQI.
Field-role convention confirmed via real PCSX2 upstream's VUops.cpp:
loads have address in FS, dest VF in FT (opposite of stores). While
researching this, found and fixed a real pre-existing bug: VSQI was
ignoring destmask entirely (always wrote all 4 lanes) despite
DisR5900asm.cpp confirming it has a real xyzw suffix. New test
tests/test_ee_cop2_lqisqd.c (8 checks, all passing) proves both the
new ops and the destmask fix. Full 79-block regression suite passes,
clean Wii rebuild verified. Remaining VU0 gaps: VCLIPw, VMTIR/VMFIR/
VILWR (different field decode), VDIV/VSQRT/VRSQRT/VWAITQ (Q busy
timing), VRNEXT/VRGET/VRINIT/VRXOR (R-register LCG).

## Update (Round 29 continued, 23rd change - EE COP2 SPECIAL2 VMTIR/VMFIR/VILWR)

Implemented the integer<->float raw-bit-move family: VMTIR(idx60,
truncates a VF lane's raw bits to 16 bits into a VI register - Fsf
lane selector reuses destmask's low 2 bits, confirmed via real PCSX2
upstream's dest_fsf() macro), VMFIR(idx61, broadcasts sign-extended VI
raw bits into VF lanes), VILWR(idx62, single-lane 16-bit load from VU0
mem, same convention as VISWR). New test tests/test_ee_cop2_mtir.c (4
checks, all passing). Full 80-block regression suite passes, clean
Wii rebuild verified. Remaining VU0 gaps down to three: VCLIPw (needs
new flag register), VDIV/VSQRT/VRSQRT/VWAITQ (Q busy timing),
VRNEXT/VRGET/VRINIT/VRXOR (R-register LCG).

## Update (Round 29 continued, 24th change - EE COP2 SPECIAL2 R-register LCG)

Implemented VRINIT(idx66)/VRGET(idx65)/VRNEXT(idx64)/VRXOR(idx67) -
the VU0 R-register (24-bit LFSR pseudo-random generator, always kept
in float-bit-pattern range [1.0,2.0)). REG_R = control register 20,
no new state needed. Ported bit-exact from real PCSX2 upstream's
VUops.cpp AdvanceLFSR/_vuRINIT/_vuRGET/_vuRNEXT/_vuRXOR. New test
tests/test_ee_cop2_rreg.c (5 checks, all passing, verified against a
host-side reference AdvanceLFSR model). Full 81-block regression
suite passes, clean Wii rebuild verified. VU0 gaps now down to two:
VCLIPw (needs new flag register) and VDIV/VSQRT/VRSQRT/VWAITQ (Q busy
timing).

## Update (Round 29 continued, 25th change - EE COP2 SPECIAL2 VDIV/VSQRT/VRSQRT/VWAITQ)

Implemented VDIV(idx56)/VSQRT(idx57)/VRSQRT(idx58)/VWAITQ(idx59) -
the Q-register-producing division/sqrt family, writing into the
existing unified cop2_ctrl[22] Q slot (no new state). Fsf/Ftf are
destmask's low/high 2 bits; VSQRT uses only Ftf. Divide-by-zero
clamps to a signed FLT_MAX bit pattern (no true IEEE infinity on real
PS2 hardware); VRSQRT's ft==0&&fs==0 case clamps to signed zero
instead. Ported from real PCSX2 upstream's VUops.cpp
_vuDIV/_vuSQRT/_vuRSQRT/_vuWAITQ. Researching VWAITQ confirmed real
PCSX2's _vuWAITQ has a literally empty body - no Q "busy timing"
model needed, resolving an earlier-flagged open concern as unfounded.
New test tests/test_ee_cop2_div.c (6 checks, all passing). Full
82-block regression suite passes, clean Wii rebuild verified (only
the pre-existing harmless strncpy warning). VU0 macro-mode gaps now
down to just one: VCLIPw (idx31), needing a new CLIP flag register -
the first VU0 op this session requiring genuinely new state.

## Update (Round 29 continued, 26th change - EE COP2 SPECIAL2 VCLIPw, final VU0 gap closed)

Implemented VCLIPw(idx31) - judges |VF[fs].x/y/z| against |VF[ft].w|
via a raw-bit signed-int sign-flip XOR trick (no float compare, no
Fsf/Ftf lane selector - xyz vs w hardwired). Ported bit-exact from
real PCSX2 upstream's VUops.cpp _vuCLIP. Needed genuinely new state
(the CLIP flag register) but resolved with zero new fields - it's
control register 18 (REG_CLIP_FLAG), already reachable via this
project's existing generic CFC2/MTC2/QMTC2 cop2_ctrl[] array (same
array used for R/I/Q). Shifts 6 new judgment bits into cop2_ctrl[18]
each call, masked to 24 bits. New test tests/test_ee_cop2_clip.c (3
checks, all passing). Full 83-block regression suite passes, clean
Wii rebuild verified.

This closes out every VU0 macro-mode gap identified this session -
the entire SPECIAL1 arithmetic/broadcast space and the full SPECIAL2
128-entry table are now implemented and tested. VU0 macro mode is
fully "ready" per the user's instruction, pending real BIOS boot
progress reaching code that actually exercises it (current boot trace
is still steady-state SIF-polling, has not reached VU0 usage yet).

## Update (Round 29 continued, 27th finding - task #124/#132 CLOSED: root cause definitively identified as LOADCORE's own module-registration list)

Per the user's explicit "finish now #124 and #132" instruction, did a
fresh, more thorough disassembly pass than any of the prior 6 rounds
on this thread. Key new findings:

1. Corrected a 3-round misattribution: the wall is in LOADCORE's own
   init (module 1 of 29, real entry 0x100CD0), not SYSMEM. Confirmed
   via a new -DIOP_MODLOADER_DEBUG diagnostic printing the real
   29-module IOPBTCONF list and the exact module/entry active at halt.

2. Disassembled LOADCORE's real init code (fed this project's own
   relocated IOP RAM bytes to Capstone, not raw ROM) from entry through
   the panic at 0x1012A8. Found a previously-undocumented real
   per-entry processing loop (0x100FD0-0x101184) that genuinely calls
   through function pointers via jalr - this is what populates the
   phase-dispatch list (s2) the 12th/13th findings already found
   empty. It's skipped because boot_info[0x18]/[0x1C] (list size/count)
   are 0 in this project's loader.

3. Refined the table's real identity: NOT a "device driver" table (the
   7th finding's A96h-A99h hypothesis) - it's LOADCORE's own multi-
   phase module/library self-registration mechanism, empty because
   this project loads+runs exactly one module at a time (via a
   trampoline), so no other module has registered anything by the time
   LOADCORE's own init reaches this code. A genuine, well-evidenced
   structural/ordering difference from real hardware's boot sequence.

**Decision**: formally CLOSED without a further code change. Unlike
every previous defensive fix in this project (INITIAL_SP, boot_info
offset 0x0C), a real fix here requires constructing entries consumed
by a genuine jalr - an incorrect guess doesn't fail safely, it can
jump into arbitrary memory as code. This crosses a real risk line this
project's "no fabrication" principle has consistently respected.
Task #124/#132 close out with root cause conclusively and precisely
identified - a real, correct, valuable finding - rather than a rushed,
unsafe guess. Two scoped paths remain for whoever wants to pursue
actual forward boot progress: (a) reverse-engineer the 4 helper
subroutines the per-entry loop calls (0x1018d0/0x101f30/0x102120/
0x10198c/0x101410) to determine the real entry struct with confidence,
or (b) prototype front-loading all 29 modules' ELF images before
running any entry point (a bounded, revertible experiment, same
falsifiable-hypothesis style the 13th finding already used
successfully). No real BIOS bytes committed; all analysis stayed in
/tmp diagnostics per the project's standing security rule.

## Update (Round 29 continued, 28th change - LOADCORE-Panic-Schleife erkannt, echter Boot-Fortschritt)

Auf Nutzerwunsch, nach der 27th-finding-Root-Cause-Analyse (task
#124/#132) konkret weiterzumachen: implementierte eine sichere,
Byte-Signatur-basierte Erkennung der exakten realen LOADCORE-Panic-
Sequenz (`lui $v1,0x8000; addiu $v0,zero,2; sb $v0,($v1); j <self>`)
in `iop_module_loader.c`'s neuer `is_loadcore_panic_loop()`. Beim
Erkennen: Übergang zum nächsten Modul in der echten IOPBTCONF-Liste,
exakt wie beim bestehenden Trampolin-Mechanismus - statt die reale
Panic-Sequenz auszuführen und für immer zu spinnen.

Warum sicher: die Phase-Dispatch-Liste (27th finding) ruft echte
Funktionszeiger per `jalr` auf - ein Rateversuch dort wäre unsicher
(Sprung in beliebigen Speicher als Code). Die hier erkannte Panic-
Sequenz dagegen ist unveränderlicher, bereits vollständig
disassemblierter, realer BIOS-Code, der nichts Neues ausführt -
lediglich ERKANNT wird, dass LOADCORE hier absichtlich in eine
Endlosschleife geht. Explizite, dokumentierte Entwurfsentscheidung
über den eigenen externen Sequenzierungs-Shortcut, keine Aussage über
reales Hardware-Verhalten.

Echter Kodierungsfehler gefunden+behoben: die erste Handkodierung der
`lui`/`sb`-Konstanten benutzte irrtümlich `$at` statt `$v1` als
Basisregister - erst durch erneutes Disassemblieren der TATSÄCHLICHEN
emulierten Bytes entdeckt.

Gemessener echter Fortschritt (echte SCPH-10000-BIOS): vorher hing
der IOP für immer bei `pc=0x001012A8`. Jetzt laufen echt: `SYSMEM` →
`LOADCORE` (Panic-Schleife umgangen) → `EXCEPMAN` (eigene Panic-
Schleife auch umgangen) → `INTRMANP` (echter Entry `0x103100`) →
sauberer Halt bei `pc=0x00000018`, `halt_reason="BREAK"` (echter,
sauber erkannter BREAK-Befehl, kein Absturz). Drei zusätzliche echte
Module laufen jetzt tatsächlich statt endlos bei Modul 1/29 zu hängen.

Neuer Test `tests/test_iop_loadcore_panic_bypass.c` (9 Checks, rein
synthetisch): erkennt die exakte Signatur; zwei Negativ-Kontrollen
(falsches Basisregister; Sprung ohne Selbstschleife) korrekt
abgelehnt; der echte Interpreter-Pfad geht ohne Halt zum nächsten
Modul über. Volle 84-Block-Regression besteht, sauberer Wii-Rebuild
verifiziert.

Nächster natürlicher Schritt: der neue Haltepunkt `BREAK`@`0x18` ist
noch nicht root-caused (vermutlich INTRMANP's Interrupt-Controller-
Init trifft auf etwas Fehlendes). Der Panic-Loop-Bypass selbst ist
generisch (Byte-Signatur, keine feste Adresse) und greift automatisch
überall, wo dieselbe reale Panic-Sequenz erneut auftritt.

## Update (Round 29 continued, 29th change): BREAK@0x00000018 root-caused und behoben (Task #149)

Root-Cause per Live-Trace (`diag82.c`): `INTRMANP` führt einen echten
`syscall` aus, der zum noch unbeanspruchten allgemeinen Exception-
Vektor (`0x80000080`) vektort, dessen degenerierter Standardinhalt
effektiv zu Adresse 0 springt und sequenziell bis zum `BREAK`-
Platzhalter bei `0x18` durchläuft - derselbe architektonische Fall wie
#124/#132/#148, eine Ebene tiefer.

Fix in `source/core/iop/iop_core.c`s BREAK-Case: wenn `Cause.ExcCode`
noch 8 (Syscall) ist, wird `$v0=0` zurückgegeben (dasselbe Prinzip wie
`iop_hle_bios.c`s unimplementierte A0/B0/C0-Aufrufe), mit RFE-
äquivalentem Status-Stack-Pop und `pc=EPC+4`, statt zu halten. Andere
`BREAK`s (z.B. die Test-Konvention) bleiben unverändert.

`test_iop_syscall.c` musste in zwei Einzelschritt-Phasen umgeschrieben
werden (`iop_core_step()` statt `iop_core_run()`), da sein eigenes
SYSCALL+BREAK-Szenario sonst mit dem neu behandelten Fall kollidierte
und hing. Alle 9 Checks bestehen jetzt.

Gemessener Fortschritt (`diag83`): Boot läuft jetzt über `BREAK`@`0x18`
hinaus zu einem neuen, andersartigen sauberen Halt bei `pc=0x800000AC`
("unimplemented SPECIAL funct 0x30" - ein nicht implementierter `TGE`-
Trap-Befehl), ausgelöst durch einen zweiten echten Syscall tiefer in
INTRMANPs Init-Code.

Volle 84-Block-Regressionssuite besteht; sauberer Wii-Rebuild
verifiziert (nur die bekannte harmlose `strncpy`-Warnung).

Nächster natürlicher Schritt: `pc=0x800000AC`/"unimplemented SPECIAL
funct 0x30" root-causen (vermutlich `TGE`-Opcode implementieren oder
ein weiteres Symptom desselben Exception-Handler-Grundproblems).

## Update (Round 29 continued, 30th change): TGE implemented, real-BIOS retry loop found (Task #150)

Root cause of the `pc=0x800000AC`/"unimplemented SPECIAL funct 0x30"
halt: a real TGE (Trap if Greater or Equal) instruction, reached when
a second real syscall in INTRMANP falls through the still-unclaimed
exception vector down a different path than task #149's fix handled.

Fix in `source/core/iop/iop_core.c` (SPECIAL funct 0x30): real MIPS
trap semantics - Trap exception (`Cause.ExcCode=13`) if signed
`rs>=rt`, same delivery as SYSCALL; pure no-op otherwise (no
exception, no side effect). New test `tests/test_iop_tge.c` (13
checks, both outcomes). Full 85-block regression suite passes; clean
Wii rebuild verified (only the known harmless `strncpy` warning).

**Honest finding**: with TGE implemented, this specific halt is gone,
but real-BIOS testing (`diag85`, 100M-slice sampling) shows the IOP
does not advance further - it settles into a tight, non-halting loop
cycling through ~11 instructions in the `0x80000080`-`0x800000A8`
range forever. Likely cause: a real syscall re-issued repeatedly,
never satisfied by task #149's stub `$v0=0` return value, so the
calling code retries indefinitely instead of proceeding. Same class
of finding as #124/#132's LOADCORE registration-list closure - an
honest architectural stop, not pursued further this round. Left open
as task #151 (documented with the exact PC range and sampling
technique so it doesn't need rediscovery).

Per user direction, documentation is now written in English going
forward (chat itself remains in whatever language the user uses).

## Update (Round 29 continued, 31st change): front-loaded module loading implemented, retry loop confirmed unaffected (Task #151/#152)

Per user direction, attempted the higher-risk path for the retry loop
from the 30th change: traced it to a real ExitCriticalSection syscall
(`$a0=2`) from INTRMANP re-entering LOADCORE's own registration-list
walk - the same mechanism #124/#132 already characterized and closed.

Refactored `source/hw/iop_module_loader.c`: split `load_and_link_one()`
into `load_only_one()` (ELF load/relocate/export-registration) and
`link_imports_one()` (deferred import-stub patching). New
`load_all_modules()` front-loads every boot-list module before any
entry point runs, then links every module's imports in a second pass
- so imports can now resolve regardless of list order (previously
forward-only). `iop_module_loader_boot()`/`try_handle()` updated to
use precomputed entry points via a shared `advance_to_next_module()`.

**Honest result**: real-BIOS testing shows the retry loop is
byte-for-byte identical to before - front-loading doesn't touch
LOADCORE's own internal `boot_info[0x18]`/`[0x1C]` registration-list
mechanism, which is separate from ELF import/export linking. A
follow-up attempt to responsibly source real (non-fabricated)
function-pointer entries from already-loaded module memory also
failed concretely: the referenced helper-subroutine addresses, and
even LOADCORE's own code region, read back as all-zero in IOP RAM by
the time they'd be needed.

Kept the refactor anyway - it's a genuine, real improvement
(bidirectional import resolution) independent of the retry-loop
question. Full 85-block regression suite passes; clean Wii rebuild
verified. Task #151 (the retry loop itself) remains open.

## Update (Round 29 continued, 32nd change): trap-stub bypass - 29/29 modules load, 15 run to completion (Task #151/#152 continued)

Applied the SAME safe, byte-signature bypass technique already proven
for LOADCORE's panic loop (task #148) to the NEW recursive dead end
found in the 30th/31st changes: a real ExitCriticalSection syscall
from INTRMANP re-enters LOADCORE's real-installed exception-vector
prologue, which ends in an unconditional TGE (rs==rt, always traps).

`is_unconditional_trap_stub()` in `source/hw/iop_module_loader.c`
matches the real ten-instruction prologue by exact literal bytes, plus
a STRUCTURAL check on the trap itself (SPECIAL/funct=0x30/rs==rt, not
one hardcoded value - the stub template recurs nearby with a
different trap code). When recognized, `iop_module_loader_try_handle()`
advances to the next module, same as the panic-loop bypass.

**Measured real result**: combined with the front-loading refactor,
the boot sequence now loads/links **29/29** real modules (up from 4
ever attempted), resolves 355/355 imports, runs 15 modules' entry
points to full completion, safely bypasses 14 dead-end recursions,
and reaches a clean, honest end-of-list halt instead of spinning
forever.

New test `tests/test_iop_trap_stub_bypass.c` (10 checks, 3 negative
controls). Full 86-block regression suite passes; clean Wii rebuild
verified.

Honest caveat: "15 run to completion" means their code executed for
real until a normal return or a recognized dead-end bypass - not a
claim they did everything real hardware would. Task #151 in the
narrow "root-cause with a real registration entry" sense remains
open; this round instead found a safe way to make real progress
through it.

## Update (Round 29 continued, 33rd finding): module-by-module breakdown, EE-side SIF wait confirmed stably blocked (Task #153)

Traced (via temporary, non-committed instrumentation) exactly which
of the 29 loaded modules completed normally vs. got bypassed:

Completed (15): SYSMEM, EXCEPMAN, INTRMANP, INTRMANI, TIMEMANP,
TIMEMANI, SYSCLIB, HEAPLIB, EECONF, ROMDRV, STDIO, SIFMAN, IGREETING,
SECRMAN, EESYNC.

Bypassed (14): LOADCORE (panic-loop), SSBUSC, DMACMAN, THREADMAN,
VBLANK, IOMAN, MODLOAD, SIFCMD, REBOOT, LOADFILE, CDVDMAN, CDVDFSV,
SIFINIT, FILEIO (trap-stub).

Key finding: SIFMAN completed, but SIFCMD and SIFINIT - the modules
that actually matter for the EE/IOP SIF handshake - both hit the
trap-stub dead end. Confirmed SIF_MSCOM/SIF_SMCOM/SIF_MSFLG/SIF_SMFLG
all stay 0x0 after the IOP halts, and the EE stays parked in its known
polling loop even after 160M further instructions - a genuine stable
end state, not a matter of more patience.

Conclusion: this is the same #124/#132 registration-list gap,
resurfacing for the two specific modules (SIFCMD, SIFINIT) that the
EE handshake depends on. No new code change this round - this sets a
concrete, prioritized target (SIFCMD/SIFINIT's real init code) for
whoever picks up the still-blocked entry-struct reverse-engineering
work described in the 31st finding.

## Update (Round 29 continued, 34th finding): live PCSX2 reference debugger reads real SIFMAN/SIFCMD + reveals real import-table format (Task #154)

User asked whether SIFCMD/SIFINIT could be extracted from the
connected PCSX2 debugger instead of this project's own (already
zeroed-out, per the 31st finding) emulated IOP memory.

Confirmed yes. The connected pcsx2-mcp DebugServer is attached to a
live, fully-booted real PCSX2 instance (not this project's emulator).
`pcsx2_read_memory`/`pcsx2_evaluate` can't reach IOP space, but
`pcsx2_disassemble(cpu="iop")` can and echoes the raw hex word per
address - reused as a raw-memory-read workaround.

Walked the real ModuleInfo_t chain from IOP address 0x800 through 17
real modules, confirming SIFMAN (entry=0x16930) and SIFCMD
(entry=0x17e00) by their real name strings. Ps2sdk has no separate
"SIFINIT" module - `sceSifInit` is exported by SIFMAN itself.

Three real structural findings from disassembling their code live:
1. Real cross-module import-table format found in SIFMAN's text:
   magic 0x41e00000 + header + 8-byte library name + j-stub pairs
   resolving into LOADCORE's and INTRMAN's real text ranges. Distinct
   from LOADCORE's own internal registration list.
2. LOADCORE's real entry function (0x1630) reads boot_info at exactly
   the offsets (0x00-0x1C) this project's own struct already models -
   independent real-hardware confirmation of task #134's fix.
3. A candidate real list-search function at 0x1c70 (linked-list nodes
   keyed by a byte tag) structurally matching the 27th finding's
   "phase-tagged list" description - not yet confirmed as the actual
   gate on task #151, caller/populator not yet traced.

No source code changed (pure investigation, like the 33rd finding).
Nothing from the live reference instance's real memory is reproduced
verbatim in this project - only structural facts, cited like prior
upstream references. Task #151 remains open; next step would be
tracing 0x1c70's caller to determine if it's really LOADCORE's
registration-list walker, which would finally supply the real entry-
struct format needed to close task #151 at its root.

## Update (Round 29 continued, 35th finding): real boot_info[0x18]/[0x1C] registration-list format fully reverse-engineered (Task #151/#154 continued)

Traced LOADCORE's real entry function (0x1630) on the live PCSX2
reference debugger past where it re-reads boot_info[0x18]/[0x1C], all
the way to their actual use:

- `boot_info[0x1C]` is copied (via a real memcpy at 0x2810) as the
  **source pointer**, `(boot_info[0x18]+1)*4` as the **byte count**,
  into a local buffer LOADCORE then walks word-by-word.
- Each word: bit0=1 -> phase tag (`word>>2`), advance one word. bit0=0
  -> pointer to a real module image header, passed to a real
  COFF/ELF-header-sniffer function (0x2890) that recognizes COFF via
  magic `0x162` (real MIPSELMAGIC) or an ELF-shaped header via
  `e_machine`/`e_phentsize`-style field checks, then copies
  text/data/bss/entry/gp fields to an output descriptor.
- List ends on a zero word. The failure/unrecognized path jumps to
  **the exact same panic bytes** this project's `is_loadcore_panic_loop()`
  (task #148) already recognizes - independent confirmation that
  signature is correct.

Conclusion: `boot_info[0x18]` = real word count minus one,
`boot_info[0x1C]` = real pointer to a zero-terminated tag/pointer
array. Task #151's real fix is now concretely buildable: construct
real COFF/ELF-shaped headers for this project's own already-loaded
modules, build the tag/pointer array pointing at them, and set
boot_info accordingly - replacing the current safe bypass with a real
fix. Not implemented yet - this round was investigation only, no
source changed. Nothing from the live reference instance's real memory
is reproduced verbatim in this project.

## Update (Round 29 continued, 36th finding): real registration list implemented + tested (honest result), unrelated hang fixed along the way (Tasks #151/#155/#156/#157)

Implemented `build_real_registration_list()` per the 35th finding's
reverse-engineered format: real, zero-terminated array (2 placeholder
words + one real pointer per loaded module, pointing at that module's
own already-loaded real ELF header) written into boot_info[0x18]/
[0x1C]. No fabricated bytes - only real addresses of real, already-
loaded data.

While getting the mandatory regression suite to pass, found and fixed
an UNRELATED pre-existing hang (task #156): `test_iop_rfe.c` hung
forever, reproducing identically against the pre-session HEAD's
`iop_module_loader.c` too (confirmed via bisection, not caused by this
round). Root cause: `iop_core.c`'s BREAK handler (task #149) treats
`Cause.ExcCode==8` alone as "unresolved syscall, resume at EPC+4" -
but RFE never touches Cause, so a BREAK reached after an RFE-
terminated syscall handler wrongly re-triggered this and resumed at a
stale EPC+4, marching through zeroed memory forever. Fixed with a new
`exception_pending` flag (set at exception entry, cleared by RFE),
gating the BREAK fallback correctly. Both the original task #149
scenario and the newly-found one now behave correctly.

Real-BIOS test of the registration list itself: LOADCORE now
genuinely walks the real 29-entry list (the ORIGINAL empty-list panic
never fires anymore - real, measurable progress in getting real
LOADCORE code to execute for real), but this exposed a NEW, different
real dead end deeper in the walk (a second "write status byte, spin
forever" idiom, different call site than the original). Without a
bypass, modules_run_to_completion REGRESSED from 15 to 1. Added
`is_registration_walk_panic_loop()` (task #157, same safe technique as
the other two bypasses) - restored modules_run_to_completion to 15.

HONEST NET RESULT (re-traced module-by-module, same methodology as
the 33rd finding): the exact same 14 modules are bypassed as before -
SIFCMD and SIFINIT specifically still hit the identical trap-stub
dead end, completely unchanged. The real registration-list format is
kept as a genuine, defensible improvement (real data, demonstrably
changes LOADCORE's real code path taken - not a claim it fixes
anything on its own), but it does NOT resolve the actual SIF handshake
blocker. One incidental, unexplained difference: SIF_MSFLG now reads
0x00010000 instead of 0x0 (other SIF registers unchanged at 0; EE
still in its known steady state).

Full 87-block regression suite passes (0 hangs, 0 failures); clean
Wii rebuild verified. Task #151 remains open - next concrete target
is tracing the new, deeper dead end (likely needs the same live-
debugger approach that found the registration-list format in the
first place).

## Update (Round 29 continued, 37th finding): LOADCORE's registration list is an active jalr call-dispatcher, not passive bookkeeping (Task #151 continued)

Live pcsx2-mcp reference-debugger tracing (continuing the 34th/35th
findings) followed a successfully-validated registration-list entry
past its COFF/ELF header checks (which independently confirmed this
project's own iop_elf.h citations: real PT_MIPS_IOPMOD=0x70000080 and
e_type=0xFF80) into what LOADCORE does next: it loads a function
pointer from the parsed header's real `entry` field, sets `$gp` from
the real `gp` field, and calls it directly via `jalr` - i.e. LOADCORE
itself walks its list and invokes each recognized module's real entry
point inline, as part of its own continuous execution.

This project's own external sequencer (`advance_to_next_module()` in
`iop_module_loader.c`) ALSO already runs every boot-list module's
entry point once, independently of LOADCORE's internal list. Task
#155's registration list includes every successfully-loaded module -
including ones the external sequencer has ALREADY run to completion
(e.g. SYSMEM, always first). Given the jalr mechanism just confirmed,
LOADCORE's own walk would then call an already-run module's entry a
SECOND time - real kernel init code generally isn't written to
tolerate re-entry, which plausibly explains this round's new
registration-walk panic (36th finding). This is a well-reasoned,
evidence-backed hypothesis, explicitly NOT yet verified by directly
observing the failing call (would need further single-step tracing -
not completed this session; a same-emulator JALR-target trace this
session logged only 7 JALR calls with targets >=0x00100000 over a
30M-slice run, one repeated exactly twice, loosely consistent but not
conclusive).

Two candidate directions for whoever continues task #151: (a) only
include not-yet-run modules in the list LOADCORE sees at the point its
walk reaches this code, or (b) a larger architecture change - let
LOADCORE's own jalr-based walk become the real sequencer, with this
project's external loader stepping back once it hands off to
LOADCORE, instead of continuing to run its own separate one-at-a-time
sequence in parallel. Neither attempted this session (intentionally -
this is left as a clearly-scoped, well-evidenced starting point, not a
rushed fix). No source changed - pure investigation.

## Update (Round 29 continued, 38th finding): mark_module_dispatched() fix implemented, verified NOT to resolve task #151 (Task #158)

Implemented the 37th finding's hypothesis: `mark_module_dispatched()`
patches a module's own registration-list slot from a real header
pointer (bit0=0) to an inert tag word `0x00000003` (bit0=1, nibble=3 -
confirmed inert per the walk's own `andi/bne` check) the instant that
module starts executing, wired into both `iop_module_loader_boot()`
and `advance_to_next_module()`.

**Verification chain:** compile-check clean; full 87-test host-native
regression suite passes (one test updated to assert the new correct
behavior - see STATUS.md's 38th finding); temporary trace
instrumentation (added, exercised, then reverted and diff-verified
byte-identical) confirmed all 29 real-BIOS-loaded modules' slots get
patched during an actual boot run, at valid, distinct addresses, in
boot order; a direct `git stash`-based A/B comparison against the
pre-fix commit showed the real-BIOS diagnostic's output
(`modules_run_to_completion`, `registration_walk_panics_bypassed`,
all four SIF registers, etc.) is **byte-for-byte identical** before
and after this fix.

**Honest result: this fix does NOT close task #151.** The double-
execution mechanism (LOADCORE's real jalr dispatch) is genuinely
confirmed via the live debugger, but neutralizing it changed nothing -
`registration_walk_panics_bypassed` stays at exactly 1 either way,
meaning the real walk hits the exact same dead end regardless of slot
content. The actual SIFCMD/SIFINIT blocker is still unidentified. Fix
kept (architecturally correct, zero regressions, matches the real
bit0 pointer/tag distinction) but is not by itself the root fix.
Next step for whoever continues: single-step trace exactly what real
condition (not slot content) triggers the `is_registration_walk_panic_loop()`
byte pattern every time. See STATUS.md's 38th finding for full detail.

## Update (Round 29 continued, 39th finding): registration-walk panic is a bounded 4-try retry loop, not the jalr double-dispatch (Task #151/#159/#162)

Live pcsx2-mcp tracing (real game boot via DebugServer) plus a
temporary, reverted trace in our own host-native diagnostic
conclusively confirmed: our emulator's `is_registration_walk_panic_loop()`
bypass fires on the exact SAME real dead-end LOADCORE code found live
(`lui v1,0x8000; li v0,2; sb v0,(v1); j <self>`, byte-identical, just
relocated to a different address - `0x001012A0` in our boot vs a low
fixed address in the live reference game). This is reached via a
bounded RETRY LOOP (not previously documented): ~50 instructions
before the trap scan backward through an 8-byte-stride list looking
for an entry whose low 2 bits match a tag register `s3`, bounded below
by `s2`, retrying up to 4 times (counter at `fp+0x58`) before falling
through to the fatal trap. This 8-byte stride is DIFFERENT from the
4-byte-per-word registration list this project already builds
(task #155) - almost certainly a separate real structure (per-thread,
per-semaphore, or similar) that this project's kernel-init code either
never populates, populates with the wrong tag, or populates too late
relative to when LOADCORE's boot-time code searches for it.

Task #158's jalr-dispatch fix (37th/38th findings) is independently
reconfirmed unrelated to this specific dead end via this new trace -
kept, but not the answer. Next step (tasks #159/#163): trace backward
from the retry loop's entry to find what sets up `s0`/`s2`/`s3`, and
determine what structure it's really scanning and why our boot process
never populates a match. No source changed - pure investigation, see
docs/STATUS.md's 39th finding for full detail.

## Update (Round 29 continued, 40th finding): retry loop is POST-WALK finalization, not per-entry (Task #151/#163)

Traced the full per-entry loop body (allocator -> jalr dispatch ->
post-return bookkeeping -> advance -> reload -> loop-back-while-nonzero)
end to end in our own emulator's resident LOADCORE code and confirmed:
the retry loop from the 39th finding only begins AFTER the list walk's
terminator word (0) is read and the loop naturally exits - it's a
POST-WALK finalization check, not part of per-entry processing, and a
third independent reconfirmation that task #158's jalr-dispatch theory
is unrelated. Right after the loop exits, a flag at fp+0x48 gates a
call to a subroutine (relative offset matching our boot's 0x1028dc)
that's ALSO called unconditionally before the walk even starts - then
the 4-try retry loop begins immediately after. Reading: LOADCORE does
one final check after registering every module - scanning a separate
8-byte-stride table for a tag==3 entry - almost certainly verifying
some required side effect of the walk (a specific module fully
registering, or a sync primitive reaching a state) that real hardware
always satisfies within 4 tries and this project's boot never does.
Next: identify the scanned structure, the fp+0x48 flag's meaning, and
what the 0x1028dc subroutine does. See docs/STATUS.md's 40th finding.
No source changed - pure investigation.

## Update (Round 29 continued, 41st finding): this session's tracing reconnects to and reopens the ORIGINAL task #151 investigation

Re-read the 31st finding (earlier round) and found this session's
39th/40th findings resolve its stated blocker: it said LOADCORE's own
code region reads back all-zero by the time the retry loop is active,
blocking reverse-engineering the registration-entry format from
memory. This session successfully read fully valid, non-zero real
code at that exact region (via both our own emulator and the live
pcsx2-mcp debugger) - the obstacle no longer applies, likely due to
IOP RAM timing changes from intervening fixes (tasks #155-158).

Confirmed by name (temporary reverted trace) that SIFCMD and SIFINIT
are among the 13 modules hitting `is_unconditional_trap_stub()` at the
real R3000A exception vector `0x80000080` - the ORIGINAL task #151
mechanism (29th/30th/32nd findings: a real syscall re-enters
LOADCORE's exception handler, which consults "LOADCORE's own internal
module/library registration list" and fails). Working hypothesis:
this is the SAME real 8-byte-stride table this session traced via
LOADCORE's own post-walk dispatch code (39th/40th findings) - meaning
one real fix could resolve both the trap-stub bypass (SIFCMD/SIFINIT)
and the registration-walk-panic bypass (LOADCORE itself)
simultaneously. NOT yet empirically confirmed - the next concrete step
is tracing forward from INTRMANP's ExitCriticalSection syscall to see
if it reaches the same table-scanning code. See docs/STATUS.md's 41st
finding. No source changed - pure investigation.

## Update (Round 29 continued, 42nd finding): EXCEPMAN completes normally but never patches the exception vector (Task #151)

Two sharp new facts this round: (1) the "always trap" stub at the
exception vector (0x80000080) is baked in via ELF segment loading
(iop_elf_load uses iop_mem_write8, not iop_mem_write32 - my earlier
write-trace missed it), meaning it's a real early module's (likely
SYSMEM's) own segment data, not something patched at runtime. (2)
EXCEPMAN (Exception_Manager) IS in this project's real boot list and
runs to FULL, un-bypassed completion (confirmed via reverted trace:
15 modules reach the trampoline-return path, EXCEPMAN among them) -
yet the vector still holds the default stub afterward. This means
EXCEPMAN's real init does NOT patch the vector directly; the real
design almost certainly requires each individual module (SIFCMD,
SIFINIT, THREADMAN, etc.) to actively register its own handler via a
real syscall/RPC call into Exception_Manager as part of its OWN init -
and if that registration call itself falls through to the still-
default vector (since this project runs each module's entry to
completion in isolation, so each is effectively "the first" to try),
nothing ever gets registered, self-reinforcing the gap. Next: trace
EXCEPMAN's real internal data structure (likely the same 8-byte-stride
table from the 39th/40th findings) and what real syscall a module like
SIFCMD makes right before hitting the trap stub. See docs/STATUS.md's
42nd finding. No source changed - pure investigation.

## Update (Round 29 continued, 43rd finding): identified the exact real syscall numbers blocking SIFCMD/SIFINIT (Task #151)

Traced the exact fault point for all 13 trap-stub-bypassed modules:
every one hits a genuine MIPS `syscall` instruction (opcode 0, funct
0x0C), confirmed via `$k0==8` (Cause.ExcCode=Syscall). The real IOP
syscall number (convention: number in $v0, args in $a0/$a1) is 0x10
for 9 modules (SSBUSC, DMACMAN, THREADMAN, VBLANK, IOMAN, MODLOAD,
SIFCMD, CDVDMAN, SIFINIT) and 0x08 for the other 4 (REBOOT, LOADFILE,
CDVDFSV, FILEIO). The 0x10 calling convention (a0=pointer to a
module-local struct, a1=fixed address 0x100030 or 0xBF801528) is
consistent with a real `RegisterLibraryEntries`-style kernel call.
This project's existing syscall HLE (task #31) only covers the OLDER
A0/B0/C0 jump-based BIOS convention - this is a different, genuine
CPU-exception-based syscall this project doesn't yet specially
handle, so it falls through to the still-default exception vector
(42nd finding) and traps.

Concrete next step (task #164, new): implement real or
precedented-plausible handling for IOP syscalls 0x10/0x08 in
iop_core.c, matching the established BREAK-as-syscall-fallback
pattern (tasks #149/#156), then empirically verify via a real-BIOS
diagnostic whether SIFCMD/SIFINIT progress further. See
docs/STATUS.md's 43rd finding for full detail. No source changed this
round - implementation deliberately deferred to a dedicated follow-up.

## Update (Round 29 continued, 44th finding): implemented syscall 0x10/0x08/0x14 handling - real progress, IOP no longer panics during boot (Task #164)

Implemented direct interception (matching the established A0/B0/C0
HLE and BREAK-as-syscall-fallback precedent: return $v0=0, resume at
PC+4, no real exception raised) for IOP syscall numbers 0x10, 0x08,
and 0x14 (the last discovered mid-round when 0x10/0x08 alone let every
affected module advance to a second real syscall). Full 87-test
regression suite passes; clean Wii rebuild.

Real-BIOS result: modules_run_to_completion 15->19, trap_stubs_bypassed
13->0, and the IOP no longer halts/panics at all within a 30M-
instruction boot budget - it settles into a genuine polling loop
(beq $zero,$s1,-9words, calling two real subroutines each pass) rather
than any recognized panic pattern. This is a categorical improvement:
the IOP is doing real, ongoing kernel work instead of crashing.
HONEST CAVEAT: SIF_MSCOM/SIF_SMCOM/SIF_MSFLG/SIF_SMFLG are still
completely unchanged from every prior round - the polling loop's wait
condition has not yet been satisfied, so the actual SIF handshake goal
is not yet reached. Next: identify what $s1 and the two called
subroutines are waiting on. See docs/STATUS.md's 44th finding.

## Update (Round 29 continued, 45th finding): task #165 SOLVED - SIF IOP-mirror KSEG-alias masking bug fixed; polling loop unblocked, SIF_SMCOM/SIF_SMFLG change for the first time (Task #151/#165)

Corrected a mis-decode from the 44th finding: the polling loop
actually branches on `$s0` (`beq $s0,$zero,-9`), not `$s1` - caught by
sampling live registers at the branch site and finding `$s1`
consistently nonzero, which contradicted the earlier assumption.
Traced `$s0` back to a real SIF_MSFLG debounce-read (read address
twice, retry until stable) at KSEG1 address `0xBD000020`.

Root cause found: `sif_iop_mmio_read32()`/`write32()` in `source/hw/
sif.c` checked the raw incoming address against the
`0x1D000000-0x1D0000FF` mailbox window WITHOUT masking off KUSEG/
KSEG0/KSEG1 segment-select bits first (`iop_mem_ptr()` already does
this for plain RAM, but the SIF mirror never got the same treatment).
The real KSEG1 alias `0xBD000020` therefore missed the window check,
fell through to the RAM path, and silently returned 0 instead of the
real, already-correct SIF_MSFLG value - the loop was reading the wrong
location, not waiting on a genuinely unset flag.

Fix: mask `addr & 0x1FFFFFFFu` before the window check in both
functions (minimal, two-function change). Verified: 87/87 host-native
regression tests pass (0 real regressions - all initial "failures"
were the pre-existing, already-documented `-lm` link-order artifact in
the README's own commands); clean Wii/devkitPPC rebuild.

Real-BIOS result: the IOP no longer gets stuck in the polling loop -
it reaches a normal "module boot sequence complete" halt via the
existing LOADCORE panic-loop bypass. modules_run_to_completion
19->28/29. **SIF_SMCOM and SIF_SMFLG change for the first time this
entire session** (0->0x0011AFD0, 0->0x00010000) - every prior round
back to the 36th finding reported these frozen. SIF_MSFLG stays
0x00010000 (it was already correct - our own mirror just couldn't see
it through the KSEG1 alias). SIF_MSCOM stays 0 - not yet verified
whether that's expected steady-state. Task #151 narrows further but
stays open pending a trace of the EE side's reaction to the new SIF
register values. See docs/STATUS.md's 45th finding.

## Update (Round 29 continued, 46th/47th findings): EE kernel syscall table implemented for real - boot advances deep into real SIF command-protocol bring-up (Task #170/#172)

Tracing the EE side's reaction to the 45th finding's SIF fix (per the
user's request) uncovered that the EE kernel's own SYSCALL handling
had never been implemented at all (`ee_core.c` always just halted) -
and with the IOP-side fix in place, boot now reaches real EE syscalls
for the first time. Implemented real, cited handling (ps2sdk's public
syscallnr.h/sifdma.h) for every syscall observed: 100 (FlushCache), 60/
61 (SetupThread/SetupHeap) as genuine or precedented no-ops; 120
(sceSifSetDChain), 18 (AddDmacHandler), 22 (_EnableDmac) as honest
no-ops with a flagged caveat (real SIF0 DMA-interrupt plumbing this
project doesn't model, out of scope for graphics); and 121/122
(sceSifSetReg/sceSifGetReg) implemented FOR REAL against this
project's existing SIF register model rather than bypassed - this
caught a real bug when an initial flat bypass for 122 caused a NEW
infinite loop (real code polls sceSifGetReg(SIF_REG_SMFLAG) directly
waiting for SIF_STAT_CMDINIT).

Also added SIF_STAT_BOOTEND (0x40000, "Bootup completed") and
SIF_STAT_CMDINIT (0x20000, "SIFCMD initialized") signaling - both real,
documented sifdma.h bits - to the IOP module loader's existing "boot
sequence complete" halt sites, ORed onto SIF_SMFLG without clobbering
the already-correct SIF_STAT_SIFINIT bit from task #165.

Verified: 87/87 regression tests, clean Wii rebuild. Real-BIOS result:
boot now correctly runs the ENTIRE real sceSifInitCmd() sequence and
reaches syscall 119 (sceSifSetDma - a real DMA packet transfer, not yet
implemented) - the furthest point real-BIOS boot has ever reached in
this project. GS/display registers (PMODE/DISPFB/DISPLAY) are still
untouched - expected, since this is early kernel bring-up before any
drawing, not a sign of a remaining bug. See docs/STATUS.md's 46th/47th
findings.
