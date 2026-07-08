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
- [x] ~48 of ~90 MMI (SIMD) opcodes (add/sub/logic/copy/extend/pack,
      MULT1/DIV1/MFHI1/MFLO1 pipe-1 variants) plus the compare/max/
      min/abs family: PCGTW/PCGTH/PCGTB and PMAXW/PMAXH (MMI0),
      PABSW/PCEQW/PMINW/PADSBH/PABSH/PCEQH/PMINH/PCEQB (MMI1). All
      ported from PCSX2's `MMI.cpp`. Compares produce an all-1s/
      all-0s mask result (not a boolean 0/1), matching real hardware's
      SIMD-compare convention. PABSW/PABSH preserve a real quirk:
      INT32_MIN/INT16_MIN have no positive representation at their
      width, so they clamp to INT32_MAX/INT16_MAX instead of
      overflowing. PADSBH is deliberately asymmetric - its low 4
      halfword lanes compute PSUBH (rs-rt) while its high 4 lanes
      compute PADDH (rs+rt), not a uniform 8-lane op. Unit tested in
      `tests/test_ee_mmi_compare.c` (32/32 checks).
- [x] MMI0's remaining saturated add/sub family - PADDSW/PSUBSW
      (32-bit), PADDSH/PSUBSH (16-bit), PADDSB/PSUBSB (8-bit) - and
      PEXT5/PPAC5 (GS 5551-pixel-format unpack/pack, rt-only). Ported
      from PCSX2's `MMI.cpp`. The saturated ops compute the sum/
      difference in a wider intermediate type and clamp to the lane
      width's signed min/max instead of wrapping on overflow/
      underflow. PEXT5 unpacks a 16-bit 5551 pixel (5/5/5/1 bits of
      R/G/B/A) into a 32-bit lane with each channel left-aligned in
      its own byte; PPAC5 is the exact inverse. This completes all
      defined MMI0 sub-opcodes. Unit tested in
      `tests/test_ee_mmi_sat.c` (21/21 checks, including a PEXT5/
      PPAC5 round-trip). Brings EE MMI coverage from ~48 to ~56 of
      the roughly 90 real opcodes.
- [x] MMI2/MMI3 permute/interleave family: PINTH/PINTEH (interleave
      Rs/Rt halfword lanes - PINTH takes Rt's ALL lanes plus Rs's
      UPPER lanes, PINTEH takes only the EVEN lanes of both - easy to
      confuse, kept as distinct implementations), PEXEH/PEXCH and
      PEXEW/PEXCW (each pair swaps a DIFFERENT lane pair - 0/2 for the
      "E" variant, 1/2 for the "C" variant - at halfword and word
      granularity respectively), PREVH (full reverse of the 4
      halfword lanes within each 64-bit half), PCPYH (broadcasts lane
      0 across the low half and lane 4 across the high half), and
      PROT3W (rotates word lanes 0,1,2 left by one, lane 3 untouched).
      All Rt-only except PINTH/PINTEH (which use both). Ported from
      PCSX2's `MMI.cpp`. Unit tested in `tests/test_ee_mmi_permute.c`
      (32/32 checks, using distinct position-identifiable lane values
      so a wrong permutation shows up immediately). Brings EE MMI
      coverage from ~56 to ~65 of the roughly 90 real opcodes.
- [x] PSLLVW/PSRLVW (MMI2, variable logical shift of a word pair -
      Rt's lanes 0/2, each shifted by the shift amount taken from the
      CORRESPONDING lane of Rs - lane 0's amount from Rs lane 0, lane
      2's amount from Rs lane 2, masked to 5 bits). Each 32-bit
      result is sign-extended to 64 bits into gpr.ud0/ud1 (matching
      every other 32-bit GPR result in this file), but the shift
      itself is a plain logical shift with no sign propagation -
      PSRLVW of `0x80000000 >> 4` is `0x08000000`, not `0xF8000000`.
      Ported from PCSX2's `MMI.cpp`. Unit tested in
      `tests/test_ee_mmi_pvshift.c` (6/6 checks). Brings EE MMI
      coverage from ~65 to ~67 of the roughly 90 real opcodes.
- [ ] Remaining ~23 MMI opcodes: QFSRV (needs the SA hardware register
      and MTSA/MTSAB/MTSAH to set it, none of which exist yet);
      PMADDW/H, PMSUBW/H, PMULTW/H, PDIVW/PDIVBW, PMULTUW/PDIVUW/
      PMADDUW (the remaining MMI2/MMI3 HI/LO-touching arithmetic -
      PMADDW/PMSUBW in particular have a documented real-hardware
      "division voodoo" rounding quirk in PCSX2's own source worth
      extra care when eventually ported); PMFHL/PMTHL clamping
      variants
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
      when both operands are negative). Also includes the FPU
      accumulator (ACC) family: ADDA.S/SUBA.S/MULA.S (write ACC),
      MADD.S/MSUB.S (read ACC, write fd = ACC +/- fs*ft), and
      MADDA.S/MSUBA.S (read+write ACC directly). Ported exactly from
      `pcsx2/FPU.cpp`, preserving a real hardware/PCSX2 quirk: MADD.S/
      MSUB.S run the intermediate fs*ft product through `fpuDouble()`
      a SECOND time before combining with ACC, but MADDA.S/MSUBA.S do
      not (direct accumulation) - a genuine asymmetry, not something
      simplified away for consistency. Unit tested in
      `tests/test_ee_fpu.c`, `tests/test_ee_fpu2.c` (13/13 checks,
      including both branch directions for BC1F/BC1T to prove they're
      not accidentally unconditional), and `tests/test_ee_fpu3.c`
      (19/19 checks, including a constructed overflow case that makes
      the MADD.S-vs-MADDA.S double-fpuDouble()-pass asymmetry directly
      observable: identical inputs give MADD.S's fd == 0.0 exactly but
      MADDA.S's ACC == +Fmax). Still missing: BC1FL/BC1TL ("likely"
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
- [x] TLB / MMU (48-entry TLB, TLBR/TLBWI/TLBWR/TLBP + KUSEG address translation - DONE, round 6)
- [x] Exception handling, KUSEG TLB misses only (BEV-dependent
      vectoring, EPC/Cause/Status.EXL, Cause.BD for delay-slot faults -
      DONE, round 7, see docs/STATUS.md. Still NOT done: general/
      interrupt/SYSCALL exception delivery through this same real path
      - RFE/ERET/EI/DI still only handle the return side for those,
      and SYSCALL still uses its own separate hand-written
      InstallExceptionHandlers trap rather than real vectoring. The IOP
      raises a real SYSCALL exception on its own side, see section 2,
      independent of this EE work.)
- [x] EE COP0 Count register (round 8): a real, free-running counter,
      advanced by 1 per instruction (a documented simplification - no
      cycle-accurate timing model exists to derive a precise bus-clock
      rate from - see docs/STATUS.md). Unblocked a real BIOS delay loop
      at `pc=0x9FC42500`.
- [x] EE Timer (Count==Compare) interrupt delivery (round 9): a real
      ExcCode 0/Int exception through the same `ee_raise_exception()`
      path round 7 built, gated by Status.IE/EIE/IM7/EXL/ERL exactly
      like PCSX2's own `cpuTestTIMRInts()`/`_cpuTestTIMR()`. Cause.IP7
      latches on Count>=Compare (verified against a real, live SCPH-
      10000 BIOS instruction sequence that overshoots an exact-equality
      check - see docs/STATUS.md), stays sticky until software writes a
      new Compare value. 32 host-native checks, 0 regressions. Live
      re-verification against the real BIOS found a real, more precise
      new wall: Cause.IP7 now latches correctly, but `Status.IE` is
      never set (no `EI` instruction executes at all) anywhere in the
      boot path this project's interpreter takes before reaching
      `pc=0xBFC0092C`'s idle loop - so a maskable timer interrupt isn't
      (yet, on this code path) what actually escapes it. See
      docs/STATUS.md's "round 9" section for the full evidence and open
      hypotheses - this is "round 10"'s starting point. IOP-side
      counters/timers remain register-stubs-only (`iop_timers.c`, no
      ticking/gating/target-IRQ behavior) and INTC (interrupt
      controller) more broadly is still needed for any other timing-
      dependent BIOS code.
- [x] EE JALR investigation round 10: root-caused (not yet fixed). The
      `pc=0xBFC0092C` idle loop is confirmed dead code on real hardware
      (0 hits) - real hardware takes the other branch at `pc=0xBFC0088C`
      because a subroutine call at `pc=0x9FC410E8` returns a positive
      value there; this project's interpreter gets `-1` from the same
      call. Traced fully: that subroutine is a BIOS SIO/UART baud-rate
      calibration loop (earlier "SIF init" labeling was wrong - the
      printed string is "Initialize memory (rev:%d.%02d, ctm:%dMhz,
      cpuclk:%dMhz %s)..."). It polls a hardware register at
      `0x1000F430` and requires at least 2 loop iterations before a
      polled value changes (`s3>=2` sanity check) or it returns error.
      This project's emulated SIO/UART responds instantly, so the loop
      only gets 1 iteration and trips the guard. Real hardware's actual
      UART timing takes enough polling cycles to clear it - same class
      of issue as round 8/9's COP0 Count timing simplification. See
      docs/STATUS.md's "round 10" section for the full trace. Next:
      round 11 should add modeled latency to the SIO/UART busy-bit
      poll so this calibration loop converges the way real hardware's
      does.
- [x] EE JALR investigation round 11: FIXED. Implemented MCH_RICM/
      MCH_DRD (RDRAM auto-init registers, `source/hw/mch.c`) per a
      verified PS2Tek/PCSX2 reference - round 10's "SIO calibration
      loop" framing was itself slightly off; these are memory-
      controller RDRAM-detection registers, not SIO. Also found and
      fixed a deeper bug: `ee_core.c`'s hardware-register MMIO
      dispatch compared the raw unmasked address against physical-
      style constants, so it never matched real KSEG0/1-addressed
      accesses (only the literal KUSEG-style addresses this project's
      own tests happened to use) - added `ee_hw_mmio_addr()` to mask
      KSEG0/1 addresses first, same aliasing `ee_mem_ptr()` already
      does. Live-verified against the real BIOS: the subroutine at
      `0x9FC410E8` now returns the exact real-hardware value
      (`0x08028020`), the branch at `pc=0xBFC0088C` matches exactly
      (`v0=0x02000000`), and `pc=0xBFC0092C`'s idle loop is never
      reached - all confirmed register-by-register against the
      original round 6 report. Step count to the fix (14.93M) lines up
      closely with round 10's live-traced ~14.9M real CPU cycles for
      the same call. Also added DADDI/DADDIU (found missing once
      execution reached ~100x further into real BIOS code than ever
      before). 29 new host-native checks across 3 new test files, 37-
      file/0-failure full regression. New, honest next wall: an
      unimplemented COP2 (VU0 macro mode) opcode deep in RAM-resident
      boot code - a legitimate new frontier, not a regression. Wii/
      devkitPPC rebuild NOT verified this round (this sandbox's
      devkitPro extraction is missing base_rules/libogc, a pre-
      existing, unrelated toolchain gap) - see docs/STATUS.md's
      "round 11" section.
- [x] EE JALR investigation round 12: COP2 (VU0 macro mode)
      control-register transfers (MFC2/CFC2/MTC2/CTC2) implemented as
      plain storage (`cop2_ctrl[32]`) - cleared a real BIOS init
      sequence doing a read-modify-write on FBRST (control reg 28) via
      cfc2/ori/ctc2, which halted on "unimplemented primary opcode
      0x12" since this project had zero COP2 dispatch before. Verified
      live against the real BIOS: correctly handles a second, different
      real use nearby (VU0 integer register 1, same CFC2/CTC2 family).
      New, confirmed-genuine wall: `viswr`, a real VU0 vector-datapath
      instruction (not a register transfer) - a separate, much larger
      subsystem (VF/VI register files, full VU macro arithmetic) left
      for a future round rather than half-implemented. 4 new checks,
      38-file/0-failure full regression.
- [x] devkitPro toolchain fully fixed, clean Wii rebuild verified:
      recovered the missing `cc1` (plain-C GCC front end) from a user-
      supplied Linux-native `devkitPPC-r32` archive, fixed a dangling
      `liblto_plugin.so` symlink from the same archive, fetched
      `libmpfr.so.4` from `archive.debian.org` (cc1 needs an older
      Debian Stretch-era mpfr than this Ubuntu 22.04 sandbox ships),
      and built `libfat` from source once real compilation worked.
      `make clean && make` now completes with 0 warnings/0 errors,
      producing a real `pcsx2-wii.elf`/`.dol` - the first Wii/devkitPPC
      rebuild actually verified this session, retroactively confirming
      rounds 9-12's C changes compile cleanly for the real target.
- [x] EE JALR investigation round 13: VU0 vector datapath implemented
      - VF[32][4] register file, VU0 local data memory (4KB),
      QMFC2/QMTC2 (128-bit transfers), VSUB (vector float subtract),
      VISWR/VSQI (VU0-mem stores, dispatched via a decoded SPECIAL2
      sub-table), VIADD/VISUB/VIAND/VIOR (VI integer ALU); VF00/VI0
      hardwired like real hardware. Cleared round 12's `viswr` wall for
      good and the subsequent VU0-register-clear routine. Also added
      LDL/LDR/SDL/SDR (64-bit unaligned load/store-left/right, the
      doubleword analog of this project's existing LWL/LWR/SWL/SWR) -
      a new, unrelated wall found immediately after. Live-verified: the
      interpreter now runs past 300M steps with no halt at all
      (previously halted at ~15.4M), settling into a bounded ~0x420-
      byte loop confirmed via live PCSX2 disassembly to be an ordinary
      SIF_SMFLG polling/debounce pattern - an honest steady state given
      this project's minimal IOP/SIF HLE model on a disc-less boot, not
      a bug. 16 new checks across two test files, 40-file/0-failure
      full regression, clean Wii rebuild.
- [x] GS: first flat-shaded triangle primitive - `source/hw/gif.c` now
      rasterizes `TRIANGLE`/`TRIANGLE_STRIP`/`TRIANGLE_FAN` (PRIM types
      3/4/5) via an edge-function scanline fill, alongside the
      existing SPRITE support. Single color per triangle (no per-
      vertex Gouraud shading/texturing/Z-test - honest simplification,
      see `include/core/hw/gif.h`). Vertex accumulation now supports
      STRIP continuation (rolling 3-vertex window) and FAN continuation
      (fixed anchor + rolling previous vertex), and correctly resets on
      any PRIM write so a primitive-type change mid-stream can't leak
      stale vertices into a new triangle. 13 new checks in
      `tests/test_gif_triangle.c`, 41-file/0-failure full regression,
      clean Wii rebuild.
- [x] Round 14: interleaved EE+IOP run confirms round 13's EE steady
      state is real; IOP side hits its own genuine wall - a live-
      traced `JALR $ra,$s1` with `$s1` holding an address (0x03400008)
      that only a real IOP module/IRX loader would ever populate (this
      project's `iop_hle_modules.c` is an explicit scaffold, not a
      real loader - not fabricated further, per this project's
      long-standing policy). Added a PC fetch-sanity guard to
      `iop_step()`: any fetch address outside real IOP RAM/BIOS ROM
      now halts immediately with a clear diagnostic naming the exact
      address, instead of silently "wandering" through unmapped
      memory for tens of millions of steps and halting confusingly far
      away. 7 new checks in `tests/test_iop_pc_guard.c`,
      44-file/0-failure full regression, clean Wii rebuild.
- [x] GS: Gouraud shading for triangles - PRIM's real IIP bit (bit 3,
      confirmed against PCSX2's own `GS/GSRegs.h`) now switches
      TRIANGLE/TRIANGLE_STRIP/TRIANGLE_FAN between flat shading and
      genuine per-vertex Gouraud interpolation. Added `tri_rgba[3]`
      color tracking alongside the existing vertex-position buffers;
      `rasterize_triangle()` blends per-pixel via barycentric weights
      derived from the existing edge-function values - plain affine
      (screen-space) interpolation, honestly noted as NOT the real
      GS's perspective-corrected (1/Q) interpolation. 9 new checks in
      `tests/test_gif_gouraud.c`, 45-file/0-failure full regression,
      clean Wii rebuild.
- [x] Wired a real GIF packet through DMA in main.c's on-device demo -
      previously the "pixels reach the screen" milestone wrote GS
      memory directly, bypassing DMA/GIF entirely. Now builds a real
      A+D-mode GIF packet, copies it into EE RAM, and calls
      `dma_channel_kick()` on the GIF channel - the same real pipeline
      (`dma_set_sink`/`gif_process_quadwords`) real EE code would
      drive - drawing a Gouraud triangle below the existing color
      bars. Promoted `GIF_REG_*`/`GS_REG_*`/`PRIM_TYPE_*`/
      `PRIM_IIP_MASK` from `gif.c`-private to public in `gif.h` for
      this. Caught a real NLOOP/buffer-size bug in the process (3 +
      n_verts instead of 3 + 2*n_verts - each vertex needs 2 register
      entries, not 1) via the Wii target compiler's `-Warray-bounds`.
      11 new checks in `tests/test_dma_gif_demo.c` (mirrors main.c's
      exact packet logic host-natively), 46-file/0-failure full
      regression, clean Wii rebuild.
- [x] Clock-rate-aware EE:IOP scheduler (8:1, was 1:1) -
      `system_run_interleaved()` now steps the EE `EE_IOP_STEP_RATIO`
      (8) times per 1 IOP step per slice, approximating real
      hardware's ~294MHz EE vs ~36MHz IOP clock ratio instead of the
      previous naive 1:1 round-robin. Still explicitly NOT
      cycle-accurate (per-instruction cycle costs aren't modeled on
      either core) - an honest, ratio-aware approximation, not a
      timing-fidelity claim. `tests/test_system_handshake.c` (the only
      existing consumer) exercises the new ratio automatically via its
      generous slice cap and still passes unchanged. 46-file/0-failure
      full regression, clean Wii rebuild.
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
- [x] Real IOP module/IRX loader (`source/hw/iop_elf.c` +
      `source/hw/iop_module_loader.c`) - this round replaced the
      module registry above's "standalone, nothing triggers it" state
      with a genuinely real loader: real ELF32/MIPS "IRX" parsing with
      relocation (R_MIPS_32/26/HI16/LO16), and a real ROMDIR/IOPBTCONF
      -driven sequential boot loader with export/import table linking,
      reverse-engineered from the user's own real BIOS (never
      committed) and cross-checked against ps2dev/ps2sdk's public
      `irx.h` and the community "PS2 BIOS in Rust" book. Live-traced
      against the real BIOS, this genuinely loads and executes real
      SYSMEM kernel code past the round-14 wall for the first time -
      see docs/STATUS.md's "Round 15" section for the full trace,
      including a second real bug found and fixed (a missing module-
      entry stack pointer). A third, deeper boundary (module-entry
      argument registers/boot-info block not modeled) was found the
      same round and **fixed in Round 18** - $a0 now points at a real
      2MB-RAM boot-info word, matching SYSMEM's own disassembled
      `lw v0,(a0); sll sp,v0,0x14` stack-pointer computation. Round 18
      also found and traced a knock-on consequence of the pre-fix bug
      (the bogus near-zero stack pointer had been silently overwriting
      the real exception-vector trampoline at address 0x80) - see
      docs/STATUS.md's "Round 18" section.
- [x] Real R3000A RFE (Restore From Exception, COP0 CO-format
      funct=0x10) - found and fixed in Round 22 while starting the
      user-directed "all IOP problems" sweep: the COP0 dispatch only
      ever handled MFC0/MTC0, so any real exception handler that
      tried to RFE-then-return would have hit an "unimplemented COP0
      sub-opcode" halt. Implemented per PCSX2's `R3000A.cpp`
      `psxException()`: `Status = (Status & ~0xF) | ((Status & 0x3C)
      >> 2)`. Unit tested in `tests/test_iop_rfe.c` (3/3 checks,
      hand-verified bit-for-bit through a full exception-entry-then-
      RFE round trip). See docs/STATUS.md's "Round 22" section.
- [x] Real exception-handler-chain mechanism at `RAM[0x100]`
      (`include/core/hw/iop_excb.h` / `source/hw/iop_excb.c`) -
      completed in Round 22 after re-fetching psx-spx's kernelbios
      page as raw markdown (the earlier rendered-HTML fetch had been
      truncated before reaching the needed section). Full citable
      reference recovered: 4 real priority chains (0-3), each a
      singly-linked list of 16-byte nodes (`00h`=next-pointer,
      `04h`=second-function ptr, `08h`=first-function ptr,
      `0Ch`=unused); `RAM[0x100]`/`RAM[0x104]` point at a real 4-entry
      chain-head array. `C(02h) SysEnqIntRP`/`C(03h) SysDeqIntRP`
      implemented byte-exactly, including SysDeqIntRP's documented
      real BUG (can only correctly remove a chain's first element -
      modeled as a safe no-op for any other position, since the real
      "garbage stack read" outcome isn't citable/reproducible).
      Deliberately NOT implemented: the real default handler CONTENTS
      (`EnqueueSyscallHandler`/`EnqueueTimerAndVblankIrqs`/`InitDefInt`)
      - these would need the real BIOS-ROM machine code bodies of
      SyscallException/VblankIrq/etc., which this project has no
      verified reference for and will not fabricate; the chains
      correctly start all-empty instead, matching the exact "before
      any handler is registered" scenario Round 19's trace hit. Unit
      tested in `tests/test_iop_excb.c` (18/18 checks). See
      docs/STATUS.md's "Round 19" and "Round 22" sections for the
      full trace.
- [x] Real hardware-interrupt delivery in the IOP interpreter (I_STAT
      & I_MASK -> a real Cause.ExcCode=0 "Interrupt" exception, gated
      by Status.IEc) - implemented in Round 22, same session as the
      RFE fix above. Cited from psx-spx's interrupts page (explicitly
      confirmed to apply to the PS2 IOP): every peripheral IRQ routes
      through ONE single CPU line, Cause.bit10 (IP2, non-latching -
      mirrors `I_STAT & I_MASK` live), taken once Cause.bit10,
      Status.bit10 (IM2), and Status.bit0 (IEc) are all set. New
      `iop_check_hw_interrupt()` in `iop_core.c`, called at the end of
      every real instruction step. Unit tested in `tests/
      test_iop_hw_interrupt.c` (8/8 checks - a real `SW` to I_MASK
      correctly preempts the very next instruction; `IEc=0` correctly
      blocks delivery even with `I_STAT & I_MASK` already nonzero).
      With this and the RFE fix above, `Status.IEc` finally has a
      real, observable, end-to-end effect for the first time in this
      project. See docs/STATUS.md's "Round 22" section.
- [x] Real B(00h) alloc_kernel_memory(size) - the real, genuinely
      executing BIOS ExCB/PCB/TCB setup code (confirmed via live
      Capstone disassembly at ROM ~0xbfc4ff90-0xbfc501f8) calls this
      via a thunk-table tail call (`jr`, not `jal`/`jalr` - why the
      earlier JAL/JALR-only trace found "zero calls to 0xB0/0xC0").
      Previously fell through to the generic default (`$v0=0`,
      "allocation failed"), so the real allocation logic correctly
      bailed out and RAM[0x100] never got a valid address. Now a real
      bump allocator over the documented Kernel Memory region (psx-spx:
      "0000E000h 2000h Kernel Memory; ExCBs, EvCBs, and TCBs allocated
      via B(00h)"), plus a companion fix in `iop_excb.c`'s
      `chain_head_addr()` to read `RAM[0x100]` dynamically instead of
      a hardcoded constant. `tests/test_iop_kmem_alloc.c` (19 checks).
      **Important honest caveat**: fixing this real gap does NOT, by
      itself, change how far boot progresses - a direct A/B trace to
      30M IOP instructions shows the pre-fix and post-fix builds land
      at the identical steady-state PC either way, because a separate,
      genuine block of ROM code unconditionally re-clears the whole
      low-RAM table-of-tables region (`0x000-0xf80`) shortly after the
      allocator succeeds. The real handler-registration step that
      would need to happen AFTER that clear has not yet been traced.
      See docs/STATUS.md's "Round 29 continued" section for the full
      story, including the two diagnostic-tooling bugs found and fixed
      along the way (JAL/JALR-only tracing missing tail calls; an
      unmasked KSEG1 address comparison missing real stores).
- [x] Real C(01h) EnqueueSyscallHandler + B(18h) ResetEntryInt - live
      disassembly fully mapped out the real exception dispatcher
      (0xc80-0xe98) and ReturnFromException (0xf30-0x1000), confirming
      the real BIOS calls both functions right after B(00h) succeeds.
      ResetEntryInt writes the real, ROM-confirmed jmp_buf pointer
      constant (RAM[0x7520]=0x6C34); EnqueueSyscallHandler installs a
      real, hand-assembled, position-independent MIPS trampoline
      (implementing psx-spx's exact SYS(01h)/SYS(02h) semantics,
      ending at the real ReturnFromException address) via the existing
      real SysEnqIntRP mechanism. `tests/test_iop_syscall_handler.c`
      (26 checks, including full execution of the installed bytes
      through the real IOP interpreter). **Still does not clear the
      ultimate wall**: a separate real ROM clear-loop wipes RAM[0x100]
      ~2.8M instructions before the dispatcher runs, well after these
      fixes take effect - see docs/STATUS.md's "Round 29 continued"
      (2nd fix) section for the precise next step (trace forward from
      the clear-loop's own return address rather than backward from
      the dispatcher).
- [x] **5th finding + fix (2026-07-08)**: real A(13h) setjmp(buf) and
      B(19h) HookEntryInt(addr) implemented (both previously fell
      through to the generic HLE no-op) after live call-tracing showed
      the real BIOS calls them back-to-back with the same address to
      install its own exception-fallback recovery point at
      RAM[0x7520] instead of the kernel default. Verified correct via
      `tests/test_iop_hook_entry_int.c` (18 checks) and live
      re-tracing (RAM[0x7520] now correctly ends up at the BIOS's own
      address). **Does not clear the wall**: re-running a long
      diagnostic after the fix produced an identical PC-for-PC trace.
      Precisely pinpointed the actual wall via direct disassembly: a
      bounded 4-pass retry loop (IOP RAM ~0x1011ac-0x101270) walking a
      linked table checking a 2-bit type tag per pass (resembling the
      A(96h)-A(99h) device-driver registration functions, none of
      which are implemented in this project), falling into a literal,
      deliberate panic routine (0x101278-0x101284: write status code 2
      to physical RAM 0, spin forever with SR=0/interrupts fully
      masked) when all 4 passes fail. See docs/STATUS.md's "Round 29
      continued (5th finding + fix)" section for the full trace and
      the concrete next step (disassemble backward from 0x1011ac to
      find what condition each retry pass actually tests).
- [ ] **7th finding (2026-07-08)**: full dynamic instruction tracing
      (not static disassembly) pinpointed the retry loop's root cause
      precisely: a specific stack slot (`$fp+0x40`) that a real IOP
      routine reads is zero/null at the time it runs, causing it to
      skip building its entry list entirely, leaving the list the
      4-pass retry loop walks genuinely empty on every pass. This
      routine is NOT the ROM-resident exception dispatcher already
      mapped this session, and its timing (~3.05M IOP instructions in,
      deep inside the LOGO-module execution window) makes it unlikely
      to be C(0Ch) InitDefInt directly, despite InitDefInt's
      documented job ("add some default IRQ and Exception handlers")
      being a tempting surface match. Most likely owner: the
      LOGO-loading IRX module itself, or a kernel helper it calls.
      Next step: trace backward from this routine's own entry/call
      site to find what real data source should have supplied
      `$fp+0x40` and why it's empty in this emulation. See
      docs/STATUS.md's "Round 29 continued (7th finding)" section.
- [ ] **9th finding (2026-07-08)**: traced `$fp+0x40` one level further
      - it's `RAM[0x100010+0x08]`, a field inside SYSMEM's OWN
      "boot info" structure (`source/hw/iop_module_loader.c`'s already-
      cited `BOOT_INFO_RAM_MB` work only populates word 0 of this
      struct; SYSMEM's own code reads 7 more words, offsets 0x04-0x18,
      and offset 0x08 being zero is what causes the empty list). No
      citable real byte layout found for what belongs at those seven
      offsets (checked psx-spx, ps2tek, PCSX2 upstream, and an
      independent detailed PS2-boot-process write-up) - fabricating
      values without evidence was avoided, matching this project's own
      standard. Next step: disassemble SYSMEM's own use of offsets
      0x08-0x1c (around 0x100d54-0x100e98) to infer plausible values
      from how each field gets used, the same approach that resolved
      the B(00h)/B(18h)/B(19h) findings. See docs/STATUS.md's "Round 29
      continued (9th finding)" section.
- [x] **6th change (2026-07-08)**: real A(96h) AddCDROMDevice() and
      A(97h) AddMemCardDevice(), per explicit user request ("add both
      as active devices, not as demo"). Implemented as real, queryable
      internal registration state (flags genuinely flip 0->1 and
      persist, idempotent on repeat calls) rather than a fabricated
      in-RAM DCB struct write, since psx-spx documents the DCB table's
      address/size but not a citable, byte-exact per-entry layout.
      Verified via `tests/test_iop_device_registration.c` (17 checks).
      **Honest empirical result**: live-tracing the real BIOS for 10M
      IOP instructions confirms neither function is ever called on
      this no-disc, no-memory-card boot path - both registration
      counters stay at 0, and the steady-state loop from the 5th
      finding is unchanged. This is real, valuable BIOS-function
      coverage (matters for disc/memory-card-aware paths and later
      game code), but does not resolve task #124/#132's wall by
      itself. See docs/STATUS.md's "Round 29 continued (6th change)"
      section.
- [x] **Reframing finding (3rd, same round)**: traced the clear-loop's
      own caller and found it's the real BIOS's own LOGO-loading
      dispatch (matches ROM string "LOGO", ROMDIR-style lookup, calls
      into a loaded IRX module at RAM 0x00030000 - task #92's real
      module loader). This module runs largely self-contained for
      ~2.8M instructions (only 2 FlushCache calls) BEFORE the
      exception-dispatcher wall this section has been chasing. This
      means the wall sits chronologically AFTER the real logo code,
      not before it - getting a real splash screen likely depends more
      on wiring the GS/display path into the real boot flow (section 8
      below / the "GS-Treiberpfad" task) than on further exception-
      dispatcher work. Not yet checked: whether the logo module's
      output reaches GS-visible memory. See docs/STATUS.md's "Round 29
      continued (3rd finding)" section.

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
- [x] VIF0/VIF1 (Vector Interface) - real VIFcode tag-stream walking
      (`source/hw/vif.c`), registered as the sink for
      `DMA_CHANNEL_VIF0`/`DMA_CHANNEL_VIF1`. Implements NOP/STCYCL/
      OFFSET/BASE/ITOP/STMOD/MARK (register stores)/FLUSHE/FLUSH/
      FLUSHA (correct no-ops)/MSCAL/MSCNT/MSCALF (run the real VU0/VU1
      microcode interpreter, see section 5)/STMASK/STROW/STCOL
      (stores)/MPG (writes real VU0/VU1 micro-instruction memory)/
      DIRECT+DIRECTHL (VIF1 only - forwards data straight to
      `gif_process_quadwords()`)/UNPACK (CMD 0x60-0x7F, task: "VIF
      UNPACK" - added this round, see below). Reference: `Vif.h`,
      `Vif_Codes.cpp` (VIFcode CMD table), `Vif_Unpack.cpp`/
      `Vif_Unpack.h` (UNPACK's own format) - all cross-checked live,
      not guessed.
      UNPACK itself decodes S/V2/V3/V4 (32/16/8-bit, plus V4-5's
      packed 16-bit format) data into VU0/VU1 local DATA memory
      (`vu0_mem_write32()`/`vu1_mem_write32()` - new this round,
      siblings of the existing MPG-facing `vu0_micro_write32()`/
      `vu1_micro_write32()`), with real STCYCL-driven CL/WL skip/fill
      cycles, STMASK/STROW/STCOL-based per-lane masking, and STMOD row
      accumulate/chain modes, ported directly from a live fetch of
      PCSX2's own `Vif_Unpack.cpp`. V3's real "reads 1 component past
      its own declared size" quirk (confirmed real hardware behavior
      games like Ape Escape 3 depend on, per PCSX2's own comment) is
      reproduced faithfully rather than "fixed". NOT implemented: a
      partial UNPACK payload split across multiple DMA calls (this
      project's `vif_process()` only ever sees one contiguous transfer
      at a time - flagged, not guessed). 12 new checks in
      `tests/test_vif.c` (23 -> 35 total), full regression still
      0-failure, clean Wii rebuild. See docs/STATUS.md's "Round 20"
      section for the full trace, including a subtle real-hardware
      timing quirk in fill-mode's advance-then-read ordering that
      needed hand-verification against the real algorithm before the
      test's own expectations could be trusted.
- [x] Texturing for the triangle rasterizer (task #85) - PRIM's real
      TME bit and TEX0's TBP0/TBW/TFX fields (cross-checked against
      PCSX2's own `GS/GSRegs.h`) now drive real texture mapping on
      TRIANGLE/TRIANGLE_STRIP/TRIANGLE_FAN. Nearest-neighbor PSMCT32
      sampling, DECAL and MODULATE TFX modes (HIGHLIGHT/HIGHLIGHT2
      simplified to behave like MODULATE), no CLAMP/wrap modeling.
      10 new checks in `tests/test_gif_texture.c`, 48-file/0-failure
      full regression, clean Wii rebuild.
- [x] Perspective-correct (ST+Q) texture coordinates + SPRITE
      texturing (task #88): PRIM's real FST bit (bit 8, cross-checked
      against PCSX2's own `GS/GSRegs.h` GIFRegPRIM) now selects
      between UV (FST=1, unchanged) and genuine perspective-correct
      ST+Q (FST=0) interpolation on TRIANGLE/TRIANGLE_STRIP/
      TRIANGLE_FAN - the standard 1/Q, S/Q, T/Q barycentric algorithm
      real GS hardware uses, verified with an exact-centroid test case
      (barycentric weights of exactly 1/3,1/3,1/3 for any triangle)
      that distinguishes genuine perspective correction from a plain-
      affine fallback. S/T (`GS_REG_ST` A+D write) and Q (bundled with
      `GS_REG_RGBAQ`'s high word, per real hardware) are real IEEE-754
      floats; TEX0's TW/TH fields (previously ignored) are now parsed
      to scale normalized ST into texel space. SPRITE gained real
      texturing too (previously flat-color only) via a new
      `rasterize_sprite()`, using a simpler, explicitly-documented
      per-corner-then-linear approximation rather than triangles' full
      per-pixel perspective correction (exact when a sprite's two
      corners share the same Q, which is the common real case). 15 new
      checks (`tests/test_gif_stq_sprite.c`), 51-file/0-failure full
      regression, clean Wii rebuild.

## 5. VU0 / VU1 (Vector Units)

Two dedicated 128-bit SIMD coprocessors with their own micro-code
instruction set (VU microcode, not MIPS). VU0 is also reachable as EE
COP2 ("macro mode"). Real PCSX2 has both an interpreter
(`VU0microInterp.cpp`, `VU1microInterp.cpp`) and x86 recompilers for
these - only the interpreter side is even theoretically portable.

- [x] VU0/VU1 micro-instruction memory - `include/core/hw/vu.h`/
      `source/hw/vu.c` (VU1: 4KB->16KB per PCSX2's `VUmicro.h`
      VU1_PROGSIZE; VU0's `vu0_micro[4096]` lives in `ee_state_t`
      alongside the existing round-13 vu0_vf/cop2_ctrl/vu0_mem fields,
      since real hardware shares one physical VU0 between macro mode
      and micro mode). MPG (`source/hw/vif.c`) now writes real
      microprogram bytes here instead of skipping them.
- [x] VU microcode interpreter - real control flow (TPC advance,
      E-bit delay slot, I-bit, branch delay-slot mechanism, byte-exact
      against a live fetch of PCSX2's `VU0microInterp.cpp`) PLUS, as
      of this round, a real per-opcode instruction table -
      `source/hw/vu_opcodes.h` (found in the original Sony "PS2 Vector
      Unit Instruction Manual" after PCSX2's own opcode tables
      couldn't be located in any fetched source file). Real upper
      FMAC arithmetic (ADD/SUB/MUL/MADD/MSUB/MAX/MINI + broadcast/Q/I
      forms, ADDA/SUBA/MADDA/MSUBA accumulator family, OPMULA outer
      product, ABS, ITOF/FTOI), real lower integer ALU (IADD/ISUB/
      IADDI/IAND/IOR), load/store (LQ/SQ/LQI/SQI/LQD/SQD/ILW/ISW/ILWR/
      ISWR/MTIR/MFIR/MOVE/MR32), and branches (B/BAL/JR/JALR/IBEQ/
      IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ) are implemented and tested (12 new
      targeted arithmetic/branch/load-store checks, not just control
      flow). A real accumulator register (`acc[4]`) was added; `vi[22]`
      is now the real Q register. Deliberately left unimplemented,
      per the no-fabrication policy (documented in vu_opcodes.h):
      OPMSUB (an irreconcilable encoding collision with MULbc.w found
      in the source manual), CLIPw, the R-register RNG family
      (RGET/RNEXT/RINIT/RXOR - needs a real LFSR model), the FC*/FS*/
      FM* MAC/status/clip flag ops, and IADDIU/ISUBIU (the source
      manual's immediate bit-packing for these two was ambiguous).
- [ ] VIF-side data unpacking into VU memory (VIF's UNPACK format -
      still needs a verified format reference, separate from the
      opcode-table work above)

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
- [x] Primitive rasterization - SPRITE (filled, flat-color, and now
      texture-mapped too - task #88) and TRIANGLE/TRIANGLE_STRIP/
      TRIANGLE_FAN (flat, Gouraud-shaded, and texture-mapped - DECAL/
      MODULATE TFX modes, nearest-neighbor PSMCT32 sampling, both UV
      "FST=1" and perspective-correct ST+Q "FST=0" coordinates, no
      CLAMP/wrap modeling), all via the GIF parser (`source/hw/gif.c`).
      POINT and LINE/LINE_STRIP (Round 21, task: "GS coverage
      breadth") are now implemented too - a single flat-color pixel
      (POINT) and real per-pixel-DDA-interpolated segments (LINE/
      LINE_STRIP, supporting Gouraud shading and linear Z, ported
      from PCSX2's `GSRasterizer::DrawEdgeLine`) - see
      `gif.c`'s `rasterize_point()`/`rasterize_line()` and
      docs/STATUS.md's "Round 21" section. No texture mapping for
      POINT/LINE (real hardware doesn't support it either).
- [x] Z-buffer / depth test (task #89, task 6) - real ZBUF_1/TEST_1
      A+D registers (`GIFRegZBUF`'s ZBP/PSM/ZMSK, `GIFRegTEST`'s ZTE/
      ZTST, cross-checked against PCSX2's GS/GSRegs.h), a genuine per-
      vertex Z value (from XYZ2's real Z word - only reachable via the
      PACKED-mode XYZ2 path; this project's pre-existing A+D XYZ2
      convention has no room left for Z, an honestly-scoped gap - see
      `include/core/hw/gif.h`'s `tri_z` field comment), barycentric
      (screen-space-linear, matching real hardware) Z interpolation
      for triangles and flat "second-vertex" Z for SPRITE, and the 4
      real `GS_ZTST` compare modes (NEVER/ALWAYS/GEQUAL/GREATER) gating
      both the color write and the (ZMSK-respecting) Z-buffer write.
      Z is stored as a plain 32-bit word via `gs_mem`'s existing
      PSMCT32-shaped helpers (no PSMZ32/24/16 format modeling). Gated
      behind this project's own `zbuf_configured` safety flag so any
      draw that never configures a Z buffer behaves exactly as before
      this round. 20 new checks (`tests/test_z_buffer.c`), 52-file/
      0-failure regression, clean Wii rebuild.
- [x] Alpha test + alpha blending (Round 23) - real `TEST_1` (`ATE`/
      `ATST`/`AREF`/`AFAIL`) and `ALPHA_1` (`A`/`B`/`C`/`D`/`FIX`)
      registers, cross-checked against PCSX2's GS/GSRegs.h and
      GSDrawScanline.cpp via a dedicated research pass. All 8 `ATST`
      compare modes and all 4 `AFAIL` outcomes (including `RGB_ONLY`'s
      old-alpha-byte preservation) implemented; blending gated by
      `PRIM`'s new `PRIM_ABE_MASK` bit, real truncating-divide blend
      equation `((A-B)*C)>>7+D` (no rounding bias - a real, if less
      strongly cited, hardware quirk), coefficients intentionally NOT
      clamped to [0,1] (real "boosted" blend results), final color
      clamped to [0,255] (COLCLAMP=1 default only). A new shared
      `gs_finish_pixel()` helper centralizes this logic across all 4
      rasterizers. 13 new checks (`tests/test_gs_alpha.c`), 57-file/
      0-failure regression, clean Wii rebuild. See docs/STATUS.md's
      "GS Round 23" section for full detail and citations. Still open:
      COLCLAMP=0's wrap-instead-of-clamp mode, and DATE/DATM
      (destination-alpha test, a separate mechanism from the alpha
      test above) remain unmodeled.
- [x] CLUT/paletted textures (PSMT8/PSMT4, Round 24) - TEX0's PSM
      field now actually parsed (previously ignored), plus its CLUT
      fields (CBP/CPSM/CSA/CLD). CLUT storage modeled as its own small
      gs_mem region at CBP with a 16-entries/row layout (CLUT_ROW_WIDTH,
      matching real hardware's 16-entry CSA-unit granularity); CSA
      selects a bank offset, letting multiple PSMT4 palettes share one
      CLUT region. PSMT8 implements the real CSM1 8-bit index swizzle
      (swaps index bits 3/4 before lookup). Only CPSM=PSMCT32 is
      supported (CPSM=PSMCT16/16S is a documented gap, matching
      gs_mem's own PSMCT32-only limitation); no real texture-upload/
      bit-packing path exists yet (indices are stored one-per-texel-
      slot, same simplification as every other texture format here) -
      that's covered by the separate REGLIST/IMAGE work below. This
      round's citation trail is weaker than usual: a live source-fetch
      research pass hit a session limit, so the PSM/CLUT field layout
      and the CSM1 swizzle are sourced from established PS2 GS
      knowledge rather than a fresh primary-source citation - flagged
      explicitly in code comments and docs/STATUS.md's "GS Round 24"
      section. 6 new checks (tests/test_gs_clut.c), 59-file/0-failure
      regression, clean Wii rebuild.
- [x] Real block-swizzled addressing, page/block level (Round 25) -
      NEW, ADDITIVE API (gs_mem_swizzle_addr32() + read/write
      wrappers in gs_mem.c/.h) alongside the pre-existing simplified-
      linear gs_mem functions, which the rendering pipeline (gif.c)
      continues to use unchanged. Real PSMCT32 page (64x32px, 8192
      bytes = 1 real BP unit) / block (8x8px, 256 bytes, 32/page,
      real non-linear 8x4 block-index grid) addressing is modeled;
      the finer within-block column pixel interleave is NOT (row-
      major within each block instead) - a documented, honest partial
      step. NOT wired into the rasterizer/texture/CLUT pipeline yet -
      doing so requires auditing/migrating every existing GS test's
      bp/bw picks to real-hardware-valid ranges (current tests use
      arbitrary large bp values like 5000/10200 that only make sense
      under linear addressing), which is explicit, left-open future
      work, not attempted this round to avoid a large, risky, cascading
      rewrite. This round's citation trail is again weaker than usual
      (session-limited research pass, same caveat as Round 24) -
      mitigated by a structural "no pixel-address collision across a
      full page" test property, which would likely fail if the block
      table were subtly wrong. 10 new checks (tests/test_gs_swizzle.c),
      60-file/0-failure regression (all 59 prior tests unmodified and
      still passing, since this is purely additive), clean Wii rebuild.
- [x] REGLIST/IMAGE GIF transfer modes (Round 26) - previously ANY
      non-PACKED GIF tag was byte-skipped with zero interpretation.
      REGLIST now parses real 2-registers-per-qword packing, routing
      every register through the existing apply_ad_write() (also
      fixed a real byte-accounting bug in the process: REGLIST's data
      span is ceil(NLOOP*NREG/2) qwords, not NLOOP as the old
      fallback assumed - would have desynced the GIF stream on any
      real REGLIST packet). IMAGE mode implements host-to-local
      (XDIR=0) transfers into a PSMCT32 destination, driven by real
      BITBLTBUF/TRXPOS/TRXREG/TRXDIR registers - local-to-host/local-
      to-local are parsed but not acted on (documented gap, no
      readback path or blit engine exists). Same session-limited-
      research caveat as Rounds 24-25 applies to the register field
      layouts. 15 new checks (tests/test_gs_reglist_image.c),
      61-file/0-failure regression, clean Wii rebuild.
- [x] GS Context 2 / dual-context support (Round 27) - previously
      only context 1 existed; PRIM's CTXT bit was never parsed. Added
      genuinely separate per-context permanent storage (ctx1_xxx/
      ctx2_xxx) plus a new gs_activate_context() called at the top of
      each of the 4 rasterizers, refreshing the pre-existing "active"
      fields from whichever context PRIM.CTXT selects - a deliberately
      non-invasive design that required zero changes to
      gs_finish_pixel()/gs_sample_texel()/gs_sample_clut() or the
      rasterizers' own bodies (validated by the full pre-existing
      61-test suite passing completely unmodified). Context 2 covers
      FRAME/XYOFFSET/TEX0/TEST/ALPHA/ZBUF only - CLAMP/TEX1/TEX2/
      SCISSOR/FBA/MIPTBP remain unmodeled for BOTH contexts, an
      existing, unrelated limitation. Same session-limited-research
      caveat as Rounds 24-26, mitigated by an internal self-
      consistency check (the real "_2 = _1 address + 1" pattern
      independently reproduced across 6 register pairs added in 5
      separate earlier rounds). Incidentally found and fixed a real
      tests/README.md doc-drift bug from Round 26 (a documented build
      command that would fail with a link error if used verbatim).
      10 new checks (tests/test_gs_context2.c), 62-file/0-failure
      regression, clean Wii rebuild.
- [x] Mipmap support (Round 28) - TEX1_1 (LCM/MXL/MMAG/MMIN/MTBA/L/K)
      and MIPTBP1_1/MIPTBP2_1 (per-level TBP/TBW for levels 1-6,
      modeled as a sequential 64-bit bitfield with no word-alignment
      padding) now parsed. SPRITE rasterizer performs per-primitive
      (not per-pixel) nearest-single-level LOD selection: LCM=0 uses
      a computed log2(texture-size/screen-size) ratio, LCM=1 uses a
      fixed K value; either way the result is clamped to MXL and
      falls back to level 0 whenever MMIN is below the "mipmapping
      enabled" threshold, the texture is being magnified rather than
      minified, or MTBA=1 (auto address calculation - documented,
      unimplemented gap, degrades safely to level 0 rather than
      misbehaving). Scoped to context 1 only and SPRITE only - real
      hardware also mipmaps TRIANGLE, and per-pixel/trilinear
      filtering is not modeled; both are documented gaps, not silent
      omissions. Implemented as a save/override/restore of
      tex_tbp0/tex_tbw around rasterize_sprite()'s pixel loop, so
      every other read site (gs_sample_texel, gs_sample_clut, the
      other 3 rasterizers) is untouched by design - same
      deliberately-non-invasive pattern as Round 27's dual-context
      work. Same session-limited-research caveat as Rounds 24-27
      (live source-fetch research hit this session's own usage limit
      again this round; the TEX1/MIPTBP bit layouts are sourced from
      established knowledge rather than a fresh citation trail). 25
      new checks (tests/test_gs_mipmap.c: TEX1/MIPTBP field
      round-trip including a negative sign-extended K and the two
      TBP fields that straddle the word0/word1 boundary; computed-LOD
      selection actually sampling a distinct mip buffer; MXL
      clamping; magnification always using the base level; MMIN
      below threshold disabling mipmapping; fixed-LOD K overriding
      the computed formula; MTBA=1's safe fallback; and a
      no-TEX1-configured regression check), 63-file/0-failure full
      regression, clean Wii rebuild.
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
- [x] **GS-Treiberpfad: main.c von Demo auf echten Boot-Flow umgestellt**
      (task #126/#128) - `run_real_boot_flow()` now runs automatically
      at startup (no menu gate, no fixed `DEMO_STEP_CAP`), loops the
      real BIOS boot in `BOOT_CHUNK_SLICES` increments up to a much
      larger `BOOT_TOTAL_CAP`, polls the real `pmode` register each
      iteration to detect an active GS display circuit, decodes the
      real hardware DISPFB1 field layout (FBP/FBW in real hardware
      units, not `gs_mem.h`'s simplified convention) when active, and
      calls `gs_blit_psmct32_to_xfb()` to present real GS memory to the
      Wii framebuffer instead of a hardcoded test pattern. See
      docs/STATUS.md's "Round 29 continued (4th change)" section.
      **Honest caveat**: per task #127's diag53 finding, GS registers
      stay at zero through the traced instruction window, so
      `display_active` is not yet expected to go true in practice -
      this is correct real scaffolding, not yet a proven splash
      screen. The next wall to chase is why the GS driver path is
      never exercised by the real boot code traced so far.

## 7. Supporting pieces (lower priority for "just the splash screen")

- [x] CDVD - real register-block scaffold for the no-disc boot case
      (2026-07-08). Base address (0x1F402000, mirrored across the
      full 4KB page) and power-on-reset register VALUES
      (Status=tray-open, Ready=drive-ready, DiscType=no-disc) are
      ported directly from real PCSX2 upstream source
      (`pcsx2/IopHw.cpp`, `pcsx2/CDVD/CDVD.cpp`'s `cdvdReset()`), not
      guessed. NCMD writes are latched and trigger a plausible
      completion IRQ rather than leaving BUSY forever, so a diskless
      boot's status-polling loop won't spin indefinitely - real
      N-command/S-command state machines (seek/read/etc) are
      deliberately NOT modeled, matching the existing
      iop_timers.c/iop_spu2.c "register scaffold" pattern. Verified
      via `tests/test_iop_cdvd.c` (19 checks). Honest empirical
      result: the real BIOS dump this project tests against never
      writes to CDVD registers within the traced window on its
      current no-disc boot path, so this doesn't (yet) change the
      steady-state outcome documented in the 5th/7th findings - see
      docs/STATUS.md's "Round 29 continued (8th change)" section.
- [x] SPU2 (audio) - PARTIAL, register scaffold only (not needed for a
      visual splash screen, added this round on a "time permitting"
      basis alongside the IOP loader/VU opcode-table work).
      `source/hw/iop_spu2.c` models the real, cited IOP-side base
      address (0x1F900000) and SPU2's real 16-bit-native register
      granularity (also 32-bit for LW/SW-based code), wired into
      `iop_core.c`'s `iop_mem_read16`/`write16`/`read32`/`write32`
      dispatch (16-bit MMIO dispatch didn't exist there at all before
      this round - only RAM/BIOS passthrough). No per-register
      (voice/ADSR/volume) semantics or actual audio synthesis/DMA
      pipeline are modeled - a real, addressable, persistent register
      file, not a real sound chip.
- [ ] Pad/memory card - not needed to reach the splash screen, needed
      for anything past it

## Suggested near-term order (rewritten Round 18 - the version below this
point had grown into a ~300-line blow-by-blow history of rounds 1-14
that duplicated, and had fallen behind, docs/STATUS.md's own per-round
log. That full history is preserved in STATUS.md; this section is now
kept short and current on purpose - update it, don't let it regrow into
a second history log.)

**Where things stand after Round 19**: both CPU cores run real,
substantial stretches of the actual SCPH-10000 BIOS (verified both in
host-native diagnostics and, as of Round 17, in a real Dolphin session
on the actual Wii .dol). Neither core has halted/crashed in the
traditional sense in a long time - both reach genuine steady states.
The EE legitimately polls real SIF mailbox/flag registers waiting for
the IOP (Round 17's disassembly-confirmed finding). The IOP, after
Round 18's boot-info fix, cleanly reaches what disassembly confirms is
a real BIOS panic/halt loop - Round 19 traced this precisely and found
it is NOT a missing SYSCALL dispatch table (that Round 18 hypothesis
was corrected): it's a genuine, real exception dispatcher correctly
handling an ordinary `ExitCriticalSection()` syscall, whose
handler-chain lookup at `RAM[0x100]` finds no handler registered yet
and falls through to the same module-loader escape hatch Round 15
already documented, rather than truly returning from the syscall.

**Next concrete tasks, roughly in dependency order:**

1. **Real exception-handler-chain default behavior at `RAM[0x100]`**
   (section 2, bullet corrected in Round 19) - the immediate next wall,
   more precisely scoped than originally thought. Not a missing
   SYSCALL-number dispatch table (Round 18's original hypothesis, ruled
   out by Round 19's precise trace) - the real gap is two-part: (a) what
   real IOP kernel init does at `RAM[0x100]` before any handler chain
   entry is registered (needs a citable reference, not a guess), and
   (b) why `Status.IEc` (global interrupt enable) never gets set to 1
   anywhere in the traced execution. See docs/STATUS.md's "Round 19"
   section for the full trace. UPDATE (Round 29 continued, 12th
   change): the boot_info struct offset-0x0C fix moved the IOP steady
   state from pc=0x00101284 to pc=0x001012A8 (real forward progress
   through a function-pointer-table dispatch loop, not a relocated
   identical wall) - the panic loop itself is still eventually hit.
   Task #124/#132 remains open and is still being actively chased per
   the user's explicit direction.

2. **CDVD (disc) stub** (section 7) - DONE (2026-07-08), see the
   register-block entry in section 7 above. Register-level scaffold
   only (no real command state machine); the real BIOS dump tested so
   far never exercises it within the traced window, so the EE's own
   boot path is not yet observed to depend on it.

3. **COP2 (VU0 macro mode) wiring** (section 5) - VU0's vector datapath
   and microcode interpreter both exist (Round 13/14). QMFC2/QMTC2/
   CFC2/CTC2 (32/128-bit transfers) and a growing set of macro-mode
   vector ops (VSUB/VISWR/VSQI/VIADD/VISUB/VIAND/VIOR from round 13;
   VADD/VMUL/VIADDI added Round 29 continued's 11th change; VMAX/VMINI
   added Round 29 continued's 16th change; VMADD/VMSUB/VOPMSUB added
   Round 29 continued's 17th change, completing the full
   VADD/VMADD/VMUL/VMAX/VSUB/VMSUB/VOPMSUB/VMINI SPECIAL1 arithmetic
   row; VADDx/y/z/w, VSUBx/y/z/w, VMAXx/y/z/w, VMINIx/y/z/w,
   VMULx/y/z/w (the FT-lane-broadcast forms, 20 opcodes) added Round
   29 continued's 18th change; VMADDx/y/z/w/VMSUBx/y/z/w and
   VMULq/VMAXi/VMULi/VMINIi added Round 29 continued's 19th change,
   completing the entire funct 0x00-0x2F COP2 CO-format arithmetic
   space; VABS/VITOF0/4/12/15/VFTOI0/4/12/15/VMOVE/VMR32 added Round
   29 continued's 20th change, the first COP2SPECIAL2 opcodes besides
   VISWR/VSQI; the entire accumulator-writing family (VADDA/VMADDA/
   VMULA/VSUBA/VMSUBA/VOPMULA/VNOP + their broadcast forms, ~37
   opcodes) added Round 29 continued's 21st change) are wired up and
   tested. Real BIOS boot code very likely uses VU0 macro mode for the
   splash screen's transform/lighting math - NOT YET REACHED by the
   current boot trace (EE is still steady-state SIF-polling), so this
   remains readiness work rather than a wall-clearing fix. Open:
   VCLIPw (needs a new CLIP flag register), the memory-access family
   beyond VISWR/VSQI (VLQI/VLQD/VSQD), VMTIR/VMFIR/VILWR (a different
   sub-field decode, not yet researched), VDIV/VSQRT/VRSQRT/VWAITQ
   (would need to model the Q register's real "busy" timing, not just
   its value), and VRNEXT/VRGET/VRINIT/VRXOR (the VU0 R-register LCG
   pseudo-random generator - separate state).

4. **VIF UNPACK** (section 4) - DONE (Round 20). Real vertex/
   texture/attribute data now flows from EE RAM into VU0/VU1 local
   data memory via UNPACK, ported from a live-fetched, real PCSX2
   source (`Vif_Unpack.cpp`). See section 4's bullet and
   docs/STATUS.md's "Round 20" for the full detail.

5. **GS coverage breadth** (section 6) - primitives (flat/Gouraud
   triangles, textured sprites, Z-test, POINT/LINE/LINE_STRIP - Round
   21, alpha test/blending - Round 23, CLUT/paletted textures - Round
   24, real block-swizzled addressing - Round 25, REGLIST/IMAGE GIF
   transfer modes - Round 26, GS context 2 - Round 27, mipmaps -
   Round 28) exist, but this is still a sliver of real GS - real
   PCSX2's own GS code is ~114,500 lines. All five items the user
   directed for this session (CLUT, block-swizzled addressing,
   REGLIST/IMAGE, GS context 2, mipmaps) are now DONE, each
   individually committed/pushed/rsynced as its own checkpoint. Open,
   not user-directed: actually wiring Round 25's real swizzle
   addressing into the rendering pipeline (currently a separate,
   additive API only); extending Round 27's dual-context support to
   CLAMP/TEX2/SCISSOR/FBA (TEX1/MIPTBP DONE, Round 29 continued's
   15th change - these registers don't exist in this codebase at all
   yet for either context, a separate/larger gap than TEX1/MIPTBP's
   was); extending Round 28's mipmap support to per-pixel/trilinear
   filtering (currently per-primitive, nearest-single-level -
   TRIANGLE coverage DONE, Round 29 continued's 14th change); and
   implementing MTBA=1 auto mip-address calculation (currently falls
   back to level 0).

6. Lower priority, deferred: the remaining ~23 EE MMI opcodes (section
   1), Pad/memory card (section 7).

**Honest distance-to-splash-screen assessment (Round 18)**: this
project has now spent many rounds (see STATUS.md in full) each finding
and fixing one real, concrete, well-evidenced bug or gap - and each fix
has reliably uncovered a NEW wall a bit further in, never yet reaching
a natural end-of-boot condition. That pattern is likely to continue:
item 1 above (the exception-handler-chain / IEc gap) is itself a
substantial, multi-round undertaking on the scale of the module loader
work (Round 15), and there is no guarantee it's the LAST such wall
before the splash screen - CDVD and COP2/VIF work (items 2-4) are
independent prerequisites likely to surface their own walls once
reached. A grounded estimate: this is more "several more focused rounds
of real investigation-and-fix work, each on the scale of Round 15/18"
than "one or two more fixes." Nobody should read the current EE/IOP
steady states as "almost there" - they're real, verified progress, but
a real PS2 BIOS boot sequence is an extremely long, precisely
sequenced process, and this project is still resolving its early
kernel-initialization phase, well before the point real hardware would
start issuing GS draw calls for the splash logo itself.
