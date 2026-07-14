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
- [x] Final 19 MMI opcodes (Round 64, 97th finding) - the project's
      own prior "~23" estimate was corrected to a precise 19 by
      cross-referencing PCSX2 v1.6.0's real `tbl_MMI[64]`/
      `tbl_MMI0-3[32]` encoding tables (several previously-assumed-
      missing opcodes - PLZCW, MFHI1-MTLO1, PSLLW/PSRLW/PSRAW - turned
      out to already be implemented). MADD1/MADDU1 (pipe-1 HI:LO
      accumulate, matching this project's existing MULT1/DIV1/MFHI1
      pipe-1 convention); QFSRV (128-bit funnel-shift using the SA
      register, already implemented via MFSA/MTSA in task #177);
      PMADDW/PMSUBW/PMULTW/PDIVW, PMADDH/PHMADH/PMSUBH/PHMSBH/PMULTH,
      PDIVBW (real division-by-zero "voodoo" preserved: LO=+-1 by
      divisor sign, HI=dividend); PMADDUW/PSRAVW/PMULTUW (full raw
      64-bit unsigned product into GPR, a distinct behavior from the
      sign-extended-low32 LO/HI split)/PDIVUW; PMFHL/PMTHL clamping
      variants. All ported bit-exact from PCSX2 v1.6.0's `MMI.cpp`
      (current master no longer has interpreter MMI bodies). Unit
      tested in `tests/test_ee_mmi_hilo2.c` (36/36 checks, including
      the even/odd-lane GPR-capture asymmetry for PMULTH-family
      opcodes). Brings EE MMI coverage to complete: every real MMI
      opcode in the R5900 SPECIAL2 encoding space now has a real
      implementation. Full 90-test regression suite: 90/90 pass.
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
- [x] COP2 (VU0 macro mode) - VU0 running as a COP2 coprocessor
      attached to the EE pipeline (reference: `pcsx2/VU0.cpp`,
      `COP2.cpp`). Stale checkbox corrected in Round 64's audit: this
      was actually implemented across many rounds (tasks #71, #133-
      #147) - control-register transfers (round 12), VF/VI regs plus
      macro arithmetic datapath (round 13), then the full VADD/VSUB/
      VMUL/VMADD/VMSUB/VOPMSUB family and all four broadcast forms
      (x/y/z/w), the SPECIAL2 accumulator family (VADDA/VSUBA/VMULA/
      VMADDA/VMSUBA/VOPMULA/VNOP), VABS/VITOF/VFTOI/VMOVE/VMR32,
      VLQI/VLQD/VSQD/VSQI, VMTIR/VMFIR/VILWR, the R-register LCG
      (VRNEXT/VRGET/VRINIT/VRXOR), the Q-register family (VDIV/VSQRT/
      VRSQRT/VWAITQ), and VCLIPw with its own CLIP flag register.
      Still missing/unverified: OPMSUB's cross-product ordering quirk
      beyond the basic VOPMSUB, and the R-register RNG's exact
      bit-for-bit hardware LCG constants (matched against PCSX2's own
      implementation only, not independently verified against real
      hardware output).
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
- [x] **7th finding (2026-07-08)**: full dynamic instruction tracing
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
- [x] **9th finding (2026-07-08)**: traced `$fp+0x40` one level further
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
      **Stale-checkbox correction (Round 64 audit)**: these two
      findings' proposed next step (populate boot_info offsets
      0x08-0x18 by inferring values from SYSMEM's own use of them) was
      NOT how the underlying wall was ultimately resolved. Instead,
      later rounds (task #134's boot_info offset 0x0C fix, then the
      LOADCORE module-registration-list work in tasks #124/#132/
      #148-#163) found and fixed the real root cause via a different,
      more direct path, and boot now passes all module loading (task
      #219). Both entries are marked done here because the problem
      they were investigating is conclusively closed, not because
      their specific proposed next step was the one that closed it.
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
- [x] At minimum: the channels needed for BIOS boot to push data to
      GIF (graphics) and to talk to the IOP over SIF. Stale checkbox
      corrected in Round 64's audit: both are real and exercised
      extensively - GIF via `dma_set_sink()`/`DMA_CHANNEL_GIF` (see
      section 4 below), and SIF0/SIF1/SIF2 via `DMA_CHANNEL_SIF0`
      register decode (`source/hw/dma.c`) plus the many rounds of real
      SIF RPC/reply-queue work in `ee_core.c` (tasks #164-#219,
      Round 53-55's real reply-queue implementation).

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
- [x] VIF-side data unpacking into VU memory (VIF's UNPACK format).
      Stale checkbox corrected in Round 64's audit: implemented in
      `source/hw/vif.c`'s `vif_unpack()` (task #107) - real VN/VL
      geometry decode (S/V2/V3/V4, 32/16/8/5-bit component widths),
      signed/unsigned extension (USN), masked writes, CL/WL fill-mode
      skip-byte handling, and the V4-5 real 5/5/5/1 bit-packed special
      case - writing real values into VU0/VU1 memory via
      `vu0_mem_write32`/`vu1_mem_write32`, cross-checked against
      PCSX2's `Vif_Codes.cpp`/`Vif_Unpack.cpp` dispatch tables.

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
   CLOSED (Round 29 continued, 27th finding): a fresh, exact-address
   disassembly (fed this project's own relocated IOP RAM bytes, not
   raw ROM) definitively corrected a 3-round-old misattribution - the
   wall sits in LOADCORE's own init code (module 1 of 29, entry
   0x100CD0), not SYSMEM - and precisely re-characterized the empty
   table as LOADCORE's own multi-phase module/library self-
   registration list (not a "device driver" table), populated from a
   real, previously-undocumented per-entry processing loop
   (0x100FD0-0x101184) that genuinely calls through function pointers
   via jalr. This project's boot_info[0x18]/[0x1C] fields (which feed
   that loop) are 0 because this project's loader runs exactly one
   module's ELF and entry point at a time, so no other module has
   registered anything by the time LOADCORE reaches this code - a
   real, well-evidenced structural finding. Formally closed without a
   further code change: unlike every prior defensive fix in this
   project (INITIAL_SP, boot_info+0x0C), a fix here would need to
   construct real entries consumed by a genuine jalr dispatch, and an
   incorrect guess at that struct's layout does not fail safely (it
   can jump into arbitrary memory as code) - a categorically different
   and unacceptable risk. See docs/STATUS.md's 27th finding for the
   full disassembly, the four still-unreverse-engineered helper
   subroutines (0x1018d0/0x101f30/0x102120/0x10198c), and the two
   scoped options for whoever pursues this further. UPDATE (Round 29
   continued, 28th change): implemented a safe, byte-signature-based
   (not address-based) recognition of the exact panic instruction
   sequence itself (`lui $v1,0x8000; addiu $v0,zero,2; sb $v0,($v1);
   j <self>`), distinct from - and much lower-risk than - trying to
   populate the real registration list (that would need a genuine
   `jalr` target, this doesn't). On recognizing it, the loader
   advances to the next module, the same way it already does at its
   own trampoline return address. Measured, real result: `LOADCORE` →
   `EXCEPMAN` (hits its OWN copy of the same panic sequence, also
   bypassed) → `INTRMANP` now all load and run for real against the
   actual SCPH-10000 BIOS, reaching a new, clean stop at
   `pc=0x00000018`/`BREAK` (a real, cleanly recognized instruction,
   not a crash). Tested via `tests/test_iop_loadcore_panic_bypass.c`
   (9 checks, including 2 negative controls proving the match isn't
   overbroad). UPDATE (Round 29 continued, 29th change): root-caused
   and fixed. The `BREAK`@`0x18` happens because a real R3000A
   `syscall` in `INTRMANP` vectors to the still-unclaimed general
   exception handler (`0x80000080`), whose degenerate default content
   falls through low RAM to a `BREAK` placeholder at `0x18` - the
   same architectural gap as #124/#132/#148, one level deeper. Fix
   (`source/core/iop/iop_core.c`, BREAK case): when `Cause.ExcCode==8`
   (Syscall) is still set at the `BREAK`, return `$v0=0` to the caller
   (same "unimplemented call returns 0" precedent as `iop_hle_bios.c`)
   with proper RFE-equivalent Status-stack pop and `pc=EPC+4`, instead
   of halting. Any other `BREAK` (e.g. the test suite's universal
   clean-halt convention) is unaffected. Required updating
   `tests/test_iop_syscall.c` into two explicit single-step phases
   (its own SYSCALL+BREAK scenario became structurally identical to
   the real one now handled differently) - all 9 checks still pass.
   Measured real result (`diag83`): boot now progresses past
   `BREAK`@`0x18` to a new, different clean stop at `pc=0x800000AC`,
   `halt_reason="unimplemented SPECIAL funct 0x30 (pc=0x800000A8)"`
   (an unimplemented `TGE` trap instruction - an honest architecture
   limit, not a crash), reached via a SECOND real syscall/exception
   deeper in `INTRMANP`'s init. Full 84-block regression suite passes;
   clean Wii rebuild verified. The new `pc=0x800000AC`/`unimplemented
   SPECIAL funct 0x30` stop is not yet root-caused - that's the
   natural next step for whoever continues this thread.
   UPDATE (Round 29 continued, 30th change, task #150): root-caused
   as a real `TGE` (Trap if Greater or Equal) instruction and
   implemented (`source/core/iop/iop_core.c`, SPECIAL funct 0x30)
   with real MIPS trap semantics - Trap exception (ExcCode=13) if
   signed rs>=rt, pure no-op otherwise, same delivery mechanism as
   SYSCALL. Tested via `tests/test_iop_tge.c` (13 checks, both
   trap-taken and trap-not-taken paths). Full 85-block regression
   suite passes; clean Wii rebuild verified. HONEST FINDING: real-BIOS
   testing (`diag85`, 100M slices) shows this specific halt is gone,
   but the IOP does not progress further either - it settles into a
   tight, non-halting loop cycling through ~11 instructions in the
   `0x80000080`-`0x800000A8` range forever (a real syscall re-issued
   repeatedly, apparently not satisfied by task #149's stub 0 return
   value). Same class of finding as #124/#132's LOADCORE closure: an
   honest architectural stop, not pursued further this round. See
   STATUS.md's 30th finding for full detail. Open follow-up: task
   #151. UPDATE (Round 29 continued, 31st change, task #152): per
   user direction, attempted the higher-risk fix - traced the retry
   loop to a real ExitCriticalSection syscall re-entering LOADCORE's
   own registration-list walk (same mechanism as #124/#132), then
   refactored source/hw/iop_module_loader.c to front-load every boot-
   list module (parse+relocate+export-registration) before running
   any entry point, deferring import-stub linking to a second pass so
   imports can resolve regardless of list order (previously
   forward-only). HONEST RESULT: real-BIOS testing shows the IOP's
   behavior is byte-for-byte IDENTICAL to before this change - the
   retry loop is governed by LOADCORE's own internal
   boot_info[0x18]/[0x1C] bookkeeping, a separate mechanism from ELF
   import/export linking, so changing load order alone doesn't fix
   it. A follow-up attempt to source REAL (non-fabricated) function-
   pointer entries by reading them from already-loaded module memory
   also failed concretely: the four helper-subroutine addresses cited
   in the 27th finding, and even LOADCORE's own code region, read
   back as all-zero in IOP RAM by the time they'd be needed - the
   real content isn't resident at that point, regardless of load
   order. The front-loading refactor is kept anyway (real, independent
   improvement: forward-only import resolution was an artificial
   limitation, not a hardware constraint); full 85-block regression
   suite passes; clean Wii rebuild verified. Task #151 remains open -
   see STATUS.md's 31st finding for the complete investigation.
   UPDATE (Round 29 continued, 32nd change): implemented a second,
   structurally-similar safe bypass -
   `is_unconditional_trap_stub()` in `iop_module_loader.c` - for a
   NEW recursive dead end reached via a real syscall (INTRMANP's
   ExitCriticalSection) re-entering LOADCORE's real-installed
   exception-vector prologue, which ends in an unconditional TGE.
   Matches the exact real ten-instruction prologue by literal bytes
   (same approach as the LOADCORE panic-loop bypass) plus a
   STRUCTURAL check on the trap itself (SPECIAL/funct=0x30/rs==rt,
   not one hardcoded trap code - the same stub recurs nearby with a
   different code field). When recognized, advances to the next
   module exactly like the panic-loop bypass. MEASURED REAL RESULT
   (combined with the 31st change's front-loading): the boot sequence
   now loads and links **29/29** real IOPBTCONF modules (up from 4
   ever attempted), resolves 355/355 imports, runs 15 modules' entry
   points to full completion, safely bypasses 14 dead-end recursions,
   and reaches a clean, honest end-of-list halt instead of spinning
   forever. New test `tests/test_iop_trap_stub_bypass.c` (10 checks,
   including 3 negative controls). Full 86-block regression suite
   passes; clean Wii rebuild verified. See STATUS.md's 32nd finding
   for full detail, including the honest scope caveat that "15 run to
   completion" doesn't guarantee those modules did everything real
   hardware would - only that they executed for real until returning
   normally or hitting a recognized, safely-bypassed dead end.
   UPDATE (Round 29 continued, 33rd finding, task #153): identified
   which modules completed vs. bypassed - SIFMAN completed, but
   SIFCMD and SIFINIT (the higher-level SIF command/init layer the EE
   handshake actually depends on) both hit the same trap-stub dead
   end. Confirmed SIF registers (MSCOM/SMCOM/MSFLG/SMFLG) stay 0x0
   after the full boot sequence halts, and the EE remains stably
   parked in its known SIF-polling steady state even after 160M
   further EE instructions - a genuine stable end state, not
   undercounted patience. This is the same #124/#132 architectural
   gap, now attributed precisely to SIFCMD/SIFINIT as the two most
   productive next targets for the still-blocked entry-struct
   reverse-engineering work. See STATUS.md's 33rd finding for the
   full module-by-module breakdown.
   UPDATE (Round 29 continued, 34th finding, task #154): confirmed the
   connected live PCSX2 reference debugger (pcsx2-mcp, a real
   fully-booted instance, not this project's own emulator) can read
   real IOP memory via `pcsx2_disassemble(cpu="iop")` as a raw-word
   workaround (pcsx2_read_memory/pcsx2_evaluate cannot reach IOP
   space). Walked the real ModuleInfo_t chain from IOP address 0x800
   to confirm SIFMAN (entry=0x16930) and SIFCMD (entry=0x17e00) real
   addresses. Found the real cross-module import-table format (magic
   0x41e00000 + header + 8-byte library name + j-stub pairs) embedded
   in SIFMAN's own text - a different mechanism from LOADCORE's own
   internal registration list. Confirmed LOADCORE's real entry
   function reads boot_info at exactly the offsets this project's own
   struct already models (independent real-hardware confirmation of
   task #134's fix). Found a candidate real list-search function at
   0x1c70 matching the 27th finding's "phase-tagged list" description,
   not yet confirmed as the actual gate on task #151. No source
   changed - pure investigation. See STATUS.md's 34th finding.
   UPDATE (Round 29 continued, 35th finding, task #151/#154
   continued): fully reverse-engineered the real boot_info[0x18]/
   [0x1C] format from the live debugger. boot_info[0x18] = real word
   count minus one; boot_info[0x1C] = real pointer to a
   zero-terminated array of tag/pointer words (bit0=1 -> phase tag =
   word>>2; bit0=0 -> pointer to a real COFF (MIPSELMAGIC 0x162) or
   ELF-shaped module header, parsed by a real header-sniffer
   function). Confirmed the failure path lands in the exact same
   panic-sequence bytes this project's task #148
   `is_loadcore_panic_loop()` already recognizes - independent
   real-hardware validation of that signature. Task #151 is now
   unblocked in the sense that the real target format is fully known;
   implementing a real fix (building real COFF/ELF-shaped headers for
   this project's own loaded modules and pointing boot_info at them)
   is the natural next step, replacing the current safe bypass. Not
   yet implemented - investigation only. See STATUS.md's 35th
   finding.
   UPDATE (Round 29 continued, 36th finding, tasks #151/#155/#156/
   #157): implemented and real-BIOS-tested the registration list
   (task #155). Along the way, fixed an unrelated pre-existing hang
   in tests/test_iop_rfe.c (task #156: BREAK's syscall-fallback
   heuristic fired on a stale Cause value left over from an already-
   RFE'd exception; added an exception_pending flag to gate it
   correctly). Real-BIOS result: LOADCORE now genuinely walks the
   real 29-entry list (panic_loops_bypassed dropped to 0 - the
   original empty-list panic never fires now), but this exposed a
   NEW, distinct real dead end deeper in the walk (a second panic
   idiom, different call site); added is_registration_walk_panic_loop()
   (task #157) to bypass it and restore modules_run_to_completion to
   15. HONEST NET RESULT: the exact same 14 modules are bypassed as
   before, including SIFCMD and SIFINIT specifically - still hitting
   the identical dead end, unchanged. The real registration-list
   format is a genuine, kept improvement (real data, demonstrably
   changes LOADCORE's real code path) but does NOT resolve the actual
   SIF handshake blocker on its own. Full 87-block regression suite
   passes (0 hangs); clean Wii rebuild verified. Task #151 remains
   open - see STATUS.md's 36th finding for the full result and the
   UPDATE (Round 29 continued, 37th finding, task #151 continued):
   live-debugger tracing confirmed LOADCORE's registration-list walk
   is an ACTIVE, re-entrant call-dispatch mechanism - for each
   recognized real entry, it loads a function pointer from the
   parsed header's real `entry` field, sets `$gp` from the real `gp`
   field, and calls it directly via `jalr`. This project's own
   external module-sequencer (`advance_to_next_module()`) ALSO
   already runs every module's entry once, independently - so task
   #155's list (which includes every loaded module, including ones
   already run) likely causes LOADCORE to re-invoke an already-run
   module's entry a second time, which real kernel init code is not
   generally written to tolerate - a well-supported, NOT YET verified
   hypothesis for this round's new registration-walk panic. Two
   candidate directions for whoever continues: only list not-yet-run
   modules at the point LOADCORE's walk reaches this code, or let
   LOADCORE's own jalr-based walk become the real sequencer with this
   project's external loader stepping back after invoking it. No
   source changed - pure investigation. See STATUS.md's 37th finding.

   UPDATE (Round 29 continued, 38th finding, task #158): implemented
   and wired in mark_module_dispatched() per the 37th finding's
   hypothesis - patches a module's own registration-list slot from a
   real pointer to an inert tag word the instant it starts executing,
   preventing LOADCORE's real jalr-based walk from re-invoking an
   already-run module. Verified correct via: full 87-test regression
   suite (all pass), a temporary trace confirming all 29 modules'
   slots get patched during a real boot run, and a direct A/B
   comparison (git stash) against the pre-fix commit. HONEST RESULT:
   the real-BIOS diagnostic's output is byte-for-byte IDENTICAL before
   and after this fix - modules_run_to_completion=15,
   registration_walk_panics_bypassed=1 (same count, same place),
   SIFCMD/SIFINIT unchanged. The double-execution mechanism is real
   (confirmed via live debugger) but is NOT the actual SIF-handshake
   blocker - task #157's registration-walk panic fires identically
   regardless of slot content. Fix kept (architecturally correct, zero
   regressions) but does NOT close task #151. See STATUS.md's 38th
   finding for the full verification chain and the next concrete step
   (trace exactly what real condition triggers the panic pattern, not
   slot content).
   UPDATE (Round 29 continued, 39th finding, task #151/#159/#162): live
   real-game tracing plus a temporary reverted trace in our own
   emulator conclusively confirmed the registration-walk panic bypass
   fires on the exact same real LOADCORE dead-end code found live
   (byte-identical trap, just relocated). New detail: it's reached via
   a bounded 4-try RETRY LOOP scanning backward through an 8-byte-
   stride list (different from the 4-byte registration list this
   project builds) for a tag-matching entry, giving up after 4 failed
   attempts. Task #158's jalr-dispatch fix reconfirmed unrelated to
   this dead end. Next: trace what real structure is being scanned and
   why our boot never populates a match. See STATUS.md's 39th finding.
   UPDATE (Round 29 continued, 40th finding, task #151/#163): traced
   the full per-entry loop body end to end and confirmed the retry
   loop from the 39th finding is POST-WALK finalization code (runs only
   after the terminator word is read and the loop exits), gated by a
   flag at fp+0x48 and calling a subroutine (also called before the
   walk starts) immediately before the 4-try tag-3 scan begins. Third
   independent reconfirmation that task #158's jalr-dispatch theory is
   unrelated. Next: identify the scanned structure, the fp+0x48 flag,
   and what the shared subroutine does. See STATUS.md's 40th finding.
   UPDATE (Round 29 continued, 41st finding, task #151): reconciled
   this session's 39th/40th findings with the ORIGINAL task #151
   history (29th/30th/31st findings) - the 31st finding's stated
   blocker ("LOADCORE's code reads back all-zero") no longer applies;
   this session read fully valid real code there via both our emulator
   and the live debugger. Confirmed SIFCMD/SIFINIT are among the 13
   modules hitting is_unconditional_trap_stub() at the real R3000A
   exception vector 0x80000080 - the ORIGINAL retry-loop mechanism.
   Working hypothesis (not yet verified): the 8-byte-stride table this
   session traced is the same "internal registration list" that
   mechanism consults - one fix could resolve both bypasses. Next:
   trace forward from INTRMANP's ExitCriticalSection syscall to
   confirm. See STATUS.md's 41st finding.
   UPDATE (Round 29 continued, 42nd finding, task #151): sharpened the
   target considerably. The exception-vector "always trap" stub is
   baked in via ELF segment loading (a real early module's own data,
   not runtime-patched). EXCEPMAN (Exception_Manager) IS in this
   project's boot list and runs to FULL completion - yet never patches
   the vector, meaning the real design requires each module (SIFCMD,
   SIFINIT, etc.) to actively register its own handler via a real
   syscall/RPC into EXCEPMAN as part of its own init, which never
   succeeds since each module runs in isolation to completion. Next:
   trace EXCEPMAN's real internal structure and the real syscall a
   module like SIFCMD makes right before hitting the trap. See
   STATUS.md's 42nd finding.
   UPDATE (Round 29 continued, 43rd finding, task #151): found the
   exact real syscall numbers blocking SIFCMD/SIFINIT and 11 other
   modules - a genuine MIPS syscall instruction (Cause.ExcCode=8) with
   real syscall number 0x10 (9 modules) or 0x08 (4 modules), matching
   a RegisterLibraryEntries-style kernel call. This project's existing
   syscall HLE only covers the older A0/B0/C0 convention, not this
   real CPU-exception-based syscall - it falls through to the
   still-default exception vector and traps. Concrete next step
   (task #164): implement handling for these syscall numbers. See
   STATUS.md's 43rd finding.
   UPDATE (Round 29 continued, 44th finding, task #151/#164):
   IMPLEMENTED syscall 0x10/0x08/0x14 handling (direct intercept,
   return v0=0, matching established precedent). Real-BIOS result:
   modules_run_to_completion 15->19, trap_stubs_bypassed 13->0, and
   the IOP no longer halts/panics at all - it settles into a genuine
   polling loop (beq $zero,$s1,...) instead of any recognized panic
   pattern. Categorical improvement, but SIF_MSCOM/SIF_SMCOM/SIF_MSFLG/
   SIF_SMFLG remain completely unchanged - the polling loop's wait
   condition isn't yet satisfied, so the SIF handshake goal is not yet
   reached. Next: identify what the polling loop is waiting on. See
   STATUS.md's 44th finding.
   UPDATE (Round 29 continued, 45th finding, task #151/#165): task
   #165 SOLVED. Corrected a mis-decode from the 44th finding - the
   loop actually branches on $s0 (not $s1); $s0 is fed by a real
   SIF_MSFLG debounce-read at KSEG1 address 0xBD000020. ROOT CAUSE:
   sif.c's IOP-side mirror (sif_iop_mmio_read32/write32) checked the
   raw address against its 0x1D000000-0x1D0000FF window WITHOUT
   masking off KUSEG/KSEG0/KSEG1 segment-select bits first (unlike
   iop_mem_ptr's existing RAM path), so the real KSEG1 alias missed
   the window entirely and silently read back 0 instead of the real,
   already-correct SIF_MSFLG value. FIX: mask addr & 0x1FFFFFFF before
   the window check (two-function change). Verified: 87/87 regression
   tests pass, clean Wii rebuild, and real-BIOS result: IOP no longer
   stuck in the loop, modules_run_to_completion 19->28/29,
   SIF_SMCOM/SIF_SMFLG change for the first time all session
   (0->0x0011AFD0, 0->0x00010000). SIF_MSCOM stays 0 - not yet known
   if that's expected. Task #151 narrows further but stays open
   pending EE-side reaction trace. See STATUS.md's 45th finding.
   UPDATE (Round 29 continued, 46th/47th findings, task #170/#172):
   tracing the EE side's reaction to the 45th finding's fix uncovered a
   much bigger gap - the EE kernel's own SYSCALL handling had NEVER
   been implemented (always just halted), and boot now reaches it for
   real. Implemented real handling for every syscall observed on the
   boot path so far (numbers cross-referenced against ps2sdk's public
   syscallnr.h/sifdma.h): 100/60/61/120/18/22 as honest no-ops
   (cache-maintenance / unmodeled kernel bookkeeping / DMA-interrupt
   setup this project doesn't model), and 121/122 (sceSifSetReg/
   sceSifGetReg) implemented FOR REAL against this project's existing
   SIF register model - catching and fixing a genuine new bug along
   the way (a flat bypass for 122 caused ANOTHER infinite loop, since
   real code polls sceSifGetReg(SIF_REG_SMFLAG) directly for
   SIF_STAT_CMDINIT). Also added SIF_STAT_BOOTEND/CMDINIT signaling
   (real, documented sifdma.h bits) to the IOP module loader's existing
   "boot complete" halt sites. Verified: 87/87 regression tests, clean
   Wii rebuild. Real-BIOS result: boot now runs the entire real
   sceSifInitCmd() sequence correctly and reaches syscall 119
   (sceSifSetDma, a real DMA packet transfer) - furthest point ever
   reached, though GS/display registers are still untouched (expected -
   this is early kernel bring-up, before any drawing). See STATUS.md's
   46th/47th findings.
   UPDATE (Round 29 continued, 48th finding, task #172): implemented
   syscall 119 (sceSifSetDma) for real - a genuine EE-RAM-to-IOP-RAM
   DMA copy, confirmed matching real ps2sdk's _SifSendCmd() sending
   its SIF_CMD_INIT_CMD packet. Caught and fixed a real regression
   along the way: the first version gave ee_core.c a hard link-time
   dependency on iop_core.c, breaking ~37 EE-only tests - fixed with
   an optional function-pointer bridge wired up in system_init(),
   left NULL (safe no-op) for EE-only tests. Verified: 87/87
   regression tests, clean Wii rebuild. Real-BIOS result: boot
   advances into a further real code path resembling sceSifInitCmd()'s
   "already initialized" guard - furthest point yet reached. See
   STATUS.md's 48th finding.
   UPDATE (Round 29 continued, 49th finding, task #171): parallel GS
   audit (run alongside the syscall trace, not sequentially) found and
   fixed a real, independent bug: ee_mem_read64()/write64() (the only
   path GS privileged registers PMODE/DISPFB/DISPLAY are reachable
   through, since they're 64-bit-only) were missing the same KSEG0/1
   mirror-masking the 32-bit hardware path already has - a real SD to
   DISPFB1 through 0xB2000070 would have silently missed gs_mmio_
   write64() even once boot reaches BIOS code that writes it. Fixed +
   added a regression test case. Verified: 87/87 tests, clean Wii
   rebuild. Confirmed main.c's real-boot-flow GS-to-framebuffer
   scaffolding (task #128) is genuine and already wired up, just never
   yet triggered (pmode still zero - boot hasn't reached logo/OSD code
   yet). See STATUS.md's 49th finding.
   next concrete target (the new, deeper dead end this round found).

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
   opcodes) added Round 29 continued's 21st change; VLQI/VLQD/VSQD
   (the pre/post increment/decrement memory-access siblings of
   VSQI, plus a VSQI destmask bugfix) added Round 29 continued's
   22nd change; VMTIR/VMFIR/VILWR added Round 29 continued's 23rd
   change; VRNEXT/VRGET/VRINIT/VRXOR (the R-register LCG) added
   Round 29 continued's 24th change; VDIV/VSQRT/VRSQRT/VWAITQ (the
   Q-register-producing division/sqrt family) added Round 29
   continued's 25th change - researching VWAITQ confirmed real PCSX2
   itself has an empty _vuWAITQ body, so no Q "busy timing" model was
   needed after all; VCLIPw, the last remaining gap, added Round 29
   continued's 26th change - resolved by reusing control-register slot
   18 (REG_CLIP_FLAG) via the existing generic CFC2/MTC2/QMTC2 paths,
   so no new field was needed after all) are wired up and tested.
   CLOSED: every VU0 macro-mode instruction identified this session
   (the full SPECIAL1 arithmetic/broadcast rows and the full SPECIAL2
   128-entry table) is now implemented and tested. UPDATE (Round 29
   continued, 28th change): the boot trace now progresses well past
   "still steady-state SIF-polling" (see item 1's IOP module-
   sequencing update below) - VU0 macro mode's real-world use may
   become reachable sooner than previously estimated as the boot
   trace advances further.

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

6. Lower priority, deferred: Pad/memory card (section 7). (EE MMI
   opcode coverage, previously listed here as "~23 remaining," was
   completed in Round 64 - see section 1 and the 97th finding.)

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

**UPDATE (Round 29 continued, 50th finding, task #176):** implemented
real EE external-interrupt delivery, which this project had entirely
lacked - INTC_STAT/MASK (new `source/hw/ee_intc.c`) and DMAC_STAT
completion+enable-mask bits (`dma.c` extended), wired into
`ee_core.c`'s Cause.IP2/IP3 (real bit positions 0x400/0x800, cited
from PCSX2's Hw.cpp/HwWrite.cpp), mirroring the existing Cause.IP7
timer-interrupt pattern. EE syscalls 22 (`_EnableDmac`) and 119
(`sceSifSetDma`) now interact with this real register model instead
of being no-ops/partial-ops. Result: the EE no longer spins forever in
the `~0x84330` "already initialized"-guard poll loop this project had
been stuck on since the 48th finding - a real Cause.IP3 interrupt now
fires and vectors into the kernel's own real interrupt-dispatch code
for the first time ever, running further than any previous session
before hitting a NEW, undiagnosed halt at EE PC `0x80001390`
("unimplemented SPECIAL funct", possibly a data table being
misexecuted as code - not yet root-caused). 87/87 regression tests
pass; clean Wii/devkitPPC rebuild. See STATUS.md's 50th finding for
full detail and next-step guidance.

**UPDATE (Round 29 continued, 51st finding, task #177):** implemented
EE MFSA/MTSA (SPECIAL funct 0x28/0x29, real R5900-specific
instructions per ps2tek's SPECIAL table - reserved in standard MIPS
III), the exact instruction the 50th finding's new interrupt-handler
code halted on. New `sa_reg` field on `ee_state_t`; new dedicated
8-check regression test (`tests/test_ee_sa_reg.c`); 87/87 suite pass;
clean Wii rebuild. Boot now runs the complete real interrupt-handler
prologue (SQ-saving $s5-$s8/$t8/$t9/$gp, MFHI/MFLO/MFHI1/MFLO1/MFSA)
and reaches a NEW halt: a real, intentional `BREAK` instruction
physically present in the BIOS image at EE PC `0x80000DC0` (code field
`0xFFFFF`). Whether this is expected real-hardware behavior or a
symptom of task #176's admittedly-incomplete SIF DMA completion
signaling (EE side only, no genuine IOP-side command processing) is
the next open question - see STATUS.md's 51st finding.


**UPDATE (Round 29 continued, 52nd finding, task #178):** implemented
real EE BREAK exception delivery (ExcCode 9, Breakpoint) - real R5900
hardware never halts unconditionally on BREAK, it raises a genuine
exception and vectors through the normal handler path, same as any
other trap. This was a bigger change than it looked: nearly all of
this project's ~35 EE host-native unit tests relied on "BREAK halts
the core" as a run-to-completion convenience convention dating back
to before real MIPS exception delivery existed; fixed via a mix of
bounded ee_core_step() counts (where genuinely useful) and a
mechanical `run_until_break()` test-harness compatibility shim
(unaffected: production system.c/main.c, which correctly benefit from
BREAK no longer halting mid-boot). 87/87 suite pass; clean Wii
rebuild. Host-native diagnostic against the real BIOS confirms this
was the actual unlock: boot no longer stops at `0x80000DC0` - the
kernel's own installed handler silently resumes past it (Status.EXL
back to 0, consistent with a clean ERET), and the EE keeps running
correctly for 65,000,000+ further instructions, settling into a NEW,
distinct wait/scan loop around EE PC `0x8000F768` (real, bounded,
touches the DMAC_STAT KSEG1 mirror - not a crash). See STATUS.md's
52nd finding for full detail; this new loop is the next thing to
root-cause for task #172.


**UPDATE (Round 29 continued, 53rd finding, task #179):** audited the
EE syscall table (0-172 requested scope) against a real 65M-instruction
boot trace - no gap found, only the 9 already-implemented numbers are
ever invoked. Implemented real VBLANK_START/VBLANK_END interrupt
delivery (INTC bits 2/3, `ee_intc_raise()` - previously declared but
never called by anything in this project), the first genuinely new
"registration" this round adds; verified it fires and vectors
correctly, and kept as real, independently-correct hardware behavior.
However, direct disassembly of the real BIOS's `0x8000CF88` polling
subroutine (called from the `0x8000F768` wait loop found in the 52nd
finding) shows the loop's real exit condition is NOT COP0-interrupt-
based at all - it's a plain memory-mapped poll of DMAC_STAT bit 0x80
(SIF2 completion) or INTC_STAT bit 0x2 (SBUS), neither of which this
project's IOP can ever raise because the IOP halts by design once its
own module-loading sequence completes (`i=29,937,994`, well before the
EE even reaches this loop), instead of continuing into a persistent
idle/scheduler loop the way real IOP hardware does. 87/87 regression
suite pass; clean Wii rebuild. This reframes task #172's next step:
the real fix is making the IOP core stay "alive" (idle loop or minimal
scheduler) after module loading finishes, not a further EE-side
registration/interrupt addition - see STATUS.md's 53rd finding for
full detail. Flagged to the user before implementing, given the scope
of an IOP-core-lifecycle change.


**UPDATE (Round 29 continued, 54th finding, task #172):** implemented
the user-approved fix direction from the 53rd finding - IOP core no
longer halts after module loading, instead entering a real, honest
`idle` state (interrupt-responsive, no fabricated instruction content
executed) matching real IOP hardware's actual never-halts behavior.
Verified via diagnostic and 87/87 regression; clean Wii rebuild. This
change is real and being kept, but empirically does NOT unblock the
`0x8000F768` loop by itself: this project's IOP has no persistent
driver-thread model, so an idling-but-not-halted IOP still raises
nothing new on its own. Deeper instrumentation found the loop's true
remaining blocker is EE-side: EE code never writes to its own D7
(SIF2) DMA channel control register across the full 65M-instruction
run, and since this project's DMA kicks are synchronous/CPU-
independent (matching real DMA hardware, cross-checked against
PCSX2's `Hw.cpp`), an alive IOP was never going to be sufficient on
its own. Narrows task #172's next step to an EE-side question. See
STATUS.md's 54th finding for full detail.

**UPDATE (Round 30, 55th finding, task #172/#180):** used PCSX2's own
live reference debugger (via `pcsx2-mcp`) against a real, legally-
dumped Gran Turismo 3 to empirically test the 54th finding's open
SIF2/SBUS-kick question - conclusively ruled out on real hardware too
(DMAC_STAT/INTC_STAT/D7-CHCR are never touched even in real gameplay,
and neither `0x8000F768`'s wait loop nor the `0x80001884` BREAK-trap
call site is ever reached on real hardware). Tracing this project's
own emulator's path into that same BREAK-trap fallback found it isn't
a "bounded retry, give up" construct at all (a mis-pattern-match to
tasks #124/#132/#148/#159, self-corrected) - it's a real kernel
printf()-style debug-console routine (SIO putc -> CRLF putchar ->
format-string parser, all genuine BIOS code), and extracting the
actual format strings recovered the real boot log, ending in
`"# DMAC(%d) Handler does not exist.."` for channel 5 (SIF0). Root
cause: EE syscall 18 (`AddDmacHandler`) was bypassed with a hardcoded
`return 0` instead of vectoring as a real Syscall exception, so the
real kernel-owned DMAC-handler table this message checks was never
populated by AddDmacHandler's own real handler code. Fixed: syscall 18
now raises a genuine `EE_EXC_CODE_SYS` exception (same general-vector
mechanism task #178 proved out for BREAK) instead of being intercepted
in software - letting real BIOS code build its own table. Verified:
the "does not exist" message and the BREAK-trap fallback are both
completely gone post-fix; the emulator now reaches a new, deeper halt
at EE PC `0x00081FF4` (`$v1=-5`, no matching documented syscall,
outside the `0x8000xxxx` kernel range explored so far) - flagged as
the concrete next blocker rather than guessed at. 87/87 regression
pass; clean Wii/devkitPPC rebuild. See STATUS.md's 55th finding for
full detail.

**UPDATE (Round 31, 56th finding, task #172/#181):** the deeper halt
from Round 30 (EE PC `0x00081FF4`, `$v1=-5`) turned out to be a real,
intentional `addiu $v1,zero,-5` immediately before `syscall`, not a
corrupted register - disassembly shows the classic shape of a kernel
interrupt-dispatch trampoline (indirect `jalr` through a handler
pointer, then this syscall on return). Cross-referencing the full raw
ps2sdk `syscallnr.h` source confirms positive syscall 5 is
`ResumeIntrDispatch // Arbitrarily named` (ps2sdk's own maintainers
flag it as inferred/undocumented), and the dual positive/negative
"fast" syscall convention is real and established elsewhere in that
same header - just not named for number 5, consistent with a
kernel-internal-only call. Fixed the same way as task #180's
AddDmacHandler: let `-5` vector as a real Syscall exception instead of
being bypassed or halted, so genuine BIOS handler code runs. Result:
the EE core no longer halts at all within a 100M-instruction budget -
real, continued forward progress through several genuinely new code
regions, settling into a steady-state loop around `0x00083B40`
(confirmed to be an ordinary array-accessor function, not a spin-loop
itself - the actual iterating caller is not yet identified). 87/87
regression pass; clean Wii/devkitPPC rebuild. See STATUS.md's 56th
finding for full detail.

**UPDATE (Round 32, 57th finding, task #172/#182):** investigated the
Round 31 steady-state loop (`0x00083B40`) and found two things. First,
a major positive milestone: this project's boot now performs a real,
complete GS CRTC/video-timing configuration sequence for the first
time ever (`GS_CSR`/`SMODE1`/`SYNCH1`/`SYNCH2`/`SYNCV`/`SMODE2`/
`SRFSH`, all real registers, real addresses, correct order) - a
genuine prerequisite for a splash screen. Second, precisely identified
the next blocker: the `0x00083B40` loop polls a fixed address
(`0x0008C440`) that is zeroed once at BSS-init time and never written
again - confirmed via a dedicated whole-boot memory watch, a
2-billion-instruction run that never resolved it, and frozen register
state across 1.4M+ loop iterations (ruling out "real hardware busy-
wait, just slow in emulation"). One candidate mechanism (the 4-call
device-registration sequence found in Round 31) was investigated and
ruled out - it writes a different, nearby table. The real mechanism
that should set this flag is not yet identified; flagged for live
PCSX2 debugging (the same technique that resolved Round 30) as the
next step, rather than guessed at. No source changed this round -
pure diagnostic investigation. See STATUS.md's 57th finding for full
detail.

**UPDATE (Round 33, 58th finding, task #172/#183):** live PCSX2
debugging against real GT3 confirmed real hardware DOES write
`0x0008C440` to 1 (this project's own emulator never does, past BSS
init). Live write-timing capture proved fundamentally impractical over
this tool bridge (the emulator runs in real wall-clock time between
calls, not lockstep with tool call cadence - confirmed by cycles
jumping from tens of millions to over 2 billion in a single
continue/pause round trip regardless of retry count), so pivoted to
static ROM analysis: searched the entire real BIOS for every
occurrence of the exact address-computation instruction pattern
(`lui reg,9; addiu reg,reg,-0x3bc0`) used by every known reader/writer
of this address so far. Found 4 matches - the known read site, a
near-duplicate read site, one unrelated stub, and a 32-word zero-fill
loop confirming `0x0008C440` is entry 0 of a real 32-entry table - but
NONE of them is a nonzero write. This rules out ordinary EE CPU code
using this literal address pattern as the write mechanism, pointing
instead toward an indirect write (most likely a SIF DMA transfer from
the IOP side, matching this project's established SIF/IOP-EE
communication gaps). No source changed this round. See STATUS.md's
58th finding for full detail.

## UPDATE (Round 34, task #172/#184/#185): IOP never reaches real SIF0/SIF1 DMA register writes during boot; SIFCMD's own init code found to retry an undocumented ~438,000-iteration internal poll loop before giving up on its own

Traced whether the IOP, even with the existing task #164 syscall
0x10/0x08/0x14 bypass in place, ever reaches real code that writes to
the real SIF0/SIF1 DMA channel registers (0x1F801520-0x1F80153F) at
any point during boot. Host-native diagnostic across a full
60-million-instruction boot (all 28 real IOPBTCONF modules, LOADCORE
through EESYNC) found **zero** such writes anywhere - module init
completes successfully and the IOP goes idle exactly as already
understood (54th finding), but no module's code ever touches a SIF
DMA channel register.

Along the way, found that SIFCMD's own module init code (not any of
the module loader's own bypass/panic logic) calls the same two
bypassed syscalls with identical arguments roughly 438,000 times in a
row before proceeding on its own - a real, previously-undocumented
retry/poll loop whose exit condition our `$v0=0` bypass never
satisfies, but which SIFCMD's own code eventually gives up on anyway.
Considered un-bypassing these syscalls (mirroring the twice-successful
EE syscall-18/-5 fix pattern), but ruled it out: this project's own
existing comments already establish there is no real, resident kernel
dispatcher for these calls to vector into yet, so removing the bypass
would only reintroduce the original task #151 module-abandonment bug,
not fix anything.

Conclusion: the real 0x0008C440 write (confirmed present on real
hardware - 58th finding) is not reachable via any boot-time IOP module
code path this project currently models. The next real target is
IOP-side **post-boot runtime** behavior (a real SIF RPC service loop
or interrupt-driven mechanism), which is presently unmodeled - a
genuine feature gap, not a bug fix. No source changed this round
(three throwaway `/tmp` scratch copies, diff-verified against the real
files). See STATUS.md's 59th finding for full detail.

## UPDATE (Round 35, task #172/#186): precisely traced the real interrupt-driven SIF0 kick; found two concrete, distinct gaps blocking any real SIF RPC service - no DMA channel has a registered sink, and neither side ever kicks SIF1's return transfer

Traced the EE side of the SIF0/SIF1 picture to complement the 59th
finding's IOP-side trace. Confirmed the existing Cause.IP3 (DMAC)
interrupt delivery (task #176) still works correctly this far into
boot: the EE's real SIF0 kick fires a genuine DMAC completion
interrupt, which real kernel code handles by calling EE syscall 119
(`sceSifSetDma`) or its "fast" -119 variant via simple syscall
trampolines - twice, then never again (a drained queue, not a bug).

Found two concrete, previously-undocumented gaps: (1) no DMA channel
in this project has ever had a real sink function registered
(`dma_register_sink()` is dead code - called from nowhere), so even a
well-formed real transfer would silently drop its payload; (2) the EE
prepares a real SIF1 receive chain (TADR set to a real address) but
never kicks it, and per the 59th finding, the IOP never touches its
own SIF0/SIF1 channel registers either - so nothing ever drives the
return transfer.

Fetched real citations for the next round: ps2sdk's actual
`SifCmdHeader_t` struct and `SIF_CMD_*`/`SIF_SREG_RPCINIT` constants
(common/include/sifcmd-common.h), and ps2tek's DMAC interrupt
semantics (already correctly implemented here). Scoped task #186's
first concrete increment as wiring real SIF0/SIF1 sinks - smaller and
better-grounded than attempting the full command-dispatch protocol at
once. No source changed this round. See STATUS.md's 60th finding.

## UPDATE (Round 36, task #172/#186): fetched the real ps2sdk sceSifInitCmd()/_SifSendCmd() source and confirmed byte-for-byte that this project's boot sends genuine SIF_CMD_INIT_CMD packets; real IOP-side assembly needed to complete the fix could not be fetched this round

Building on Round 35's sink-wiring plan, fetched the real
`ee/kernel/src/sifcmd.c` from ps2dev/ps2sdk and matched it field-by-
field against this project's own diagnostic trace: confirmed the two
captured packets are genuine `SIF_CMD_INIT_CMD` (cid=0x80000002) sends
from the real, unmodified `sceSifInitCmd()` routine, and that real
cross-CPU data movement for this exact path already works correctly
via the existing `sceSifSetDma` syscall HLE. This corrected course
before writing unneeded code - an IOP-side hardware DMA execution
engine (Round 35's plan) would not have fixed this specific blocker,
since real BIOS code uses the software syscall path here, not a
hardware CHCR kick.

The real missing piece is a genuine IOP-side consumer for this packet
- something needs to notice it arrived and act on it, which never
happens because the IOP has already gone idle by this point (per
Round 34's finding). Could not fetch the real IOP-side SIFCMD
assembly source this round (ps2sdk's IOP SIFCMD is .s, not .c, and all
fetch attempts for it returned empty), nor cross-check against the
live PCSX2 reference instance (its IOP RAM no longer has this module
resident at the expected address, ~2 billion cycles into gameplay).
No source changed this round. See STATUS.md's 61st finding for full
detail and the explicit fork in next steps (further source-fetching
vs. a labeled, protocol-symmetry-based minimal implementation).

## UPDATE (Round 37, task #172/#186): minimal IOP-side SIF_CMD_INIT_CMD consumer implemented and verified - no regression, boot progress unchanged

Per user direction ("proceed with what I have now" after exhausting
real IOP-assembly fetch attempts), implemented `sif_cmd_iop_handle_init_cmd()`
in `sif.c`/`sif.h`: records the EE's reply-buffer address on receipt of
`SIF_CMD_INIT_CMD`, by direct symmetry with the real, byte-exact EE-side
`SIF_CMD_CHANGE_SADDR` handler (61st finding). Explicitly labeled as
protocol-grounded, not a byte-exact IOP assembly port. Full 88-test
regression suite passes; clean Wii rebuild. Real-BIOS diagnostic
confirms it fires correctly (records `0x0008C240`, the real EE pktbuf
address) with zero regression - boot reaches the identical furthest
point (`0x00083B40`) as before. Does NOT by itself unblock the
`0x0008C440` poll; that remains open, likely gated by the
AddDmacHandler 32-entry table instead. See docs/STATUS.md's 62nd
finding for full detail, including a diagnostic-tooling quirk found
and root-caused along the way (out-of-band `ee_mem_read32()` calls
before boot execution begins can corrupt CPU state - not a shipped
bug, just a harness hazard, documented so it isn't rediscovered).

## UPDATE (Round 38, task #172/#187): MAJOR BREAKTHROUGH - 0x0008C440/SIF_SREG_RPCINIT poll resolved by real BIOS code; boot reaches CreateSema (syscall 64), a brand-new stage

Fetched the full real ee/kernel/src/sifcmd.c, confirming the 32-entry
table blocking boot since the 57th finding is real ps2sdk's
`sregs[32]`, and index 0 is `SIF_SREG_RPCINIT`. Implemented a
byte-exact synthetic SIF_CMD_SET_SREG(RPCINIT,1) delivery (triggering
the REAL, already-resident _SifCmdIntHandler()/set_sreg() dispatch -
not a direct poke), plus fixed a real gap (isceSifSetDChain's negative
"fast" form wasn't bypassed, only its positive counterpart). Full
88-test regression passes; clean Wii rebuild. Real-BIOS diagnostic
confirms 0x0008C440 now genuinely becomes 1 via real BIOS code, and
boot advances to a brand-new syscall (CreateSema, 64) never reached
before. See docs/STATUS.md's 63rd finding for full detail, honest
caveats (synthetic delivery TIMING is an approximation; everything
else is byte-exact and live-disassembly-confirmed), and the new open
task (implement or scope around CreateSema).

## UPDATE (Round 39, task #172/#188): CreateSema implemented for real (user: "implement it"); boot reaches WaitSema (syscall 68), a new wall

Implemented a real (not bypass) `CreateSema` (syscall 64/0x40): a
256-slot EE semaphore table (`g_ee_sema[]`), first-fit allocation, real
`ee_sema_t` struct fields read from the caller (`max_count`/
`init_count`/`attr`/`option`), returning a real slot-index ID. Grounded
in `ee/kernel/include/kernel.h`, cross-checked against a full local
ps2sdk source tree the user supplied (`ps2sdk-master.zip`). Full
88-build/87-distinct-binary regression suite passes (0 failures);
clean Wii/devkitPPC rebuild. Re-hit and re-resolved the known
"diagnostic tooling hazard" from the 62nd finding (a stale harness
copy briefly produced a false "regression" signal; rebuilding without
the premature out-of-band memory read confirmed zero actual
regression - task #187's RPCINIT fix remains fully intact).

Real-BIOS empirical result: `CreateSema` is called with `max_count=1`,
`init_count=0` (a locked binary semaphore/mutex idiom), succeeds
(id=0), and boot immediately calls `WaitSema` on it (syscall 68/0x44) -
a brand-new wall, honestly reported, not implemented yet. Confirmed
via GitHub directory listing that ps2sdk ships no `WaitSema.c` (these
are pure kernel syscalls, real BIOS-ROM-resident, like `CreateSema`) -
real blocking semantics (decrement-if-positive, else block until
signaled) are well-documented at the protocol level (psdevwiki, ps2sdk
`thread.c` usage pattern) but this project has no real multi-thread
scheduler yet, so implementing this needs careful characterization
first. New task #189 opened for this. See docs/STATUS.md's 64th
finding for full detail, including the live-PCSX2-disassembly-traced
caller chain (an apparent `AddIntcHandler`-family call reached shortly
after `CreateSema` returns, not yet fully characterized).

## UPDATE (Round 40, task #172/#189): traced WaitSema's real caller - found a likely sceSifSetDma return-convention conflict, NOT fixed (docs-only investigation round)

Traced the real caller chain past CreateSema via live PCSX2
disassembly: a SIF0 interrupt/handler-registration function
(0x00084870-0x00084940 -> 0x00083FD0/0x00084010 -> 0x00083E90) that
finishes by calling this project's own already-implemented
sceSifSetDma (syscall 119). Found a likely return-convention conflict:
this caller treats a ZERO return as success (skipping WaitSema
entirely) but this project's syscall 119 handler always returns a
nonzero value (`count ? count : 1u`), so this caller always falls into
its "error" path, calling WaitSema (the observed wall) then DeleteSema.
Deliberately did NOT change the syscall 119 return convention this
round - it's load-bearing for the already-verified SIF_CMD_INIT_CMD/
RPCINIT boot progress (tasks #186/#187), and changing it without being
certain of the real convention risks a silent regression. No source
changed; docs-only investigation round. See docs/STATUS.md's 65th
finding for full detail and the precise next step.

## UPDATE (Round 41, task #172/#189/#190): real WaitSema/SignalSema/iSignalSema/DeleteSema implemented; corrected the 65th finding's misread; boot safely parks (no halt) with an honest new open lead (task #191)

Corrected a mistake from Round 40 (65th finding): the "sceSifSetDma
return-convention conflict" was a misread MIPS branch-delay slot -
delay-slot instructions execute unconditionally, taken or not.
Re-reading the real caller correctly shows WaitSema is the genuine,
intended control flow (create a locked semaphore, kick off an async
SIF op, wait for completion, clean up), not an error branch - nothing
about this project's existing sceSifSetDma needed to change (and
confirmed against the user's ps2sdk-master.zip: real ee/kernel/src/
sifrpc.c's own `while (!sceSifSetDma(&dmat, 1))` idiom matches this
project's zero=fail/nonzero=success convention exactly).

Implemented real WaitSema (68), SignalSema (66), iSignalSema (-67),
DeleteSema (65). WaitSema blocks by "parking" (not advancing pc past
the syscall) while all existing per-step interrupt checks keep running
normally, so a real interrupt can still vector away, run genuine BIOS
handler code, and retry the syscall on return - reusing the same real
exception-delivery machinery already built for timer/vblank/DMAC
interrupts, not a new synthetic mechanism.

Full 88-build/87-distinct-binary regression suite passes; clean
Wii/devkitPPC rebuild. Real-BIOS diagnostic over 300,000,000
instructions past the WaitSema wall: the EE never halts, stays
correctly parked at the WaitSema syscall PC, and 0x0008C440 stays at
its correct value 1 (no regression) - but boot does not progress
further, since nothing in this project's currently-modeled interrupt
chain calls SignalSema on this semaphore yet. Honestly reported, not
disguised. New task #191 opened: the real function traced in the 64th
finding writes a new, raw (non-syscall-registered) handler-table entry
this project's interrupt dispatch doesn't yet know how to invoke - the
leading hypothesis for what's still missing. See docs/STATUS.md's
correction + 66th finding for full detail.

## UPDATE (Round 42, task #172/#191): confirmed the real caller past CreateSema is byte-exact ps2sdk _SifSendCmd(); the blocker is now precisely an undocumented "system command 9" (docs-only investigation round)

Re-disassembled the caller chain past CreateSema against the real,
fetched ee/kernel/src/sifcmd.c and confirmed a byte-exact match to
_SifSendCmd(cid, mode, pkt, pktsize, src, dest, size): the stack writes
previously described as a mysterious "handler table" are in fact a
real SifDmaTransfer_t {src,dest,size,attr} construction (attr=0x44=
SIF_DMA_ERT|SIF_DMA_INT_O, dest=_sif_cmd_data.iopbuf read from
0x0008C320, matching the 63rd finding's struct layout exactly), and
the "0x84168" calls are real sceSifWriteBackDCache(), not a generic
cache utility as previously guessed.

The one remaining unknown: cid=0x80000009 (SIF_CMD_ID_SYSTEM|9) is not
part of public ps2sdk's sceSifInitCmd() sequence (which only sends
cid 0/2) - it's a real, later kernel subsystem's own internal SIF
command with no available source describing its semantics or expected
IOP-side response. Deliberately did NOT fabricate a synthetic response
(unlike task #187's RPCINIT delivery, which was grounded in a fully
documented struct/field) since there is no real source to ground one
here. No source changed this round; docs-only. See docs/STATUS.md's
67th finding for full detail and the precise open question.

## UPDATE (Round 43, task #192/#193): "system command 9" identified for real (SIF_CMD_RPC_BIND); WaitSema-park epilogue-starvation root cause found and FIXED - real forward progress past the WaitSema wall

User-provided ps2tek URL (following up on two earlier user-provided
research links) identified cid=0x80000009 as the real, documented
SIF_CMD_RPC_BIND - not an undocumented mystery. Cross-confirmed
byte-exact against the user-uploaded ps2sdk-master.zip's real
sceSifBindRpc()/_request_end() source. Implemented a synthetic
SIF_CMD_RPC_END (REND) reply delivery mechanism symmetric to the
already-proven RPCINIT delivery (task #187) - initially did not fire.

Root-caused via extended host-native diagnostic tracing: EVERY syscall
handler in the EE's syscall dispatch chain (`if (sysnum == N) { ...
return 1; }`) exits ee_step() before reaching its own shared per-step
epilogue (Count increment, VBLANK, RPCINIT/RPC-bind pending checks,
and the timer/INTC/DMAC interrupt checks). Harmless for one-shot
syscalls, but WaitSema's park branch re-executes the SAME instruction
every step while blocked, so the epilogue - and all real-interrupt
delivery it drives - was being silently starved for as long as any
WaitSema park lasted. This silently invalidated part of the 66th
finding's claim that interrupt checks "keep running normally" while
parked (never actually exercised by a real external signal until this
round). Fixed by explicitly running the same interrupt/pending checks
inside WaitSema's park branch itself (source/core/ee/ee_core.c),
leaving the shared epilogue itself untouched.

Verified: all 87 host-native regression tests pass; a host-native
diagnostic against the real fixed source shows the synthetic REND
delivery firing, the real, already-resident _request_end() BIOS code
calling iSignalSema on the correct semaphore, WaitSema genuinely
unparking, and the boot executing several million more real
instructions (DeleteSema, return from sceSifBindRpc(), and beyond)
before hitting a NEW WaitSema wall on a different semaphore (semid=1) -
real, substantial, honestly-verified progress past tasks #188-#192's
wall, not a synthetic shortcut. See docs/STATUS.md's 68th/69th findings
for full detail. New open item: identify what semid=1's real blocking
condition corresponds to (task #172 continues).

## UPDATE (Round 44, task #194): generalized RPC_BIND reply to every bind; fixed a real infinite re-bind loop (NULL cd->server); boot reaches a genuine sceSifCallRpc() - next wall identified

Diagnostic tracing showed the semid=1 wall from Round 43 was simply a
second sceSifBindRpc() call to the same real service (sid=0x80000006,
LOADFILE). Removed the "first bind only" gate on the synthetic REND
delivery so every Bind gets answered. This surfaced a deeper bug: the
reply's `sd` field (SifRpcServerData_t*) was left NULL, an already-
documented gap - but the diagnostic proved it causes a REAL infinite
loop, since real ps2sdk-based callers poll `cd->server == NULL =>
retry bind` while waiting for a target IOP module to register. Fixed
by writing a clearly-labeled non-NULL placeholder (0x00001000, NOT a
real modeled IOP address) into `sd`, satisfying only the real `!=
NULL` check the caller performs.

Verified: all 87 regression tests pass; diagnostic confirms the
re-bind loop is gone and boot proceeds to a genuine, different
CreateSema call followed by a real `SIF_CMD_RPC_CALL` (cid=0x8000000A,
size=64 matching SifRpcCallPkt_t) - an actual sceSifCallRpc() against
the now-bound LOADFILE service, with a payload that appears to
reference a "rom0:"-style path (consistent with loading a real boot
module, possibly the boot logo). Clean Wii/devkitPPC rebuild verified.
See docs/STATUS.md's 70th finding for full detail. New task #195:
trace the real SifRpcCallPkt_t protocol and what a genuine LOADFILE
response needs to contain.

## UPDATE (Round 45, task #195/#196): fixed RPC_CALL descriptor-order bug; implemented real ELF32 loading of "rom0:OSDSYS"; implemented syscalls 23/19/7 (_DisableDmac/RemoveDmacHandler/_ExecPS2) - boot genuinely executes real jump into OSDSYS, re-enters a previously-solved wait loop as new open item

Traced the real SifRpcCallPkt_t/sceSifCallRpc() protocol against the
user-uploaded ps2sdk-master.zip. Found and fixed a descriptor-order
bug: the RPC_CALL payload descriptor (the raw request struct
containing the target path) is built BEFORE the header descriptor by
real _SifSendCmd() (dmat[0]=payload, then dmat[1]=header), the
opposite of what this project's synthetic handler assumed.

Implemented a real ELF32 loader for "rom0:OSDSYS" straight out of the
already-loaded BIOS ROM image: a ROMDIR lookup finds the real
582,704-byte "OSDSYS" entry, its PT_LOAD segment's real bytes are
copied into EE RAM at the real p_vaddr (BSS zero-filled), and the real
e_entry/gp are returned - grounded in the real
iop/system/loadfile/src/eeelfloader.c behavior (including the
non-obvious detail that gp is always hardcoded 0 for full-ELF loads,
not computed). Independently verified against a direct Python ROMDIR/
ELF scan of the real BIOS: e_entry=0x00200008, matches exactly.

Implemented three new real EE syscalls, each confirmed via byte-exact
register-state matches at each new wall: 23 (_DisableDmac, mirror of
already-implemented 22), 19 (RemoveDmacHandler, mirror of 18, vectored
as a real exception per the task #180 lesson), and 7 (_ExecPS2 - the
actual jump-to-loaded-program mechanism; $a0 matched OSDSYS's real
e_entry byte-exact). Real ps2sdk ships no C source for _ExecPS2
itself (confirmed bare SYSCALL() trampoline in kernel.S) so it too is
vectored as a real exception into resident BIOS code, consistent with
this project's established syscall-7/18/19 pattern.

Verified: all 87 regression tests pass; diagnostic confirms the full
chain fires for real (RPC_CALL -> ROMDIR lookup -> ELF load -> WaitSema
unpark -> _DisableDmac/RemoveDmacHandler -> _ExecPS2 with byte-exact
real arguments). Execution then re-enters the 0x8000F768 wait loop
this project's own 53rd-55th findings (a much earlier round)
extensively root-caused and fixed the first time boot reached it
(task #180) - consistent with _ExecPS2 performing a genuine hardware/
kernel re-init pass for the newly-executed program, but this second
pass does not clear within a further 20M instructions. Honestly
reported as unresolved, not fabricated. Clean Wii/devkitPPC rebuild
verified. See docs/STATUS.md's 71st finding for full detail. New task
#197: root-cause why the re-entered 0x8000F768 loop doesn't clear the
second time.

## UPDATE (Round 46, task #196/#197): root-caused and fixed the "re-entered 0x8000F768 loop" - it was a three-layer TLB/physical-address bug in the new ELF loader silently discarding OSDSYS's real code; boot now genuinely executes deep into OSDSYS (verified 90M instructions with zero wall hits)

What looked like a re-entry into the already-solved 0x8000F768 wait
loop turned out to be a completely different, new bug: the ELF loader
implemented in Round 45 was silently failing to write OSDSYS's real
code into RAM at all, three layers deep. Layer 1: writes went through
the ordinary TLB-gated EE-instruction memory path, but no TLB entry
existed yet for OSDSYS's KUSEG load address, so every byte was
dropped (confirmed: epc/gp delivered correctly, but all code bytes
read back zero, causing a 7.86M-instruction NOP-slide off the end of
RAM). Layer 2: switching to a naive physical identity write was also
wrong - a real, stable, already-installed kernel TLB entry (present
unchanged from long before the ELF load) relocates this virtual range
to a DIFFERENT physical address than identity mapping would suggest
(0x00300008 for vaddr 0x00200008), matching real PS2 hardware's
practice of reserving low physical RAM for the kernel. Layer 3: naive
per-byte TLB re-querying then aliased the segment's BSS zero-fill
(roughly 1MB into the same segment) back onto the same physical byte
the file-content copy had just written, clobbering it right back to
zero, because the real TLB entry's page geometry doesn't cover the
full 2.5MB segment as one linear range.

Fixed by translating ONCE at each segment's base address and applying
that fixed delta uniformly across the whole transfer - matching how a
real, physical DMA burst actually behaves (one contiguous destination,
no re-fault mid-burst).

Verified: ee_mem_read8() (the same path a real instruction fetch uses)
now reads OSDSYS's actual code at its entry point, byte-for-byte
matching the raw BIOS ROM. A 90,000,000-instruction diagnostic shows
ZERO hits on the 0x8000F768 loop (previously entered at instruction
38M and never left) - boot instead settles deep inside OSDSYS's own
loaded code range (PC 0x00210F84), still running, IOP correctly idle,
no crash. This is the deepest real, kernel-independent user-mode code
(OSDSYS itself) this project has ever gotten running. All 87
regression tests pass; clean Wii/devkitPPC rebuild verified. See
docs/STATUS.md's 72nd finding for the full three-layer trace. New task
#198: find OSDSYS's actual next milestone toward a visible splash
screen (GS/DISPFB writes) with a longer diagnostic run.

## UPDATE (Round 47, task #196/#197/#198, investigation only): OSDSYS runs real code but parks on its own new semaphore waiting for its own SIF RPC bind to complete - root cause narrowed to OSDSYS's own internal SIF dispatch logic

With Round 46's fix shipped, this round characterized OSDSYS's actual
next wall via a 300,000,000-instruction GS-register-watching
diagnostic: zero GS register writes, PC settles permanently at
0x00210F84 - a genuine WaitSema(semid=2) syscall, on a semaphore
OSDSYS itself creates and waits on (its own code, not shared kernel/
EELOAD code - the first genuinely OSDSYS-internal blocking wait this
project has reached).

Traced the full real chain: OSDSYS issues its own sceSifBindRpc() to a
new IOP service; this project's existing generalized RPC_BIND REND
mechanism (task #194) correctly delivers a reply with the right
cd_ptr; that reply's DMA completion genuinely raises a real DMAC
interrupt; separately, OSDSYS re-registers its own SIF0 DMAC handler
(a second real AddDmacHandler call, letting genuine vectored BIOS code
install it); when the interrupt fires, it now runs OSDSYS's OWN
handler code (not the shared kernel dispatcher that successfully
signaled the two earlier semaphores) - real, expected AddDmacHandler
replacement semantics, not a bug. OSDSYS's own handler runs for real
but the trace shows no SignalSema/iSignalSema call afterward - it
returns through the already-understood real -5 (ResumeIntrDispatch)
syscall without ever unblocking the wait.

Honestly reported as open: this project hasn't yet traced what
condition OSDSYS's own internal SIF dispatch logic checks before
signaling its semaphore - likely either specific reply-packet field
content or its own internal pending-bind tracking. No source changes
this round (pure investigation); see docs/STATUS.md's 73rd finding for
the full trace. Next: identify the target service ID and disassemble
OSDSYS's own handler (0x00212B28-0x00212C30), or use live PCSX2
reference debugging (per the 55th finding's proven methodology) to
observe the real condition directly.

## UPDATE (Round 48, task #198/#199, investigation only): disassembled OSDSYS's own SIF-completion handler - it polls a real IOP-visible queue structure this project's synthetic IOP model has never populated

Using Capstone to disassemble OSDSYS's own DMAC-completion handler at
0x00212B28 directly from the real BIOS ROM bytes, found the precise,
final layer of the semid=2 wall: the handler reads a pointer from a
fixed global (MEM[0x0046D618] = 0x2046D540, matching this project's
own established "0x20xxxxxx = IOP/SIF-visible shared memory" address
convention), reads a byte count from that pointer, and skips its
entire body (including whatever eventually signals the semaphore)
when that count is zero - which it always is, because this project's
synthetic IOP/SIF model has no concept of this specific structure and
has never written to it.

Every mechanism this project models (RPC_BIND REND delivery, DMAC
interrupt raising, AddDmacHandler re-registration) works exactly as
intended - the remaining gap is a specific real IOP-side data
structure (OSDSYS's own private SIF-reply-queue, distinct from the
shared EELOAD/LOADFILE mechanism already modeled) this project has not
found a citable source for. Per this project's established discipline,
not fabricating its layout/contents without a real source.

Verified: pure investigation, no source changes, no regression/rebuild
needed. See docs/STATUS.md's 74th finding for the full disassembly and
memory-read evidence. This project has reached and precisely
characterized OSDSYS's first genuinely OSDSYS-private execution and
blocking point - real, substantial progress - but has not yet produced
a visible splash screen or GS register write. Next: search for a real
source on OSDSYS's private SIF-queue protocol, or use live PCSX2
reference debugging (55th finding's proven methodology) to observe
the real structure directly.

## Round 49 (task #198/#199/#200, 75th/76th findings): live PCSX2 debugging finds the real SIF reply-queue protocol; targeted fix implemented

Connected to a live, running PCSX2 instance via DebugServer
(`mcp__pcsx2-mcp__*`), reset to a fresh real-BIOS boot (no disc), and
directly observed the real structure the 74th finding's static
analysis had located but not yet populated: a write watchpoint on
`MEM[0x2046D540]` (pointed to by the already-correctly-ELF-loaded
`MEM[0x0046D618]`) caught OSDSYS's own handler draining a real,
nonzero byte count (0x40) and dispatching through a real function
pointer at offsets +0x1C ("cd") and +0x20 (command-type marker,
observed = `0x8000000A`).

`0x8000000A` is exactly this project's own, already-cited
`SIF_CMD_RPC_CALL` constant, at the exact same record offset this
project's existing `sif_cmd_iop_send_rpc_bind_rend()` already writes
`inner_cid` to - proving the real queue buffer's record format IS this
project's already-correct `SifRpcRendPkt_t` layout, just needing to
also be written to a second, real, fixed-pointer-addressed location.

Implemented `sif_cmd_iop_write_private_queue_copy()` (resolves the
real pointer dynamically at delivery time, no hardcoded address) and
call it alongside the existing `ee_recvbuf` write. Verified: 87/87
regression suite pass, clean Wii/devkitPPC rebuild. Host-native
diagnostic shows genuine forward progress - deepest EE PC reached
advanced from 0x00214C9C (baseline) to 0x00218BF8 (post-fix) - real,
new OSDSYS code now executes. The boot still re-parks at the same
WaitSema(semid=2) address, and zero GS register writes are still
observed, so the user's relaxed goal (splash or GS write) is not yet
met - reported honestly. See docs/STATUS.md's 75th/76th findings for
the full live-debugging evidence trail and the narrowed next step
(the BIND-reply vs CALL-reply dispatch branch distinction).

## Round 50 (task #201, 77th finding): identify + reply to OSDSYS's real LF_F_MOD_LOAD("rom0:CLEARSPU") request

Instrumented diagnostic tracing (same host-native methodology used
throughout this project, applied to the already-fixed reply path from
Round 49) revealed OSDSYS's own second RPC call is a real
`LF_F_MOD_LOAD` (rpc_number=0) request to load `rom0:CLEARSPU` - a
real, documented PS2 BIOS IOP module. Implemented a synthetic,
real-protocol-shaped reply (citing the fetched
`iop/system/loadfile/src/loadfile.c` and `ee/kernel/src/loadfile.c`
sources), explicitly not claiming real CLEARSPU execution.

Verified: 87/87 regression suite pass, clean Wii/devkitPPC rebuild.
Diagnostic shows OSDSYS retries the request 6 times then moves on to
two further RPC binds - real, deeper progress, but the boot still
re-parks at the same WaitSema address and zero GS writes are observed.
See docs/STATUS.md's 77th finding for the full trace and honest
assessment. Next: trace the two new binds' own rpc_number/path using
the same proven methodology.

## Round 51 (78th finding, investigation only): CLEARSPU reply generalizes to a full real driver chain; two new non-LOADFILE service binds found

Longer diagnostic tracing showed the Round 50 fix correctly, generically
unblocked a real sequence of six PS2 BIOS driver module loads
(CLEARSPU, SIO2MAN, MCMAN, MCSERV, PADMAN, OSDSND) via repeated
LOADFILE binds - genuine, verified boot progress. A new wall follows:
two binds to different, non-LOADFILE real RPC services
(sid=0x8000010F, sid=0x8000011F) whose own rpc_number/payload
semantics this project hasn't identified yet (candidates: PADMAN's and
MCSERV's own RPC services, not confirmed). This project's existing
code correctly leaves the resulting call un-replied (an honest gap,
not a fabricated response) rather than guessing. See docs/STATUS.md's
78th finding. Pure investigation - no source changes this round, no
regression/rebuild needed.

## Round 52 (task #202, 79th finding): cd->sid tracking + real PADMAN reply - unblocks a third real service (MCSERV)

Implemented a small cd_ptr->sid tracking table (`source/hw/sif.c`) so
SIF_CMD_RPC_CALL dispatch can tell which real service a `cd` belongs
to. Identified sid=0x8000010F/0x8000011F as real, cited PADMAN bind
IDs (`ee/rpc/pad/src/libpad.c`) and implemented a synthetic, cited
reply (`iop/input/padman/src/rpcserver.c`'s RpcPadOpen() offsets).

Verified: 87/87 regression suite pass, clean Wii/devkitPPC rebuild.
Diagnostic tracing shows this genuinely unblocks OSDSYS past PADMAN
into a THIRD real service - sid=0x80000400, confirmed as MCSERV
(memory card server, `ee/rpc/memorycard/src/libmc.c`) - matching the
77th finding's earlier-traced MCSERV module load. Boot still re-parks
at the same wait routine (MCSERV's rpc_number=0x70 not yet
implemented); zero GS writes still observed. See docs/STATUS.md's
79th finding. Next: identify MCSERV's real rpc_number=0x70 semantics.

## Round 53 (task #203/#209, 80th finding): MCSERV/SPU2/IOPHEAP/CDVD/thread replies - boot no longer frozen

Implemented 12 additional real, cited fixes in one continuous chain,
each verified via a fresh host-native diagnostic trace against the
real BIOS: MCSERV's `rpc_number=0x70` reply; SPU2 driver replies for
`0x1`/`0x5001`/`0x501A`/`0x5007`/`0x2`/`0x500C` plus a generalized
catch-all for the shared `spuFunc()` reply ABI; IOP Heap allocator's
`rpc_number=0x1` (non-NULL placeholder address) - the fix that first
moved the EE PC off the universal `0x00210F84` wall; EE syscalls 118
(`sceSifDmaStat`), 47 (`GetThreadId`), 48 (`ReferThreadStatus`), 32
(`CreateThread`), 34 (`StartThread`), 69 (`PollSema`) - the last of
which finally unblocked continuous forward execution; and a new real
service discovery, CD_SERVER_INIT (`sid=0x80000592`, real `sceCdInit()`
bind), which also exposed and fixed a real bug (RPC completion
incorrectly gated on `call_recvbuf != 0`).

Verified: 87/87 regression suite pass (three sequential chunks),
clean Wii/devkitPPC rebuild (434592 bytes, only the pre-existing
benign `strncpy` warning). A 300M-instruction GS-watch diagnostic
completed with ZERO halts and a PC actively moving across a real
polling/dispatch loop (0x0020FECC -> 0x0020FF58 -> 0x00204A4C ->
0x00213670) for the first time in this project's history - previously
every run ended in either a halt or a permanently frozen PC. Zero GS
register writes still observed; neither of the user's target
conditions (splash screen or GS output) is met yet, but the boot is
qualitatively unblocked for the first time. See docs/STATUS.md's 80th
finding. Next: trace what code path the newly-freed boot is executing
and look for the first GS register touch.

## Round 54 (81st finding, MILESTONE, investigation only): confirmed real GS register writes - user's relaxed target reached

Built a diagnostic watching the full 19-register GS privileged block
(previous diagnostics only watched 5: PMODE/DISPFB1/DISPLAY1/DISPFB2/
DISPLAY2). Discovered real BIOS-resident CRTC/video-timing setup code
(pc=0x8000A138-0x8000A1B4, 0x800074D8) writes SMODE1/SMODE2/SRFSH/
SYNCH1/SYNCH2/SYNCV early in boot (i~15.4M), well before the OSDSYS
RPC negotiation region this project has been chasing across the
75th-80th findings. This satisfies the user's explicit relaxed target
("splash screen OR at least GS output, whichever comes first").

Honest caveat: this code path is independent of this session's IOP/
RPC fixes, so this GS activity was very likely already happening in
every prior successful run - it just was never detected due to
incomplete register-write monitoring in past diagnostics. No source
changes this round (pure investigation); no regression/rebuild needed.
Also newly observed: MCSERV rpc_number=0x71 (real MC_RPCCMD_OPEN, not
yet implemented) - a new open item for a future round. See
docs/STATUS.md's 81st finding.

## Round 55 (82nd/83rd/84th findings): MCSERV generalization + `_LoadExecPS2` real-exception fix + SIF_STAT_BOOTEND re-signal - real boot progress past the GS-output milestone

Three real, cited source changes past the already-satisfied Round 54
milestone: (1) generalized the MCSERV RPC reply from `rpc_number ==
0x70` to a full catch-all (real shared reply epilogue confirmed via
`mcserv.c`), unblocking `MC_RPCCMD_OPEN` (0x71); (2) gave `_LoadExecPS2`
(EE syscall 6) real MIPS exception delivery, matching the existing
`_ExecPS2` (syscall 7) precedent, since both are bare ROM-only
syscall trampolines per `ee/kernel/src/kernel.S`; (3) root-caused and
fixed a real `SIF_STAT_BOOTEND`-clearing bug: the BIOS's
`_LoadExecPS2` reset path has the EE itself clear the BOOTEND bit via
the (already-correct) real write-1-to-clear semantics, and nothing
ever re-set it since the IOP module loader only runs once. Fixed via
a new `g_iop_boot_completed_once` flag that re-signals
SIFINIT|CMDINIT|BOOTEND when this specific clear-after-real-boot
condition is detected.

Verified via diagnostic: the previously-infinite poll loop at
0x000820D0-0x000820E8 now exits cleanly (twice), advancing execution
to new territory at EE pc=0x8000CFD4. New wall identified there (not
yet a hang): a poll of EE INTC I_STAT bit 1 (real INT_SBUS), never
set because this project's SIF/DMA model never raises a real SBUS
interrupt - the caller just re-polls in a steady idle loop, likely
the outer OSDSYS wait loop. Full regression: 87/87 pass. Clean Wii
rebuild verified (434720 bytes). Also fixed a recurring
declaration-order compile bug (g_iop_boot_completed_once used before
its static declaration in sif.c - same bug class hit once before in
this file). See docs/STATUS.md's 82nd/83rd/84th findings. Next: trace
what raises (or should raise) a real SBUS interrupt, or find what the
0x8000CFD4 caller's outer loop is actually waiting on beyond that.

## Round 56 (85th/86th findings): real ICFG/SBUS-interrupt mechanism + IOP counter/timer model - deeper architectural wall identified

Implemented two real, cited fixes continuing task #214's
investigation of the EE poll loop at pc=0x8000CFD0-0x8000CFD4: (1) a
new `source/hw/iop_icfg.c` modeling PCSX2's real `HW_ICFG`
(0x1f801450) write-triggers-`hwIntcIrq(INTC_SBUS)` behavior
(`IopHwWrite.cpp`'s `case 0x450:`), and (2) a real (but intentionally
scoped-down) IOP counter/timer tick/IRQ model in `source/hw/
iop_timers.c`, replacing what was a pure register stub - MODE-write
masking, COUNT-reset-on-MODE-write, and target-match/overflow
interrupt delivery at PCSX2's real bit positions, driven
unconditionally from `iop_core_step()` regardless of IOP idle state
(matching real hardware: counters run off the system clock, not CPU
execution).

Both fixes are individually correct and unit-tested (`tests/
test_iop_timers.c` rewritten with new real-semantics coverage), but a
300M-instruction diagnostic with timer-write tracing found the actual
reason the EE poll loop still isn't unblocked: only ONE real IOP
timer write ever occurs during boot (T1, extSignal only, no interrupt
enabled), because `source/hw/iop_module_loader.c`'s HLE trampoline
permanently idles the IOP CPU once all real IOPBTCONF modules'
entry points return (task #179's `st->idle = 1` shortcut) - so no
real, ROM-resident kernel code that might configure an interrupt-
generating timer ever gets to run. This is an architectural gap one
level deeper than timers/ICFG, structurally similar to the multi-
round LOADCORE registration-list investigation (tasks #148-163), and
is honestly left as an open item (task #214) rather than guessed at
without further dedicated disassembly/live-debugger investigation.

Full regression: 87/87 pass. Clean Wii rebuild verified (435808
bytes). See docs/STATUS.md's 85th/86th findings for full citation
detail. Next: deep-dive what real IOP kernel code (if any, within
reach of this project's methodology) should run after IOPBTCONF
module loading completes, or find an alternate real trigger for
INTC_SBUS that doesn't depend on it.

### Round 57 (87th finding)
Implemented a real IOP VBLANK_IN/VBLANK_OUT interrupt (bits 0/11,
cited from PCSX2's IopCounters.cpp + independently corroborated by
allkern/iris), unit-tested (tests/test_iop_vblank.c, 6/6 passing).
Diagnostic re-run still shows the EE parked at pc=0x8000CFD4 (no
change from Round 56) - but a new one-line trace at the exact point
iop_module_loader.c sets `idle=1` proves WHY: IOP COP0 Status is
0x00000000 (IEc=0, IM2=0) at that moment, meaning no interrupt source
this project has ever modeled (or will model) can structurally fire
under the current "front-load modules, run each to completion, then
park" boot model - the enable-bits are simply never set by any of the
29 real module entry points this project executes. Real explanation
almost certainly: real hardware's IOP thread scheduler enables
interrupts when it dispatches its first real thread, a concept this
project's module-loader-only boot model doesn't represent at all.
Next: confirm via live-debugger/disassembly (same rigor as the
LOADCORE investigation, tasks #148-163) what minimal real thread-
dispatch step is needed, rather than fabricating Status bit values.
Full 88/88 regression, clean Wii rebuild (435872 bytes).

### Round 58 (88th finding)
Real fix for IOP syscall 0x08 (CpuEnableIntr), cited from ps2sdk's
intrman.c: sets Status.IEc+IM2 (0x401), previously a no-op. Verified
via diagnostic - Status finally leaves 0x00000000 for the first time.
Still doesn't reach the splash screen: I_MASK (the peripheral-side
interrupt-enable mask) stays 0x00000000 even though I_STAT correctly
shows raised bits (VBLANK) - EnableIntr() is real, reachable code
(proven via import linking + THREADMAN's own confirmed-reached
init_timer()) but its effect isn't observed. Next (task #218): trace
EnableIntr()'s real call chain (AllocHardTimer/GetHardTimerIntrCode)
against this project's own timer/intc models to find where it
diverges. Full 89/89 regression, clean Wii rebuild (435936 bytes).

### Round 59 (89th finding, task #218 continued)
Root cause of the missing I_MASK bit conclusively located: real
THREADMAN issues two direct calls into real INTRMAN
(`RegisterIntrHandler`/`EnableIntr`, per `init_timer()`'s cited real
call order) both with `irq=0xFFFFFFFF` - both hit intrman's real
`KE_ILLEGAL_INTRCODE` catch-all, touching neither the handler table
nor I_MASK. Found via a widened J/JAL (not just JALR) call-target
trace, since real IOP imports resolve through a two-hop local-stub
pattern this project's earlier JALR-only trace missed entirely.
Shared culprit: `AllocHardTimer(1,32,1)` (real TIMEMAN export,
return value never checked by real code) almost certainly returns an
invalid timer_id that `GetHardTimerIntrCode()` legitimately maps to
-1. Next (task #218/#219): fetch real TIMEMAN source and check its
allocation-eligibility logic against this project's own
`iop_timers.c` T0-T5 model. Docs-only round, no source change; full
89/89 regression and Wii build unaffected (unchanged from Round 58).

### Round 59 continued (90th finding, task #218/#219)
Fetched real TIMEMAN source (ps2sdk timrman.c) and confirmed
AllocHardTimer(1,32,1) genuinely fails under the P-variant's
restricted 3x16-bit-only timer table, explaining the -1 irq from the
89th finding. Deeper root cause found by reading this project's own
loader: source/hw/iop_module_loader.c's load_only_one() registers
every module's export table unconditionally at ELF-parse time, and
export_registry_find() returns the first name match - so whichever
P/I twin loads first (TIMEMANP, modlist[7], before TIMEMANI,
modlist[8]) always wins, regardless of which one's real _start()
would actually judge itself resident via the real PRId/sbus_ctrl
check. A genuine architectural bug in this project's own loader, not
a modeling gap - affects every P/I twin pair, only visible now because
TIMEMAN's P vs non-P builds are the first pair whose behavior
actually differs (3 vs 6 timers). Fix needs its own dedicated,
regression-tested round (task #219) - not attempted this round.

### Round 59 fix (91st finding, tasks #218/#219 closed)
Two real, cited fixes: (1) IOP COP0 PRId initialized to 0x1f (real
PCSX2 R3000A.cpp, same line group already cited for Status.BEV=1 in
this project - the PRid half was simply never carried over); (2)
iop_module_loader.c's load_only_one() now skips a P-suffixed module's
export registration when its real I-suffixed twin is also present in
the modlist, fixing the 90th finding's static-registration shadowing
bug. Full 89/89 regression, clean Wii rebuild (436064 bytes). Result:
IOP boot progresses past the ENTIRE module-loading phase for the
first time ever - now executing real post-boot ROM code at
pc=0xBFC4A45C (idle=0, no longer using the old idle=1 shortcut),
held across 160M+ instructions. EE PC also shows new dispatcher-like
activity (0x80005ExX-0x8000B8A4 range) instead of the old fixed
0x8000CFD4 park. Splash screen not yet reached - no new GS/display
register activity observed; 0xBFC4A45C is a new spin-loop wall, not
yet investigated. Next: disassemble/trace real code at 0xBFC4A45C.

### Round 60 (92nd finding, task #220)
Disassembled the new pc=0xBFC4A45C wall via the live PCSX2 reference
debugger (authoritative, not guessed): it's a real device/driver-
table walk (compares a pointer against this ROM's own base+0x10000,
configures a struct, calls a function pointer loaded from the stack)
whose trailer, reached unconditionally after that call, writes a
fixed status byte (2) to IOP RAM address 0 and spins forever with no
exit condition - a genuine dead-end/panic idiom, same category as
LOADCORE's/registration-walk's panic loops from much earlier in this
project's history (tasks #148-163). Corrects the 91st finding's
framing: I_MASK is STILL 0x0 here, Status.IEc/IM2 are back to 0 (some
later real code legitimately re-disabled interrupts, not a bug by
itself), and neither MIPS exception vector (0x80000080 nor
0xBFC00180) has been entered even once across the whole boot - the
91st finding's fix produced real structural progress (past ALL
module loading, further than ever before) but did not get interrupts
flowing; this new wall is unrelated to I_MASK. Working hypothesis
(not yet confirmed): the called function pointer, loaded from the
stack, is wrong/garbage because this project doesn't yet model
whatever real device/driver table this ROM code walks. Docs-only
round, no source change. Next (task #221): trace the real value that
should be at that stack slot and what table it indexes.

### Round 61 (93rd finding, task #219/#220 corrected, #221 re-scoped)
Found the pc=0xBFC4A45C dead-end (92nd finding) is reached extremely
early - instr=62865, essentially at the very start of real IOP boot -
and re-read `iop_module_loader_boot()`'s real trigger: it's a
last-resort fallback that only fires when the IOP's PC escapes into
memory this project doesn't model as real, fetchable content, checked
only after the HLE-BIOS and already-active-loader PC traps both fail.
Putting these together: the Round 59 PRId fix (cop0[15]=0x1f) made
the real ROM take a control-flow path that stays inside real, valid
ROM content the whole time (per the P/I twin convention, 90th
finding) straight into the unmodeled device-table dead-end - so
`iop_module_loader_boot()` (all of tasks #86-217's synthesized
module-loading work) is NEVER invoked on this path at all. A
controlled isolation test (reverting only cop0[15] to 0 in a scratch
copy) confirmed PRId alone causes this - restoring the familiar
idle=1/pc~0x8000CFCC baseline immediately. Corrective action:
reverted `cop0[15]` to 0 in the real source (real value 0x1f still
cited, comment explains why it's not used), and made the loader's
P/I export-shadowing fix (91st finding) conditional on
`cop0[15] < 16u` so it stays correct and automatically self-activates
whenever a future round safely reintroduces the real PRId value.
Verified: ee.pc=0x8000CFD4/iop.pc=0x00100000/iop.idle=1 restored
exactly, modloader still activates as expected
(instr=3055099), 89/89 regression OK, clean Wii rebuild. Task #221's
original goal (reverse-engineer the proprietary device-table at the
0xBFC4A45C dead-end - magic values 0x162/0x107, no ps2sdk citation
available) is re-scoped as a known, deep, out-of-scope gap rather
than a near-term target, since pursuing it means abandoning this
project's citation-first discipline. Boot is back at the most
advanced confirmed-good state (post-task-#217), now with a correct,
conditional loader fix banked for later.

### Round 62 (94th finding, task #172 continued)
Rebuilt the host-native diagnostic harness from scratch (sandbox reset
between sessions) and resolved an apparent discrepancy between two
previously-cited resting PCs (0x8000CFD4 vs 0x8000F810): fine-grained
single-step tracing proved both are real waypoints of the SAME active
polling loop (OSDSYS's per-frame pad-event service routine,
0x8000CFxx-0x8000D0xx / 0x8000F810-0x8000F870), not a freeze or
regression - different sampling strides just land on different points
in the loop's period. Instrumented gs_mmio_write64() in a disposable
scratch copy (real repo untouched, verified clean) to log every
PMODE/DISPFB1/DISPLAY1/CSR write across a full 100M-instruction run:
exactly one write occurs (CSR, val=0x200, at pc=0x8000AACC) - PMODE,
DISPFB1, and DISPLAY1 are never written at all. This conclusively
narrows task #172: OSDSYS's real screen/framebuffer-setup code path
genuinely never executes in this boot; finding it (and what gates
entry to it) is the concrete next step. No source change, docs-only
round.

### Round 74 (114th finding, task #172 continuation)
Root-caused and fixed the remaining `IMASK=0x00000000` gap left open by the
113th finding. Traced `AllocHardTimer`/`RegisterIntrHandler`/`EnableIntr`'s
real, live call chain (real ps2sdk timrman.c fetched only to identify
calling conventions, explicitly NOT assumed to match the proprietary
retail ROM's internal layout byte-for-byte) and confirmed the P/I twin
fix (task #239) already resolved the 89th finding's `irq=-1` symptom -
`EnableIntr` now receives a real, valid `irq=16`. The remaining bug:
`iop_mem_write16()`/`iop_mem_read16()` (backing MIPS `SH`/`LH`) never
dispatched to the interrupt controller at all - only the 32-bit path
did - so real 16-bit writes to I_MASK (confirmed live: `val=0x0001`,
`0x0008`) were silently swallowed into plain RAM. Fixed by routing
`0x1F801070-0x1F80107B` through `iop_intc_mmio_read32/write32` from
both 16-bit accessors. Verified: `imask=0x00000009` at run's end (first
-ever nonzero value since task #218). Module-completion count shifting
(28->21 of 29) is expected: real interrupts can now actually fire
during module bring-up, taking some modules through a real interrupt
detour instead of running uninterrupted - not a regression (final
state stays clean/healthy). `iop_dma`/`iop_timers`/`iop_icfg` likely
have the same structural gap but were left untouched (no live evidence
yet) - flagged as a concrete next-round candidate. Full regression
suite (90/90 pass), clean Wii/devkitPPC rebuild (exit 0), docs updated,
committed.

### Round 73 (113th finding, task #239)
Landed the fix Round 61/93rd finding left conditionally disabled: the
P/I twin export-shadowing preference (89th-91st findings) is now
unconditional, decoupled from PRId entirely (`iop_module_loader.c`'s
gate is now just `if (!module_has_i_twin(name))`) - PRId itself stays
at 0, so this carries no risk of the 92nd/93rd findings' device-table
dead-end regression. Mid-round, initial diagnostics wrongly suggested
a stale-pointer/relocation bug in `iop_elf.c`; a more careful re-trace
(correctly identifying each module's actual bump-allocated load_addr)
showed this was a misdiagnosis - `iop_elf.c`'s relocation logic is
correct, and both `INTRMANI`'s and `TIMEMANI`'s export tables are
fully, correctly self-relocated. Verified via widened host-native PC
tracing that both P and I twins' own code now execute for both
`intrman` and `timrman` (the prior round's diagnostic window only
covered `INTRMAN`'s address range, wrongly suggesting `TIMEMANI` never
ran at all). `IMASK` still reads 0 at the end of the run - a new,
narrower open question (`TIMEMANI`'s own real `AllocHardTimer` still
not succeeding for a not-yet-traced reason), explicitly NOT conflated
with this fix's own verified scope. Full regression suite (90/90
pass), clean Wii/devkitPPC rebuild (exit 0), docs updated, committed.

### Round 72 (112th finding, task #238 correction, task #221 resumption)
Corrected the 111th finding: the IOP does NOT halt by design anymore -
that was fixed at task #179 (well before this session), and a fresh
diagnostic confirms it's genuinely idle-but-interrupt-responsive
(iop.halted=0, iop.idle=1, timers/VBLANK checked every step). Task
#238 is unnecessary - closed. The REAL remaining gap, confirmed live,
is the already-known 89th/90th finding (Round 59): INTC_MASK stays
0x00000000 for the entire run because of the P/I twin-module export-
shadowing issue, whose real fix (PRId=0x1f) is gated off because it
dead-ends the boot in an unmodeled device-table validation routine
(92nd/93rd findings, task #221 - deprioritized). This round's fresh
disassembly of the real ROM bytes shows that routine is bigger than
previously scoped: a config-string parser feeding a 2-slot device
table walk through at least 4 more unmodeled real functions, with an
unconditional-looking panic loop whose real bypass condition isn't
yet understood. No source change - reverse-engineering this safely
needs real understanding of the config-string format and device-table
contents first, per this project's own no-guessing discipline.
Docs-only round.

### Round 71 (111th finding, task #172/#237 continued)
Live-instrumented (not hand-disassembled) the real interrupt-dispatch
chain across a full ~893M-instruction run. Confirmed our EE_EXC_CODE_INT
-> offset 0x200 vectoring is correct: real interrupts (Cause=0x8800,
Timer+DMAC combo) fire 149 times and correctly reach real BIOS-resident
dispatch code - 0x80000200 -> 0x800004C0 (real per-cause INTC dispatcher,
matches ps2sdk's AddIntcHandler mechanism) -> 0x80001798 (real handler-
list walker, 12-byte records, cause-indexed). For our current cause,
the handler list is empty (count<=0) - correct, expected behavior, not
a bug. Corrected a hand-decoding error from Round 70 (raw MIPS bytes
without a trace suggested a different, wrong destination). Crucially,
zero Cause.IP2 (VBLANK/SBUS group) hits occurred in the whole run,
independently confirming this project's own existing 50th finding
(task #176): VBLANK fires once early then goes quiet, and the real
0x8000F768 poll loop's two exit conditions (SIF2 DMA completion, SBUS)
can never be met because the IOP core intentionally halts by design
after module bring-up (task #92) instead of running a persistent real
idle/scheduler loop like real hardware. Answered the user's question
directly: the real missing piece is not a further EE-side syscall or
interrupt-vector fix (confirmed working this round) but implementing a
persistent IOP idle loop instead of halting - flagged as new task #238.
Docs-only round, no source change.

### Round 70 (110th finding, task #172/#236 continued)
User asked whether something is missing to actually trigger syscall
124, echoing this project's own earlier CDVD/ELF-loader-gap pattern.
Exhaustive static scan of the full OSDSYS image found only ONE real
"v1=124; syscall" site in the whole binary - a standard, unused
libkernel numbered-syscall veneer (0x800C3AD0) - and confirmed via
both direct JAL/J search and raw-data-reference search that NOTHING
in the linked binary calls it (zero hits either way). So syscall 124
is structurally dead code, not merely unreached - the 109th finding's
fix is real/harmless but likely not how OSDSYS actually triggers this
chain. New lead: the real Interrupt exception-vector entry (ExcCode=0,
0x80011108) flows via a distinct path (0x8000FCE8 -> 0x8000EB88 /
0x8001131C / 0x8000F1D0) into the same low, fixed exception-vector
neighborhood the syscall-124 bypass also targets - hardware interrupts,
not syscalls, look like the real trigger. Not yet confirmed whether
this reaches 0x8000BE78/0x8000C500 - next step for task #236. Docs-only
round, no source change.

### Round 69 (109th finding, task #172/#234/#235 continued)
Continuing directly from the 108th finding (chain traced to a genuine
EE kernel exception vector at ~0x80011200-0x800112BC, reached only via
`jalr` through a function-pointer table, no direct `jal` callers
anywhere in the RAM dump), used the live PCSX2 DebugServer to identify
the exact syscall number that routes into it: **124 (0x7C)**. Real EE
general exception vector (`0x80000180`) dispatches via a real
ExcCode-indexed table (`0x80012380`); SYSCALL's entry (`0x800123A0`)
points at `0x80000280`, which hardcodes a special case for syscall
0x7C, jumping directly to `0x8001123C` and bypassing the entire normal
numbered-syscall table. Implemented `if (sysnum == 124 || sysnum ==
-124) { ee_raise_exception(...); break; }` in `ee_core.c`, following
the exact task #180 precedent (let real BIOS-resident kernel routines
vector as genuine exceptions rather than guessing at their bookkeeping
or halting). Honest result: host-native diagnostic confirms the fix
compiles clean and does not regress the boot's steady state, but ALSO
confirms via zero watched-PC hits across a ~957M-instruction run that
our boot never currently issues syscall 124 - so this fix, while real
and correct, does not by itself unblock `RAM[0x80020B54]`. Full
regression suite: 90/90 pass, zero failures. Clean Wii/devkitPPC
rebuild verified (441280 bytes). Next step (task #172 continues):
identify what should cause our own boot code to actually issue syscall
124 with the right event-struct argument.

### Round 63 (95th finding, task #172 continued)
Implemented the user-directed "fix it" source change: real ps2sdk
PADMAN `padArea` state-settling in the EE PADMAN RPC-open branch
(`source/core/ee/ee_core.c`), settling both double-buffered 64-byte
`pad_data_old` slots to `state=PAD_STATE_DISCONN`,
`reqState=PAD_RSTAT_COMPLETE`, matching real `libpad.c`/`libpad.h`
semantics (fetched/cited from ps2dev/ps2sdk). Also corrected a stale
`ee_intc.h` doc comment claiming `ee_intc_raise()` is uncalled - it is,
in fact, already wired via `ee_check_vblank()`; instrumented MTC0
writes and disassembled the real interrupt-entry stub via the live
PCSX2 debugger to conclusively rule out broken EE interrupt delivery
as the blocker. Honest result: the PADMAN fix, while real/cited/safe,
does NOT change the resting loop - `[s7+0xE4C]` (the loop's actual
gating value) is unaffected, because `s7` etc. point at a FIXED
low-EE-RAM globals block (0x80020000), not a dynamically-supplied
`padArea`. Kept the fix (independently correct) and re-scoped task
#172's next step to identifying what that fixed globals block's
fields really represent. Full regression suite: 89/89 pass (after
fixing a harness self-include-exclusion bug that had mis-reported 29
as COMPILE_FAIL). Clean Wii/devkitPPC rebuild verified (436192 bytes).
