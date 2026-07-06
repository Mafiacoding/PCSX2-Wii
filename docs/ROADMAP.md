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
yet solved, unlike the two IOP fixes above, but a second investigation
round (see docs/STATUS.md's "EE JALR investigation, round 2") pinned
the mechanism down at the byte/instruction level using a purpose-built
tracing harness (instruction ring buffer + full RAM shadow-diff): the
`$s3=*(0x100)`, `$s6=*($s3+0)`, `$s1=*($s6+8)`, `JALR $ra,$s1` chain is
confirmed exact via the real decoded instructions; `RAM[0x100]` is
confirmed written by NO instruction across the whole ~99,261-instruction
run; and - the new finding - `RAM[0]`/`RAM[8]` are NOT simply
zero-by-default, they resolve through a real exception-vector-trampoline
scratch buffer this same boot code builds (at ~instruction 84,143,
mirroring the IOP's own InstallExceptionHandlers convention
independently on the EE side), actually jumps to and executes (confirmed:
pc really does reach address 4 and execute the installed bytes as
code), and then only partially cleans up (bytes 0-3 get re-zeroed
afterward, bytes 4-11 are leftover). So the eventual bad JALR target
(`0x03400008`) is the raw encoding of that leftover JR instruction
word, misread as a function pointer through a chain that was meant to
reach a different, legitimately-populated structure - not a
"zero-decodes-as-NOP" coincidence like the earlier LQ/SQ finding.
Root cause of why `RAM[0x100]` itself is never populated remains
genuinely unresolved. A third round tested both hypotheses directly:
DMA is ruled out (every DMA channel register is still completely zero
at the point of the fatal JALR - nothing has been kicked), and the
"missed guard" idea is ruled out for the immediate window (the ~90
instructions between the trampoline landing and the fatal JALR contain
zero branch/jump-and-link opcodes - pure straight-line code, nothing to
misjudge). The most plausible remaining explanation is a real
hardware pre-boot RAM-population step (before the first CPU
instruction even runs) that this project's boot model has no
equivalent of - but there is no citable public reference for this,
unlike psx-spx/ps2tek elsewhere in this project, so no fix is being
applied rather than fabricate BIOS-internal behavior. See
docs/STATUS.md's "round 3" section for the full reasoning and the two
honest paths forward (a real COP0 TLB/exception system as its own
feature, or accepting this as a documented limitation for now). A
fourth round used the real BIOS dump itself as ground truth (the user's
own legal dump - disassembling it is not fabrication): this killed the
EE-SYSCALL-exception hypothesis outright (SYSCALL fires zero times in
150K instructions), confirmed the interpreter never actually halts
after the bad JALR (it wanders through zeroed memory for 3M+ more
instructions, `RAM[0x100]` staying zero throughout), and chased down
what looked like a strong lead - an apparent copy loop whose `LW`/`SW`
instructions read back as zero - only to resolve it as an ordinary,
expected heap-allocator initialization pattern (a real BIOS routine at
`pc=0xBFC4D30C` zeroing one word per 16 bytes across low RAM, ordinary
free-list-header clearing, not a bug). See docs/STATUS.md's "round 4"
section for the full trace.

**Round 5 found and fixed the actual root cause.** The user connected a
real, working PCSX2 instance to live-trace their own BIOS dump and
captured what this project never had: ground truth for what real
hardware does. `RAM[0x100]` really does hold a nonzero, valid vector
(`0x08004469`) on real hardware, written by a real ROM-resident
vector-install routine at `pc=0xBFC00C54-0xBFC00CB4` - which this
project's interpreter never reaches at all. The reason: this project's
`ee_core_init()` never set COP0 register 15 (PRId), leaving it at 0.
The real BIOS's instruction #0 is `MFC0 $k0,$15`, and instruction #3 is
a branch on a CPU-revision check (`SLTI $at,$k0,89`/`BNE`) that sends
boot down one of two completely different paths depending on this
register. With PRId=0 this project took the wrong path from the third
instruction of the entire boot sequence onward and never rejoined the
real vector-install code. **Fixed**: `cop0[15] = 0x00002e20`, ported
directly from PCSX2's own `R5900.cpp` (not guessed - the same constant
real PCSX2 uses). Verified as a real fix, not another false-progress
trap: a ROM-coverage re-trace confirms the EE now takes the correct
branch and runs through code it never reached before. The EE now halts
cleanly, honestly, and much later - at `pc=0xBFC0086C`, on `TLBWI`
(a real COP0 TLB instruction this project has always documented as
unimplemented) - not on the old JALR-to-out-of-range bug, which no
longer occurs at all with this fix in place. See docs/STATUS.md's
"round 5" section for the full trace, including the live PCSX2 data
that made finding this possible.

**Update (round 6): the COP0 TLB is now implemented** -
`TLBR`/`TLBWI`/`TLBWR`/`TLBP` plus real KUSEG address translation via
a 48-entry `tlb[]`, ported from PCSX2's own `COP0.cpp`/`R5900.h`
(`tests/test_ee_cop0_tlb.c`, 9/9 checks). Continuing the same trace
past this fix also turned up and fixed three more real gaps: the
kseg0 ROM mirror (`0x9FC00000+`) wasn't recognized as ROM; the MIPS
"Branch Likely" family (`BEQL`/`BNEL`/`BLEZL`/`BGTZL`/`BLTZL`/`BGEZL`/
`BLTZALL`/`BGEZALL`) was entirely missing; and `LWC1`/`SWC1` were
entirely missing. See docs/STATUS.md's "round 6" section for the full
trace and a note on a pre-existing test (`test_ee_unaligned.c`) whose
own premise (an unmapped raw KUSEG address) had to be corrected once
KUSEG started requiring a real TLB entry, same as real hardware.

With all four fixes in place, real-BIOS boot progresses further but
still diverges - at instruction #158-159, into a genuine **TLB miss**:
the BIOS sets up `$sp = 0x70003eb0`, which falls just outside the one
TLB entry (`tlb[0]`) the boot path installs before that point. This is
not a translation bug (confirmed via the passing test suite) - it's an
honestly-reached wall pointing at the next real blocker: this project
has no MIPS exception delivery at all (no Cause/EPC/Status handling,
no vectoring to the BIOS's own exception vectors), which is
presumably what real hardware relies on here to install the missing
TLB entry on demand (a TLB Refill exception).

**Update (round 7): real exception delivery is now implemented** -
`ee_raise_exception()`/`ee_raise_tlb_exception()`, ported from PCSX2's
`cpuException()`/`cpuTlbMiss()` in `R5900.cpp`: Cause/EPC/Status.EXL
updates, Status.BEV-dependent vectoring (ROM vs. RAM base), correct
Cause.BD/EPC bookkeeping for faults inside a branch-delay slot (which
needed real delay-slot tracking added too - see docs/STATUS.md's
"round 7"), and a nested-exception path matching real hardware's rule
of freezing EPC and forcing the general vector. Tested in
`tests/test_ee_exceptions.c` (16 checks) plus an updated
`tests/test_ee_cop0_tlb.c` KUSEG-miss case (rewritten to check the real
exception fires correctly, replacing its old "reads as 0" assumption).

Verified as real, dramatic progress against the actual SCPH-10000
BIOS: a 20-million-instruction run now executes 97.62% real
(non-zero-decoded) instructions, compared to 0.0008% (151 out of 20
million) before this fix - and the code being executed in the range
the trace spends most of its time in was confirmed by disassembly to
be genuine MIPS exception-handler prologue (a full GPR context save
followed by saving EPC/Cause), not zero-decoded filler.

**New wall**: exactly two exceptions fire in a 50-million-instruction
run (the original TLB miss, then an immediate nested fault when the
handler's own register-save routine touches a different unmapped
KUSEG page) - after that, Status.EXL never clears (no ERET) and the EE
just keeps running real code in that same handler-prologue region
without resolving, for tens of millions of instructions. This looks
architecturally like a missing "wired" TLB entry situation (real MIPS
kernels reserve a few TLB entries, via `COP0.Wired`, specifically so
kernel/handler code and its own scratch memory can never TLB-miss while
already servicing a miss) - this project doesn't implement `Wired` at
all, and/or the real boot path may install more `TLBWI` entries by this
point than this project's trace has executed so far.

**Update (round 8): both root causes found and fixed via a second live
PCSX2 trace** (same PCSX2-MCP bridge as round 5). The "wired TLB entry"
guess above was superseded by a more precise finding: the faulting
address (`$sp=0x70003FC0`) falls inside the R5900's real Scratchpad RAM
(SPR) - a fixed 16KB window (`0x70000000-0x70003FFF`) that hardware
bypasses the TLB for *entirely*, confirmed both by the live trace and
PCSX2's own source (`pcsx2/Memory.cpp`'s "scratch pad" comment,
`pcsx2/MemoryTypes.h`'s 16KB `Ps2MemSize::Scratch`, `pcsx2/COP0.cpp`'s
`isSPR()`-gated direct-buffer mapping). Fixed by intercepting this fixed
range in `ee_mem_ptr()` before any TLB lookup, routing to a dedicated
`scratch[]` buffer. A second wall immediately followed once that loop
resolved: COP0 Count never advanced (see the checkbox above) - fixed by
incrementing it once per instruction.

With both fixes, an 800-million-instruction run against the real
SCPH-10000 BIOS raises zero exceptions (down from 2-then-stuck-forever)
and reaches a genuinely new region: `pc=0xBFC0092C`, a real `j $`
(`J 0xBFC00928`) self-loop, immediately preceded by a `Compare=1` timer
setup a few instructions earlier. This is a real, intentional
"wait for interrupt" idle pattern - real hardware escapes it via an
actual interrupt (very plausibly the Timer/Compare-match interrupt this
setup is meant to trigger), which this project's EE core has never
raised at all.

**Update (round 9): EE Timer (Count==Compare) interrupt delivery
implemented and tested** - a real ExcCode 0/Int exception through the
same `ee_raise_exception()` path round 7 built, gated exactly like
PCSX2's own `cpuTestTIMRInts()`/`_cpuTestTIMR()`
(Status.IE/EIE/IM7/EXL/ERL). Cause.IP7 latches via Count>=Compare, not
exact equality - a live real-BIOS instruction sequence
(`MTC0 Count,0` then, two instructions later, `MTC0 Compare,1`) proved
exact equality would silently miss the match, since Count already
overshoots past 1 before Compare is even written. 32 host-native
checks pass, including that exact overshoot scenario reproduced
verbatim. Re-verifying against the real SCPH-10000 BIOS found a more
precise new wall, though: Cause.IP7 now latches correctly (confirmed
directly), but `Status.IE` is never set anywhere in the boot path this
project's interpreter takes before reaching the idle loop - a targeted
trace confirmed zero `EI` instructions execute in the first 5 million
steps. So a maskable Count/Compare interrupt isn't (at least not
via this code path) what actually escapes this loop; something else -
a wrong earlier branch, a different interrupt source, or an
unexplored code path through the surrounding `JAL`/`JALR` calls - is
the real next question. See docs/STATUS.md's "round 9" section for
the full trace, evidence, and open hypotheses.

The
IOP has no known halt point left
to chase at all right now; COP2/VU0 remains unstarted and unproven
against real BIOS code, since the EE hasn't legitimately run far
enough to demonstrate needing it yet.

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
   uses, output to Wii GX framebuffer - PARTIAL: SPRITE and now
   flat-shaded TRIANGLE/TRIANGLE_STRIP/TRIANGLE_FAN work via a
   direct-to-XFB pixel blit (not real GX; edge-function scanline fill,
   single color per triangle, no Gouraud/texturing/Z-test - see
   `include/core/hw/gif.h`'s scope comment), textures still open, and
   this whole path is still not driven by real BIOS/EE code since IOP
   HLE isn't wired up yet

Remaining near-term candidates, roughly in order of how directly they
unblock "the BIOS actually draws something": IOP hardware register
stubs (INTC/DMA/timers) and/or IOP HLE stubs for the specific
BIOS-boot-path modules (both are needed before real BIOS code - as
opposed to hand-written test programs - can get through a real SIF
handshake), texturing for the triangle rasterizer (Gouraud shading, a
clock-rate-aware 8:1 EE:IOP scheduler, and wiring a real GIF packet
through `dma_channel_kick` at boot are all DONE now - see above), and
VIF0/VIF1 passthrough.

Step 7 (real GX-based rendering with textures) is where this stops
being "a lot of careful work" and becomes genuinely research-scale for
a solo project - see the GS line count above.
