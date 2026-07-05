# Roadmap: everything between here and a BIOS splash screen

This is the structured task list for the project, broken down by
subsystem. Checked items are done; everything else is open. Rough
scale references (real PCSX2 upstream line counts) are included where
known, so "not implemented" items can be weighed honestly rather than
treated as equally-sized todos.

Order below is roughly priority order for reaching a visible BIOS
splash screen, not just difficulty.

## 1. EE (Emotion Engine) CPU core

- [x] Full MIPS III integer core (ALU imm+reg, shifts, MULT/DIV,
      HI/LO, branches, jumps, byte/half/word/double load+store)
- [x] Basic COP0 (MFC0/MTC0 - generic registers, Status, Config)
- [x] CACHE/SYNC/PREF as no-ops
- [x] ~35 of ~90 MMI (SIMD) opcodes (add/sub/logic/copy/extend/pack,
      MULT1/DIV1/MFHI1/MFLO1 pipe-1 variants)
- [ ] Remaining ~55 MMI opcodes (saturated arithmetic, PCGT*/PCEQ*/
      PMAX*/PMIN* compares, QFSRV, PMADDW/H family, PINTH/PINTEH,
      PROT3W, PEXEH/PEXEW/PEXCH/PEXCW, PMFHL/PMTHL clamping variants)
- [x] LWL/LWR/SWL/SWR (unaligned word load/store) - ported from
      R5900OpcodeImpl.cpp, unit tested in `tests/test_ee_unaligned.c`
      (also gave the IOP core this ability first, then brought it to
      the EE core for parity)
- [x] LQ/SQ (128-bit load/store) - ported from PCSX2's
      `R5900OpcodeImpl.cpp`: address masked to 16-byte alignment
      (real hardware ignores the low 4 bits rather than faulting);
      LQ skips the read entirely when rt==$0 (matches PCSX2's own
      interpreter, unlike other loads in this file which still
      perform a discarded read for its memory side effects). Unit
      tested in `tests/test_ee_lqsq.c` (8/8 checks). Added after
      real-BIOS testing showed the EE halting on exactly this opcode
      - but re-testing after the fix revealed the halt point didn't
      move at all, which led to a bigger, more important finding (see
      docs/STATUS.md's "LQ/SQ implemented" section): the EE wasn't
      actually doing 53M instructions of real boot work at all - see
      "Suggested near-term order" below for the corrected picture.
- [x] COP1 (FPU) - core single-precision ops: MFC1/CFC1/MTC1/CTC1,
      ADD.S/SUB.S/MUL.S/DIV.S/ABS.S/MOV.S/NEG.S, SQRT.S/RSQRT.S,
      MAX.S/MIN.S, CVT.W.S/CVT.S.W, C.EQ.S/C.LT.S/C.LE.S, and
      BC1F/BC1T (branch on FP condition flag). Ported from
      `pcsx2/FPU.cpp` including the PS2 FPU's non-IEEE quirks
      (denormal inputs/outputs flushed to signed zero, infinities
      clamped to +/-Fmax), SQRT.S/RSQRT.S's special cases (negative
      input takes sqrt(fabs()) instead of NaN; RSQRT.S with a
      zero/denormal divisor returns +Fmax instead of infinity - and
      the real-hardware quirk that SQRT.S's source operand is Ft, not
      Fs), and MAX.S/MIN.S's bit-level signed-int comparison trick
      (`fp_max`/`fp_min` - only differs from a naive float compare
      when both operands are negative). Unit tested in
      `tests/test_ee_fpu.c` and `tests/test_ee_fpu2.c` (13/13 checks,
      including both branch directions for BC1F/BC1T to prove they're
      not accidentally unconditional). Still missing: the
      MADD/MSUB/MADDA/MSUBA accumulator family, BC1FL/BC1TL ("likely"
      branches - this project has no likely-branch infrastructure yet
      for ANY branch, integer or FP), and the FPU exception-cause
      control-register flags (only the condition flag needed for BC1
      is modeled).
- [ ] COP2 (VU0 macro mode) - VU0 running as a COP2 coprocessor
      attached to the EE pipeline (reference: `pcsx2/VU0.cpp`, `COP2.cpp`)
- [x] COP0 "CO"-format instructions: RFE, ERET, EI, DI - dispatched
      via a 6-bit `funct` field (not the `rs` field used by MFC0/MTC0)
      once `rs`'s top bit is set (`rs & 0x10`), matching PCSX2's
      `tbl_COP0_C0[64]` table. Added directly in response to real
      BIOS testing (SCPH-10000 - see docs/STATUS.md): the EE
      interpreter used to halt almost immediately on these. RFE
      shifts Status's KU/IE bit-stack right by 2 (real note: RFE
      isn't actually implemented on real EE hardware per PCSX2's own
      table, but the SCPH-10000 BIOS's PS1-compat boot path executes
      it anyway, so it's modeled to let that path progress). ERET
      branches to ErrorEPC or EPC depending on Status.ERL and has NO
      branch delay slot (unlike ordinary branches). EI/DI set/clear
      Status.EIE, gated by `_EDI || EXL || ERL || KSU==0`. Semantics
      ported from PCSX2's `COP0.cpp`. Unit tested in
      `tests/test_ee_cop0_special.c` (9/9 checks). After this fix,
      the real-BIOS diagnostic went from halting at ~99K instructions
      to running past 5 million without hitting another unimplemented
      opcode.
- [ ] TLB / MMU (32-entry TLB, address translation)
- [ ] Exception handling (BEV, EPC, Cause, actual exception vectors -
      currently MFC0/MTC0 are read/write-only, and RFE/ERET/EI/DI
      above manipulate Status/EPC directly without a real exception
      ever being raised on the EE side - the IOP now raises a real
      SYSCALL exception, see section 2, but the EE side of this is
      still open)
- [ ] Counters/Timers + INTC (interrupt controller) - needed for any
      timing-dependent BIOS code and for the IOP/EE to ever synchronize
- [x] SIF mailbox/flag registers (MSCOM/SMCOM/MSFLAG/SMFLAG/CTRL,
      0x1000F200-0x1000F260), EE side only - `source/hw/sif.c`, wired
      into `ee_core.c`'s 32-bit MMIO dispatch. Register-level
      semantics ported directly from PCSX2's `HwWrite.cpp`/
      `HwRead.cpp`/`Hw.cpp` (MSFLAG ORs on write, SMFLAG ANDs-off on
      write, CTRL's fixed 0xF0000102 read-side OR mask and bit-0x100
      lock flag). Unit tested in `tests/test_sif.c` (13/13 checks,
      including a real subtlety this test caught: CTRL's read mask
      always shows bit 0x100 set, so the lock flag's clear behavior
      has to be checked against internal state, not through the read
      path - matches real hardware). NOT modeled: the actual IOP-side
      mirror registers, the IRQ-raise/IOP-reset side effects of CTRL
      bits 18/19 (no-ops - no cross-CPU wiring to iop_core.c exists
      yet), and SIF0/SIF1/SIF2 themselves are still just DMA channel
      register slots in dma.c with no IOP on the other end consuming
      them.
- [x] IOP-side SIF mirror (0x1D000000-0x1D0000FF) - `sif_iop_mmio_read32/
      write32` in `source/hw/sif.c`, wired into `iop_core.c`'s 32-bit
      MMIO dispatch. Modeled as a flat, plain read/write window (no
      OR/AND special casing), matching PCSX2's OWN IOP-side treatment
      (`MemoryTypes.h`'s `u8 Sif[0x100]` flat array + `IopMem.h`'s
      `psxSu32` macro - PCSX2 itself doesn't special-case this side
      either, per its own "likely not needed" comment). The IOP-side
      window's low byte lines up with the EE-side register's low byte
      (0x1D0000XX <-> 0x1000F2XX) so both sides read/write the same
      underlying state.
- [x] **A real, working EE<->IOP handshake** - `source/core/system.c`
      (`system_init`/`system_run_interleaved`) steps both cores
      alternately (one instruction each per slice) instead of running
      either to completion in isolation, which is what actually makes
      a mailbox-register handshake possible for the first time in
      this project. Proven end-to-end in
      `tests/test_system_handshake.c`: a hand-encoded EE program
      writes MSCOM and sets an MSFLAG bit, a hand-encoded IOP program
      polls MSFLAG, reads MSCOM, echoes the value into SMCOM and sets
      an SMFLAG bit, and the EE polls SMFLAG and reads back the exact
      value the IOP echoed - a genuine round trip through shared
      hardware state between two independently-interpreted CPU cores.
      All 9 checks passed on the first run. `main.c` now calls
      `system_init`/`system_run_interleaved` instead of driving the EE
      core alone - the IOP actually runs at boot for the first time.
      Known simplification: 1:1 instruction-count stepping, not
      clock-rate-accurate (real EE:IOP is roughly 8:1) - see
      `system.h`'s header comment. This is the mailbox layer only - a
      REAL BIOS boot still needs IOP HLE/BIOS module emulation on top
      of this (see section 2) before any of this produces meaningful
      behavior beyond the test's toy protocol (reference:
      `pcsx2/Sif0.cpp`, `Sif1.cpp`, `Sif.cpp` for the full DMA-backed
      protocol this only partially models at the register level).

## 2. IOP (I/O Processor) - separate MIPS core

The PS2 has a second, independent CPU (R3000A, MIPS I, no MMI/VU/GS -
much simpler ISA than the EE) that runs its own copy of low-level BIOS
code and handles controllers, memory cards, and the CD/DVD drive. The
EE BIOS boot sequence depends on IOP modules loading successfully over
SIF; without this, BIOS boot stalls waiting for IOP responses that
never come.

Reference sizes: `R3000A.cpp` (304 lines) + `R3000AInterpreter.cpp`
(324) + `R3000AOpcodeTables.cpp` (382) for the CPU core itself;
`IopBios.cpp` (1502 lines) for the HLE BIOS module layer PCSX2 uses
instead of fully emulating the real IOP BIOS ROM.

- [x] R3000A CPU state + memory model (2MB IOP RAM)
- [x] R3000A interpreter (simpler than EE - no MMI/128-bit registers,
      but same MIPS I branch-delay-slot structure; includes LWL/LWR/
      SWL/SWR unaligned load/store, which the EE core doesn't have
      yet). Unit-tested in `tests/test_iop_core.c`.
- [x] Wired into `main.c` and running interleaved with the EE core via
      `source/core/system.c` (see section 1's SIF handshake entry for
      details) - no longer standalone. Has SIF mailbox register
      access (section 1). Still has NO other IOP hardware registers
      (interrupt controller, DMA, timers) - see next bullet.
- [x] IOP interrupt controller (I_STAT/I_MASK/I_CTRL,
      0x1F801070-0x1F801078) - `source/hw/iop_intc.c`, wired into
      `iop_core.c`'s 32-bit MMIO dispatch. Semantics ported from
      PCSX2's `ps2/Iop/IopHwWrite.cpp`/`IopHwRead.cpp`: I_STAT write
      ANDs the value in (write-0-to-clear - opposite polarity from
      GS_CSR/SIF SMFLAG's write-1-to-clear elsewhere in this
      project), I_MASK is plain read/write, I_CTRL clears itself to 0
      as a side effect of being READ (a one-shot latch - the first
      register in this project where the interesting behavior is on
      the read path). `iop_intc_raise(irq)` lets future peripheral
      models set a pending bit. Unit tested in `tests/test_iop_intc.c`
      (11/11 checks). NOT modeled: actually raising a CPU interrupt/
      exception in iop_core.c when I_STAT & I_MASK becomes nonzero
      (no interrupt/exception handling exists in the IOP interpreter
      at all yet), and the ~20 individual real IRQ source assignments
      (VBLANK, DMA completion, etc).
- [x] IOP DMA controller register stubs - `source/hw/iop_dma.c`,
      wired into `iop_core.c`'s 32-bit MMIO dispatch. Separate from
      the EE's DMA model in `dma.c` - the IOP has its own controller
      with its own 13-channel layout (MDEC in/out, GPU, CDROM, SPU,
      OTC, SPU2, DEV9, SIF0, SIF1, SIO2 in/out - channel 5 doesn't
      exist on real hardware, modeled as a genuine gap). Per-channel
      MADR/BCR/CHCR/TADR are plain latched storage (writing CHCR does
      NOT trigger a transfer - real PCSX2 calls a per-channel,
      device-specific transfer function here that isn't modeled).
      DMA_ICR/DMA_ICR2 have real, non-trivial write semantics ported
      from `ps2/Iop/IopHwWrite.cpp`: bits 0-23 (force-IRQ, per-channel
      enable, master-enable) are plainly overwritten, bits 24-30
      (per-channel pending flags) are write-1-to-clear and can only
      ever be cleared by software (never set - only a real DMA
      completion event sets them, not modeled), and bit 31 (master
      IRQ flag) is recomputed on every write. Unit tested in
      `tests/test_iop_dma.c` (20/20 checks; caught a real test-design
      subtlety, not an implementation bug, while being written - see
      tests/README.md). NOT modeled: any actual transfer execution
      for any channel, or the interrupt/exception side effects a real
      ICR write would trigger (no such wiring exists in iop_core.c).
- [x] IOP timer register stub (T0-T5: COUNT/MODE/TARGET across the
      two real hardware address windows 0x1F801100-0x1F80112B and
      0x1F801480-0x1F8014AB) - `source/hw/iop_timers.c`, wired into
      `iop_core.c`'s 32-bit MMIO dispatch. Deliberately a PLAIN
      register stub, more limited than the DMA/INTC/SIF models above:
      COUNT/MODE/TARGET are just latched storage with no special-case
      write behavior. Real hardware/PCSX2 (`IopCounters.cpp`,
      `psxRcntWmode16/32`) preserves certain status flag bits across
      a MODE write, recomputes IRQ-mode state, and drives a live,
      ticking counter service that advances COUNT based on elapsed
      CPU cycles/gate mode/clock source and fires an interrupt at
      TARGET/overflow - NONE of that is modeled here; these registers
      do not advance on their own. Unit tested in
      `tests/test_iop_timers.c` (10/10 checks - address decoding
      across both hardware windows, cross-counter isolation, and
      confirming addresses inside a counter's window that aren't one
      of the 3 known register offsets are correctly unclaimed).
      Modeling real counting/gating/target-IRQ behavior remains
      future work and would be a meaningfully bigger effort than this
      stub (needs to tick forward in sync with instruction execution,
      not just latch writes) - not attempted here.
- [x] IOP BIOS syscall trap (the "HLE BIOS replacement" mechanism) -
      `source/hw/iop_hle_bios.c`, wired into `iop_core.c`'s
      `iop_step()`. Implements the classic, well-established PS1/PS2
      A0/B0/C0 BIOS call convention (function number in $t1, `JAL`
      to one of the three fixed trap addresses, real BIOS ROM code
      there dispatches and returns via `JR $ra`) - intercepting PC
      reaching 0xA0/0xB0/0xC0 and handling the call natively instead
      of decoding whatever bytes are actually loaded there. IMPORTANT:
      this is explicitly NOT a port of PCSX2's actual IOP HLE
      (`IopBios.cpp`, ~1500 lines), which takes a fundamentally
      different and far more involved approach - it lets a REAL,
      working IOP BIOS ROM boot far enough to build its own internal
      data structures (loadcore's module list, thread manager state)
      and only intercepts specific IRX library import calls once
      those exist, which requires a real BIOS ROM this project
      doesn't ship and can't verify version-specific internal
      structure layouts for. What's implemented instead is honest and
      bounded: every trapped call is logged (table + function number)
      and given a generic default return value (0) - NO specific
      function-number-to-behavior mapping is implemented or guessed
      at, since this project doesn't have a verified, citable
      reference for real PS1/PS2 BIOS syscall semantics. Still a real
      improvement over the prior behavior (an incomplete/fake BIOS
      image reading as zero bytes at these addresses would decode as
      valid NOP instructions and loop forever, going nowhere - the
      trap now returns control to the caller immediately, letting
      whatever boot code issued the call keep making progress).
      Unit tested in `tests/test_iop_hle_bios.c` (9/9 checks; caught a
      real MIPS J-type addressing subtlety in the test's own setup -
      see tests/README.md).
- [x] IOP module registry (a "module loading logic" scaffold) -
      `source/hw/iop_hle_modules.c`. Records module name/version pairs
      via a project-owned API (`iop_hle_module_register`/
      `iop_hle_module_is_registered`) - explicitly NOT a port of real
      IRX module parsing or PCSX2's `sceSifLoadModule`-style SIF RPC
      protocol (Sifcmd.h), both of which are substantially bigger
      undertakings requiring real module binary formats and BIOS
      structures this project doesn't have verified references for.
      Standalone so far: NOT yet wired to the BIOS syscall trap above
      with a specific "this function number means load-module" rule,
      because this project doesn't have a verified real function
      number for that and didn't want to fabricate one that could
      later be mistaken for real hardware behavior - wiring a real
      trigger is future work once/if a verified reference is found.
      Unit tested in `tests/test_iop_hle_modules.c`.
- [x] SYSCALL exception handling (MIPS I, SPECIAL funct 0x0C) - added
      directly in response to real BIOS testing (SCPH-10000 - see
      docs/STATUS.md): the IOP interpreter used to halt
      unconditionally on the first real SYSCALL instruction it hit.
      Now sets Cause.ExcCode (pre-shifted value 0x20 = ExcCode 8/
      "Syscall"), sets EPC to the SYSCALL instruction's own address
      (branch-delay-slot BD-bit handling is explicitly NOT modeled -
      documented simplification), vectors PC to the bootstrap handler
      (0xBFC00180) if Status.BEV is set, or the normal handler
      (0x80000080) otherwise, and shifts Status's KU/IE stack LEFT by
      2 (opposite direction from the EE's RFE, section 1). Ported
      from PCSX2's `psxException()` in `R3000A.cpp`. Also fixed
      `iop_core_init()`, which incorrectly left Status.BEV at 0 on
      reset via a plain `memset` - real hardware/PCSX2's `psxReset()`
      sets it to 1, which matters directly here since it's what
      selects which vector SYSCALL jumps to. Unit tested in
      `tests/test_iop_syscall.c` (5/5 checks). After this fix, the
      real-BIOS diagnostic progressed from halting at 3,054,721
      instructions (raw SYSCALL) to 3,054,763 (42 more real
      instructions executed inside the exception handler) before
      hitting a NEW halt: an unimplemented SPECIAL `funct 0x3F` at
      `pc=0x00000068` - a low RAM address. Investigated (see
      docs/STATUS.md) and traced to a real, now-fixed gap: see the
      InstallExceptionHandlers bullet below.
- [x] InstallExceptionHandlers (`C0h` function `0x07`) - real behavior
      instead of the generic HLE default, added directly in response
      to the SYSCALL halt above. Using the public psx-spx community
      reference (https://psx-spx.consoledev.net/kernelbios/) as a
      citable source: this real BIOS function's job is to write a
      well-known 16-byte trampoline (`LUI $k0,0` / `ADDIU $k0,$k0,
      <addr>` / `JR $k0` / `NOP`) into RAM at the hardware exception
      vector (address 0x80, mirrored to address 0) - which this
      project's generic "every call returns 0" HLE stub was silently
      skipping, leaving the exception vector empty and causing
      execution to eventually wander into unrelated reserved memory
      after the SYSCALL fix above started actually using it.
      `source/hw/iop_hle_bios.c` now locates the real 16-byte template
      inside the actual loaded BIOS ROM itself (a distinctive
      3-of-4-words byte signature; only the jump-target immediate
      varies by BIOS revision) rather than hardcoding an assumed
      immediate from the documentation's example values, and installs
      those exact, version-correct bytes - confirmed to appear exactly
      once in the SCPH-10000 dump used for testing. Every other A0/B0/
      C0 function number is completely unaffected. Unit tested in
      `tests/test_iop_hle_exception_install.c` (9/9 checks). Result:
      the IOP no longer halts at all - re-tested out to 100,000,000
      instructions, still running, making the same 27 real HLE calls
      as before. The EE, run against that larger cap, now reaches
      53,592,141 instructions before hitting an expected, already-
      documented gap (COP2/LQ-SQ, section 1) - see docs/STATUS.md's
      "Resolved: InstallExceptionHandlers" section for full detail.

## 3. DMA controller

10 DMA channels move data between EE RAM, the IOP, VIF0/VIF1, GIF, and
the SPU2. Reference: `Dmac.cpp` (583) + `Dmac.h` (570).

- [x] DMA register block (D_CTRL/D_STAT/D_PCR/etc, per-channel
      CHCR/MADR/QWC/TADR/ASR0/ASR1/SADR) - `source/hw/dma.c`, unit
      tested in `tests/test_dma_core.c`. Registers only: reads/writes
      are latched faithfully, addresses are decoded against the real
      PS2 memory map, but no channel is wired into `ee_core.c`'s
      memory bus yet and no transfer actually executes.
- [x] Wire dma_mmio_read32/write32 into ee_core.c's memory access
      path (32-bit load/store only so far - matches how real code
      actually accesses these registers). Verified end-to-end in
      `tests/test_ee_dma_bus.c`: SW to a DMA register address now
      correctly reaches `dma.c` instead of vanishing into RAM, and LW
      reads it back with correct sign-extension.
- [x] Channel state machine - NORMAL mode (one-shot MADR+QWC
      transfer) and CHAIN mode walking DMA_TAG_REFE/CNT/NEXT/END tags
      (the four most common), with correct inline-vs-out-of-line data
      addressing. Writing CHCR with the STR bit set now actually
      triggers a transfer (`dma_channel_kick`), which delivers data to
      a per-channel sink callback (`dma_set_sink`) - the hook point
      for a future GIF packet parser - and correctly auto-clears STR
      on completion. Unsupported tag IDs (REF/REFS/CALL/RET - address
      indirection and the ASR call/return stack) set an error flag
      and stop cleanly rather than misbehaving. INTERLEAVE mode (SPR
      only) is not implemented. Unit tested end-to-end in
      `tests/test_dma_chain.c`.
- [ ] At minimum: the channels needed for BIOS boot to push data to
      GIF (graphics) and to talk to the IOP over SIF

## 4. GIF / VIF (packet interfaces)

- [x] GIF (Graphics Interface), PACKED mode only - `source/hw/gif.c`,
      registered as the sink for `DMA_CHANNEL_GIF` via
      `dma_set_sink()` inside `ee_core_init()`, so it now receives
      real quadwords whenever the DMA chain engine kicks the GIF
      channel. Parses GIFtag (NLOOP/EOP/PRE/PRIM/FLG/NREG) and, for
      PACKED-mode data, the four register formats currently handled:
      PRIM, RGBAQ, XYZ2, and A+D (address+data - used to write PRIM/
      RGBAQ/XYZ2/FRAME_1/XYOFFSET_1 by address rather than by fixed
      per-loop register list, which is how real BIOS/game code
      typically drives the GIF). XYZ2 does real 12.4 fixed-point ->
      pixel conversion against XYOFFSET_1, and on the second vertex
      of a SPRITE primitive (PRIM type 6) it rasterizes a filled
      axis-aligned rectangle directly into GS memory via
      `gs_mem_write_psmct32`. This is the first genuine "GIF packet
      in -> pixels in GS memory" path in the project, exercised
      end-to-end in `tests/test_gif.c` (13/13 checks, including
      pixel-level bounds checks that the rectangle is exactly the
      right size and position and touches nothing outside it).
      Reference: `Gif.cpp` 799 lines, `Gif_Unit.cpp` 244 - only a
      sliver of that is covered. NOT yet covered: REGLIST/IMAGE
      transfer modes, any primitive besides SPRITE (no lines,
      triangles, or triangle strips/fans/textures), GS context 2
      (FRAME_2/XYOFFSET_2), and VU1-sourced GIF traffic (path 1) -
      only the direct EE->GIF path (path 3) is modeled.
- [ ] VIF0/VIF1 (Vector Interface) - feeds VU0/VU1 with microcode data
      and unpacks data formats (reference: `Vif.cpp` 418, plus
      `Vif_Unpack.cpp`, `Vif_Codes.cpp`, `Vif1_Dma.cpp`, `Vif0_Dma.cpp`)

## 5. VU0 / VU1 (Vector Units)

Two dedicated 128-bit SIMD coprocessors with their own micro-code
instruction set (VU microcode, not MIPS). VU0 is also reachable as EE
COP2 ("macro mode"). Real PCSX2 has both an interpreter
(`VU0microInterp.cpp`, `VU1microInterp.cpp`) and x86 recompilers for
these - only the interpreter side is even theoretically portable.

- [ ] VU0/VU1 register file + micro-instruction memory
- [ ] VU microcode interpreter (separate ISA from MIPS - not a small
      addition)
- [ ] VIF-side data unpacking into VU memory

## 6. GS (Graphics Synthesizer) + Wii output

This is the largest remaining piece by a wide margin. Real PCSX2's
`GS/` directory alone is **~114,500 lines** across local memory
management, rasterization, texture/CLUT handling, and per-backend
renderers (it ships DX11/DX12/Vulkan/Metal/OpenGL/software renderers -
none of which are relevant to Wii, which has neither a modern
programmable GPU nor any of those APIs).

- [x] GS privileged register block (PMODE, SMODE1/2, SRFSH, SYNCH1/2,
      SYNCV, DISPFB1/2, DISPLAY1/2, EXTBUF/DATA/WRITE, BGCOLOR, CSR
      w/ write-1-to-clear, IMR, BUSDIR, SIGLBLID) - `source/hw/gs.c`,
      wired into `ee_core.c`'s 64-bit load/store path (LD/SD - these
      are genuinely 64-bit registers on real hardware), unit tested
      in `tests/test_gs_registers.c`. Registers only: nothing is
      rasterized, GS local memory (the 4MB eDRAM) doesn't exist yet.
- [x] GS local memory model - SIMPLIFIED: linear addressing instead
      of real hardware's block-swizzled layout (see code comments in
      `include/core/hw/gs_mem.h` for why), PSMCT32 only (no 24/16/Z/
      paletted formats). `source/hw/gs_mem.c`, unit tested in
      `tests/test_gs_mem.c`. Real swizzle addressing is still open -
      needed before texture sampling or certain blit tricks would
      work correctly.
- [x] Primitive rasterization - SPRITE (filled axis-aligned
      rectangles) only, via the GIF parser above (`source/hw/gif.c`
      calling `gs_mem_write_psmct32`). Triangles/lines/strips/fans
      and textured primitives are still open - a real BIOS splash
      likely needs at least triangles and textures too.
- [x] A first, minimal translation layer from GS memory to the Wii's
      display: `source/hw/gs_wii_output.c` converts a rectangular
      PSMCT32 region to the Wii's packed Y1CbY2Cr XFB format (RGB->YUV
      conversion ported from libogc's console.c) and blits it
      directly into the framebuffer - no GX 3D pipeline involved,
      just a direct pixel blit. Unit tested in
      `tests/test_gs_output.c` against hand-verified black/white
      YCbCr anchor points, AND wired into `source/main.c` so a real
      Wii/Dolphin boot now visibly draws a 4-color test pattern -
      the first actual pixels-on-a-real-screen milestone in the
      project. As of the GIF parser above, GS memory CAN now be
      populated by a real DMA-chain-delivered GIF packet rather than
      only by `main.c`'s hardcoded demo pattern - but `main.c` itself
      still only demonstrates the fixed 4-color test pattern; it has
      not yet been updated to drive a GIF packet through
      `dma_channel_kick` at boot as a live end-to-end demo. Still not
      driven by anything the actual BIOS does - no EE code is
      executing real GS-driving instructions yet. A real splash
      screen still needs triangle rasterization and textures, plus
      eventually a proper GX-based renderer once primitives beyond
      flat rectangles are needed.

## 7. Supporting pieces (lower priority for "just the splash screen")

- [ ] CDVD - disc/BIOS-boot-media emulation (BIOS checks for a disc
      even when booting to the OSD splash without one)
- [ ] SPU2 (audio) - not needed for a visual splash screen
- [ ] Pad/memory card - not needed to reach the splash screen, needed
      for anything past it

## Suggested near-term order

**Update: a real BIOS dump (SCPH-10000, legally owned by this
project's user) was used for local testing** (never committed - see
docs/STATUS.md's "First real BIOS boot attempt" section for full
detail, and data/pcsx2/bios/README.txt). This surfaced a real
ROMDIR-parsing bug (fixed) and gave two concrete, non-speculative next
targets by actually running the real BIOS against this project's
interpreters. **Both of those original targets are now fixed** (EE
COP0 CO-format instructions RFE/ERET/EI/DI, section 1; IOP SYSCALL
exception handling + Status.BEV reset fix, section 2) - re-running the
same diagnostic afterward: the EE now runs the full 5,000,000-
instruction test slice without halting at all (up from 99,158), and
the IOP progresses to 3,054,763 instructions (up from 3,054,721)
before hitting a NEW halt point: an unimplemented SPECIAL `funct 0x3F`
at `pc=0x00000068`.

**That new IOP halt has now been investigated AND fixed** (see
docs/STATUS.md's "Investigating the new IOP halt" and "Resolved:
InstallExceptionHandlers" sections for the full trail). It was never
a missing-opcode gap (`funct 0x3F` isn't a real R3000A instruction).
The real cause: `C(07h)` (`InstallExceptionHandlers`, per the public
psx-spx community reference, https://psx-spx.consoledev.net/kernelbios/
- now used as a citable source, distinct from disassembling the
copyrighted BIOS binary) is a real BIOS function whose job is to write
a well-known 16-byte trampoline into RAM at the hardware exception
vector (address 0x80) - this project's generic HLE stub was silently
no-op'ing that call instead. Fixed in `source/hw/iop_hle_bios.c`: this
one specific function number now locates the real 16-byte template
inside the actual loaded BIOS ROM (a distinctive byte signature - only
the jump-target immediate varies by revision, found via scan rather
than hardcoded) and installs it for real; every other function number
is unaffected. Unit tested (`tests/test_iop_hle_exception_install.c`,
9/9 checks).

**Result**: dramatic further progress on the IOP - it no longer halts
at all, re-tested out to 100,000,000 instructions, still running, same
27 real HLE calls as before. The EE's picture needed a correction,
though: it was initially reported as reaching 53,592,141 instructions
before an expected COP2/LQ-SQ-shaped halt - but implementing LQ/SQ
(now done, see section 1) didn't move the halt point AT ALL, which
led to properly investigating it (docs/STATUS.md's "LQ/SQ implemented"
section has the full trace). The real finding: the EE only executes
about 99,262 REAL instructions before a `JALR` sends it to an address
beyond the emulated 32MB EE RAM; because unmapped memory safely reads
as zero (a real NOP), the CPU doesn't crash or halt - it marches
forward in a straight line for 53+ million steps until it happens to
reach the hardware register window and finally decodes a live register
value as an invalid opcode. So "53 million instructions" was never
real boot progress; the actual, honest number is closer to 99,262.
This is now the single most important EE investigation target - NOT
yet solved, unlike the two IOP fixes above. The pointer chain leading
to the bad JALR target was traced back to a dereference of EE address
0x100, initially suspected (by analogy with the IOP's InstallExceptionHandlers
fix) to be an unpopulated kernel table - but checked against ps2tek
(https://psi-rockin.github.io/ps2tek/, a citable public PS2 hardware
reference), address 0x100 is actually the CPU's own Debug exception
vector, which real hardware never installs a handler for either - so
the zero value there may be entirely correct, and that analogy doesn't
hold. See docs/STATUS.md for the full, honest writeup: this remains
unsolved, and needs either a citable EE kernel-boot-internals reference
(ps2tek doesn't cover this) or more careful tracing before attempting a
fix. The IOP has no known
halt point left to chase at all right now; COP2/VU0 remains unstarted
and unproven against real BIOS code, since the EE hasn't legitimately
run far enough to demonstrate needing it yet.

1. IOP CPU core skeleton (this is "just" another MIPS interpreter,
   well-scoped, and unblocks everything downstream of SIF) - DONE
2. Minimal SIF + DMA register stubs (enough for EE/IOP handshake, not
   full chain-mode DMA) - DONE: both the EE-side special-cased
   registers and the IOP-side flat mirror exist and are wired into
   their respective cores.
3. Interleaved EE/IOP execution + a real, working mailbox handshake -
   DONE (`source/core/system.c`, `tests/test_system_handshake.c`) -
   this was the actual missing piece that made SIF register stubs
   meaningful; see section 1 for full detail. NOT yet a real protocol
   - it's proven with a hand-written toy handshake, not the actual
   BIOS/PCSX2 SIF DMA protocol (`Sif0.cpp`/`Sif1.cpp`).
4. IOP HLE stubs for the specific modules the BIOS boot path calls -
   PARTIAL, and meaningfully further along now: the IOP has register
   stubs for its interrupt controller, DMA controller, and timers
   (section 2), a BIOS syscall trap for the classic A0/B0/C0 call
   convention (`source/hw/iop_hle_bios.c`), and a module registry
   scaffold (`source/hw/iop_hle_modules.c`) - see section 2 for exact
   scope and the important caveat that neither of the last two is a
   port of PCSX2's actual, much more involved `IopBios.cpp` (which
   depends on a real, working BIOS ROM this project doesn't have and
   can't fake convincingly). What real BIOS/game code would still need
   before any of this does something meaningful: a verified reference
   for actual PS1/PS2 BIOS syscall function numbers (this project
   doesn't have one and deliberately hasn't guessed), and/or a decision
   to go the PCSX2 route (parse real BIOS structures - needs a real
   BIOS ROM) vs. inventing this project's own from-scratch IOP "OS"
   that real game IRX modules could never actually run against anyway
   (game code is compiled against the real IOP kernel's ABI). This is
   the crux of why "just get the BIOS splash to render" remains hard
   even with all the hardware register plumbing in place.
5. GIF/VIF passthrough - DONE for PACKED-mode GIF + SPRITE
   rasterization (see section 4 above); VIF0/VIF1 itself is still
   open
6. GS register block + local memory - DONE (section 6)
7. Minimal rasterizer for whatever primitive types the splash actually
   uses, output to Wii GX framebuffer - PARTIAL: SPRITE works via a
   direct-to-XFB pixel blit (not real GX), triangles/textures still
   open, and this whole path is still not driven by real BIOS/EE
   code since IOP HLE isn't wired up yet

Remaining near-term candidates, roughly in order of how directly they
unblock "the BIOS actually draws something": IOP hardware register
stubs (INTC/DMA/timers) and/or IOP HLE stubs for the specific
BIOS-boot-path modules (both are needed before real BIOS code - as
opposed to hand-written test programs - can get through a real SIF
handshake), a clock-rate-aware EE:IOP scheduler (currently 1:1
instruction stepping, real hardware is roughly 8:1), triangle
rasterization in the GIF parser, VIF0/VIF1 passthrough, and wiring a
real GIF packet through `dma_channel_kick` at boot in `main.c` as a
live demo (currently only the hardcoded 4-color pattern runs there).

Step 7 (real GX-based rendering with textures) is where this stops
being "a lot of careful work" and becomes genuinely research-scale for
a solo project - see the GS line count above.
