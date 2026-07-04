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
- [ ] LWL/LWR/SWL/SWR (unaligned word load/store)
- [ ] LQ/SQ (128-bit load/store - used constantly for VU/GS data)
- [ ] COP1 (FPU) - single-precision float ops, used by a lot of BIOS
      and game code (reference: `pcsx2/FPU.cpp`)
- [ ] COP2 (VU0 macro mode) - VU0 running as a COP2 coprocessor
      attached to the EE pipeline (reference: `pcsx2/VU0.cpp`, `COP2.cpp`)
- [ ] TLB / MMU (32-entry TLB, address translation)
- [ ] Exception handling (BEV, EPC, Cause, actual exception vectors -
      currently MFC0/MTC0 are read/write-only, no exceptions raised)
- [ ] Counters/Timers + INTC (interrupt controller) - needed for any
      timing-dependent BIOS code and for the IOP/EE to ever synchronize
- [ ] SIF0/SIF1/SIF2 (EE<->IOP sub-CPU interface) - the channel the
      BIOS uses to hand off to IOP modules; currently doesn't exist at
      all (reference: `pcsx2/Sif0.cpp`, `Sif1.cpp`, `Sif.cpp`)

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
      yet). Unit-tested in `tests/test_iop_core.c`. Standalone so far -
      not yet wired into main.c, no SIF, no IOP hardware registers.
- [ ] IOP hardware register stubs (interrupt controller, DMA, timers)
- [ ] Either: emulate the real IOP BIOS ROM, or (like PCSX2 optionally
      does) HLE the common IOP modules (SIO2MAN, MCMAN, PADMAN, etc.)
      well enough that SIF handshakes succeed

## 3. DMA controller

10 DMA channels move data between EE RAM, the IOP, VIF0/VIF1, GIF, and
the SPU2. Reference: `Dmac.cpp` (583) + `Dmac.h` (570).

- [ ] DMA register block (D_CTRL, per-channel CHCR/MADR/QWC/TADR)
- [ ] Channel state machine (normal/chain/interleave transfer modes)
- [ ] At minimum: the channels needed for BIOS boot to push data to
      GIF (graphics) and to talk to the IOP over SIF

## 4. GIF / VIF (packet interfaces)

- [ ] GIF (Graphics Interface) - packages EE/VU1 output into GS
      primitives (reference: `Gif.cpp` 799 lines, `Gif_Unit.cpp` 244)
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

- [ ] GS privileged register block (PMODE, DISPFB, DISPLAY, BGCOLOR,
      CSR, IMR, etc.) - currently these memory-mapped addresses are
      silent no-ops in `ee_core.c`
- [ ] GS local memory model (4MB eDRAM, PSMCT32/24/16/Z formats)
- [ ] Primitive rasterization (triangles/sprites/lines - at minimum
      whatever the BIOS splash actually draws with)
- [ ] A translation layer from GS's output format to something the
      Wii's GX (fixed-function, Flipper/Hollywood-derived) can
      display - this is its own subproject, not a thin adapter,
      because GS and GX have fairly different texture/blending models

## 7. Supporting pieces (lower priority for "just the splash screen")

- [ ] CDVD - disc/BIOS-boot-media emulation (BIOS checks for a disc
      even when booting to the OSD splash without one)
- [ ] SPU2 (audio) - not needed for a visual splash screen
- [ ] Pad/memory card - not needed to reach the splash screen, needed
      for anything past it

## Suggested near-term order

1. IOP CPU core skeleton (this is "just" another MIPS interpreter,
   well-scoped, and unblocks everything downstream of SIF)
2. Minimal SIF + DMA register stubs (enough for EE/IOP handshake, not
   full chain-mode DMA)
3. IOP HLE stubs for the specific modules the BIOS boot path calls
4. GIF/VIF passthrough (accept packets, don't yet rasterize)
5. GS register block + local memory (still no rasterization output)
6. Minimal rasterizer for whatever primitive types the splash actually
   uses, output to Wii GX framebuffer

Steps 1-4 are substantial but tractable in the way the EE interpreter
was. Step 6 is where this stops being "a lot of careful work" and
becomes genuinely research-scale for a solo project - see the GS line
count above.
