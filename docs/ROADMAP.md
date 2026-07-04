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
- [ ] LQ/SQ (128-bit load/store - used constantly for VU/GS data)
- [x] COP1 (FPU) - core single-precision ops: MFC1/CFC1/MTC1/CTC1,
      ADD.S/SUB.S/MUL.S/DIV.S/ABS.S/MOV.S/NEG.S, CVT.W.S/CVT.S.W,
      C.EQ.S/C.LT.S/C.LE.S. Ported from `pcsx2/FPU.cpp` including the
      PS2 FPU's non-IEEE quirks (denormal inputs/outputs flushed to
      signed zero, infinities clamped to +/-Fmax). Unit tested in
      `tests/test_ee_fpu.c`. Still missing: SQRT.S/RSQRT.S, the
      MADD/MSUB/MADDA/MSUBA family, MAX.S/MIN.S, and BC1 branches.
- [ ] COP2 (VU0 macro mode) - VU0 running as a COP2 coprocessor
      attached to the EE pipeline (reference: `pcsx2/VU0.cpp`, `COP2.cpp`)
- [ ] TLB / MMU (32-entry TLB, address translation)
- [ ] Exception handling (BEV, EPC, Cause, actual exception vectors -
      currently MFC0/MTC0 are read/write-only, no exceptions raised)
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
- [ ] IOP timers (T0-T5: COUNT/MODE/TARGET at 0x1F801100-0x1F8014A8) -
      still open. Note when this is picked up: real timer behavior
      (actual cycle counting, gate modes, clock source selection,
      target-reached IRQs - PCSX2's `Counters.cpp`) is substantially
      more involved than the register-stub pattern used for DMA/INTC
      above, since it requires ticking state forward in sync with
      instruction execution rather than just latching register
      writes - budget for that as a bigger increment than the ones
      before it.
- [ ] Either: emulate the real IOP BIOS ROM, or (like PCSX2 optionally
      does) HLE the common IOP modules (SIO2MAN, MCMAN, PADMAN, etc.)
      well enough that SIF handshakes succeed

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
   PARTIAL: the IOP interrupt controller (I_STAT/I_MASK/I_CTRL) now
   exists (section 2), but the IOP still has no DMA controller of its
   own, no timers, and - the bigger gap - no actual module-loading
   logic or HLE BIOS replacement (like PCSX2's `IopBios.cpp`). The
   IOP core still just executes whatever raw BIOS ROM bytes are at
   its reset vector with nothing resembling real boot behavior.
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
