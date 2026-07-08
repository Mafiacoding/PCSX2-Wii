# Tests

`test_ee_core.c` is a host-native unit test for `ee_core.c` (compiled
with your regular host `gcc`, not devkitPPC - it's for fast iteration
on interpreter correctness, not part of the Wii build/Makefile).

It hand-encodes a tiny MIPS/EE instruction sequence, runs it through
the interpreter, and checks register results against known-correct
values (cross-checked against PCSX2's own semantics for the opcodes
covered). Run it with:

```sh
gcc -I../include -I../source -o test_ee tests/test_ee_core.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee
```

This caught a real bug during development: the initial memory
accessors used `memcpy()` directly, which silently assumes host and
guest share the same endianness. PS2 (EE/IOP) is little-endian; the
Wii (PowerPC 750) is big-endian. `ee_core.c`'s `ee_mem_read*/write*`
functions now compose/decompose bytes explicitly in little-endian
order instead, and `bios_loader.c`'s ROMDIR walk does the same for the
version-string parse.


`test_iop_core.c` is the same style of host-native test for
`iop_core.c` - verifies basic ALU/load-store behavior and the
LWL/LWR unaligned-load reconstruction logic specifically (new code,
not yet covered by the EE test since ee_core.c doesn't implement
LWL/LWR yet). Needs `sif.c` linked too now that `iop_mem_read32/
write32` route the IOP-side SIF mirror window (0x1D000000-0x1D0000FF)
through it. Run it the same way:

```sh
gcc -I../include -I../source -o test_iop tests/test_iop_core.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_excb.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c
./test_iop
```

`test_iop_module_loader_bootinfo.c` covers Round 29 continued's 12th
change: `iop_module_loader.c`'s boot_info struct (the `$a0` argument
every loaded module's entry point receives). A live-traced
disassembly of the real SCPH-10000 BIOS's own SYSMEM init code found
it reads a LARGER struct than the single RAM-MB word previously
allocated, and actively dereferences offset 0x0C as a pointer
(`sw $zero,(a0)` where a0 == that field's value) - before this fix,
offset 0x0C was always 0 (out of bounds of the old 4-byte
allocation), so that store's real target was RAM address 0. This test
uses an entirely synthetic ROMDIR + ELF module image (same convention
as `test_bios_loader.c`/`test_iop_elf.c` - no real BIOS bytes) to
drive `iop_module_loader_boot()` end to end and verify the fix: offset
0x0C now holds a valid, non-null, dedicated scratch address distinct
from the struct itself, while the still-unknown offsets
(0x04/0x08/0x10/0x14/0x18/0x1C) stay honestly zero rather than being
fabricated. 7 checks.

```sh
gcc -I../include -I../source -o test_iop_module_loader_bootinfo tests/test_iop_module_loader_bootinfo.c ../source/core/iop/iop_core.c ../source/hw/iop_elf.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_hle_modules.c ../source/hw/iop_excb.c ../source/hw/iop_cdvd.c ../source/hw/iop_spu2.c
./test_iop_module_loader_bootinfo
```


`test_dma_core.c` covers the DMA register skeleton
(`source/hw/dma.c`) - channel address decoding and register roundtrip.
This one caught a real bug during development: the initial channel
decoder masked addresses to a fixed 0x1000-byte block to find the
channel base, which incorrectly matched `fromSPR`/`toSPR` (and
`fromIPU`/`toIPU`, `SIF0`/`SIF1`/`SIF2`) to the same base since those
channels pack two channels into one 0x1000 region using 0x400-byte
sub-blocks. Fixed with an explicit (base, size, channel) range table
instead of address masking. Run it the same way:

```sh
gcc -I../include -I../source -o test_dma tests/test_dma_core.c
./test_dma
```


`test_ee_dma_bus.c` verifies the wiring between `ee_core.c`'s memory
bus and the DMA register skeleton: a `SW` to a DMA channel's register
address is routed to `dma.c` instead of silently vanishing, and a
subsequent `LW` reads back the same value (correctly sign-extended
into the 64-bit register, matching real EE/PCSX2 LW semantics). Needs
`dma.c` linked in as well as `ee_core.c`:

```sh
gcc -I../include -I../source -o test_ee_dma tests/test_ee_dma_bus.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_dma
```


`test_gs_registers.c` covers the GS privileged register skeleton
(`source/hw/gs.c`) - register roundtrip and specifically the GS_CSR
write-1-to-clear semantics vs. GS_IMR's plain read/write behavior.
This test caught a real bug while writing it: the CSR special-case in
`gs_mmio_write64` was originally keyed to GS_IMR's address (0x12001010)
instead of GS_CSR's (0x12001000), which would have made the actual
CSR register behave like a plain store and made IMR wrongly
write-1-to-clear. Fixed before it was ever committed. Run it with:

```sh
gcc -I../include -I../source -o test_gs tests/test_gs_registers.c
./test_gs
```


`test_ee_unaligned.c` covers `ee_core.c`'s LWL/LWR/SWL/SWR (unaligned
load/store). Needs `dma.c` and `gs.c` linked in too, since ee_core.c
now depends on both:

```sh
gcc -I../include -I../source -o test_ee_unaligned tests/test_ee_unaligned.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_unaligned
```

Writing this test surfaced a genuine subtlety worth documenting: the
correct address pairing for these instructions is LWL/SWL at
`start+3` and LWR/SWR at `start` (not the other way around) for this
little-endian-target formula set - i.e. the "R" variant always uses
the lower/start address, the "L" variant uses `start+3`. Get this
backwards and the instructions individually "work" (no crash, no
obviously wrong output) but silently reconstruct/store the wrong
bytes. Confirmed by hand-tracing the mask/shift tables before locking
in the test's expected values.


`test_ee_fpu.c` covers the new COP1/FPU opcodes (ADD.S/SUB.S/MUL.S/
DIV.S/ABS.S/NEG.S, MTC1/MFC1, C.EQ.S) with real float arithmetic.
Needs dma.c and gs.c linked too:

```sh
gcc -I../include -I../source -o test_ee_fpu tests/test_ee_fpu.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_fpu
```


`test_gs_mem.c` covers the simplified linear GS local memory model
(`source/hw/gs_mem.c`) - pixel roundtrip and scanline/adjacent-pixel
isolation:

```sh
gcc -I../include -I../source -o test_gs_mem tests/test_gs_mem.c
./test_gs_mem
```

`test_gs_output.c` covers the GS-memory-to-Wii-XFB output path
(`source/hw/gs_wii_output.c`) - the RGB->YCbCr conversion (ported from
libogc's console.c) against hand-verified black/white anchor points,
and the rectangular blit against a mock framebuffer (checking both
that the right pixels get written AND that pixels outside the blit
region are left untouched):

```sh
gcc -I../include -I../source -o test_gs_output tests/test_gs_output.c
./test_gs_output
```

Together these are the first "pixels actually reach a real display"
milestone in the project - `source/main.c` now also draws a 4-color
test pattern to the real Wii/Dolphin framebuffer on every boot (not
yet driven by the BIOS/GIF/DMA pipeline - see docs/ROADMAP.md - but
proof the underlying pixel path is real and working end-to-end, not
just correct in isolated host tests).


`test_dma_chain.c` covers the DMA chain-mode transfer engine
(`dma_channel_kick` in `source/hw/dma.c`): NORMAL mode, CHAIN mode
walking CNT->REFE tags with correct inline/out-of-line data addressing,
STR bit auto-clear on completion, and that an unsupported tag ID
(REF/REFS/CALL/RET) sets an error flag and stops cleanly instead of
misbehaving silently. Doesn't need dma.c linked separately since it
#includes it directly:

```sh
gcc -I../include -I../source -o test_dma_chain tests/test_dma_chain.c
./test_dma_chain
```

Note: after adding chain-mode support, `tests/test_dma_core.c` needed
a small update - it originally wrote CHCR with the STR bit (0x100)
set as part of a plain register-roundtrip check, which now correctly
triggers `dma_channel_kick()` and clears STR again (real hardware
behavior). Updated that test to use a CHCR value without STR set,
since chain/transfer *behavior* now has its own dedicated test file.


`test_gif.c` is the big one: it builds a real PACKED-mode GIF packet
by hand (GIFtag + 6 A+D register writes: FRAME_1, XYOFFSET_1, PRIM,
RGBAQ, and two XYZ2 vertices), feeds it through
`gif_process_quadwords` exactly as the DMA chain engine would, and
checks that a correctly-colored, correctly-positioned filled rectangle
actually lands in GS memory - with explicit checks that pixels just
outside the rectangle's bounds are untouched. This is the first test
in the project that exercises the full intended pipeline in miniature:
GIF packet -> register state -> primitive rasterization -> GS memory.
Self-contained (`#include`s both `gs_mem.c` and `gif.c` directly, no
extra link deps - a stale earlier version of this doc incorrectly
listed `../source/hw/gs_mem.c` as an extra link argument, which
actually fails with duplicate-symbol linker errors since it's already
pulled in via #include; fixed here):

```sh
gcc -I../include -I../source -o test_gif tests/test_gif.c
./test_gif
```

`test_gif_triangle.c` covers the first GS round: flat-shaded
`TRIANGLE`/`TRIANGLE_STRIP`/`TRIANGLE_FAN` (PRIM types 3/4/5), added
alongside the existing SPRITE rasterizer. Same self-contained style as
`test_gif.c` (`#include`s `gs_mem.c`/`gif.c` directly). 13 checks: a
plain TRIANGLE fills exactly its interior (checked against a point just
outside the hypotenuse and a point outside the triangle entirely) and
draws exactly once; a 4-vertex TRIANGLE_STRIP and a 4-vertex
TRIANGLE_FAN each draw exactly 2 triangles that together fill the
whole intended square (checked one sample point per triangle); and a
PRIM change mid-stream correctly resets the vertex-accumulation
sequence so stale vertices from a previous primitive type can't leak
into a new one.

```sh
gcc -I../include -I../source -o test_gif_triangle tests/test_gif_triangle.c
./test_gif_triangle
```

`test_gif_gouraud.c` covers the second GS round (task #78): Gouraud
shading for triangles, driven by PRIM's real IIP bit (bit 3). Same
self-contained style as `test_gif_triangle.c`. Uses a right triangle
(0,0)-(60,0)-(0,60) with distinct red/green/blue per-vertex colors, so
the barycentric weights work out to a clean closed form. 9 checks: a
Gouraud (IIP=1) triangle's sample points near each vertex are
dominated by that vertex's color, its centroid reads back an even
blend of all 3 vertex colors (not any single pure color), and alpha
interpolates too; a flat (IIP=0) triangle with the SAME distinct
per-vertex colors still uses only the last vertex's color everywhere
(proves per-vertex color capture didn't change flat-shading behavior);
and the unrelated SPRITE path still flat-fills correctly.

```sh
gcc -I../include -I../source -o test_gif_gouraud tests/test_gif_gouraud.c
./test_gif_gouraud
```

`test_dma_gif_demo.c` mirrors main.c's "real GIF-packet demo" (added
right after the Gouraud round): builds the exact same A+D-mode GIF
packet main.c builds (FRAME_1 + XYOFFSET_1 + PRIM(TRIANGLE|IIP) +
3x(RGBAQ+XYZ2) for a red/green/blue Gouraud triangle) and drives it
through the real `dma.c`/`gif.c` code via `dma_channel_kick()` -
proving the packet layout is well-formed host-natively, since a clean
devkitPPC compile alone doesn't prove that (this test caught a real
NLOOP/buffer-size bug this same round). 11 checks: the packet builder
fills its buffer exactly, the DMA kick reports no error and fully
consumes QWC/advances MADR, exactly one triangle is drawn with IIP
set, and the same red/green/blue/centroid sample-point checks as
`test_gif_gouraud.c` pass through the real DMA path.

```sh
gcc -I../include -I../source -o test_dma_gif_demo tests/test_dma_gif_demo.c
./test_dma_gif_demo
```

`test_vif.c` covers the first VIF0/VIF1 increment (`source/hw/vif.c`):
NOP/STCYCL/ITOP (VIF0 vs VIF1 masking)/OFFSET+BASE (VIF1-only, rejected
on VIF0)/STMASK/STROW/STCOL/MPG (now writes real VU0/VU1 micro-
instruction memory - see the VU micro-mode round below, no longer
counted as unsupported) and, most importantly, DIRECT (VIF1-only)
forwarding a real SPRITE GIF packet straight to
`gif_process_quadwords()` - verified by reading back the drawn pixel's
color from GS memory, not just parser-internal counters. Also proves
UNPACK (not implemented) stops processing the rest of a transfer
cleanly rather than misparsing it (a marker code placed right after an
UNPACK code is confirmed NOT to have been parsed). 25 checks. No
longer self-contained via `#include` (needed once vif.c started
calling into ee_core.c/vu.c for MSCAL/MSCNT/MSCALF/MPG) - now built as
separate translation units like `test_system_handshake.c`:

```sh
gcc -I../include -I../source -o test_vif tests/test_vif.c ../source/core/ee/ee_core.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gs_mem.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/sif.c ../source/hw/mch.c ../source/hw/vu.c
./test_vif
```

`test_vu_micro.c` covers the VU0/VU1 "micro mode" microcode
interpreter (task #87 - `source/hw/vu.c`'s `vu_micro_step()`/
`vu1_exec_micro()`/`vu1_micro_write32()`, and `ee_core.c`'s
`vu0_exec_micro()`/`vu0_micro_write32()`). See
`include/core/hw/vu.h`'s header comment for the full scope: real
memory sizes (VU0 4KB/4KB, VU1 16KB/16KB - PCSX2's `VUmicro.h`), real
TPC/branch/E-bit/I-bit control flow (byte-exact against a live fetch
of PCSX2's `VU0microInterp.cpp`'s `_vu0Exec`), and MPG now actually
writing microprogram bytes into real micro-instruction memory. No real
per-opcode VU instruction body is decoded (no verified real opcode-
number table was found this round despite fetching `VU.h`/`VUmicro.h`/
`VUmicro.cpp`/`VUops.h`/`VUops.cpp`/`VUmicroMem.cpp`/`VU1micro.cpp` -
every instruction pair is fetched and its real flag bits honored, but
its body is a logged no-op). 14 checks: a 4-instruction VU1 program
with the E-bit set on instruction 3 stops after exactly 4 real
instructions (the E-bit one plus its genuine one-instruction "delay
slot" - verified against the exact countdown arithmetic in the cited
PCSX2 source); MSCAL's start address is confirmed to be an
instruction-pair index (`*8` for the byte offset); a safety cap
(65536 instructions - this project's own guard, not real hardware)
correctly terminates a genuinely-infinite all-zero "program"; the
I-flag correctly loads VI[21] (REG_I) from the lower word's raw bits;
`vu1_micro_write32`'s little-endian storage and 16KB address wraparound;
and VU0's execution reusing `ee_state_t`'s existing shared VF/VI/data-
memory fields (from round 13) while keeping its own, separate micro-
instruction memory.

```sh
gcc -I../include -I../source -o test_vu_micro tests/test_vu_micro.c ../source/core/ee/ee_core.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gs_mem.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/sif.c ../source/hw/mch.c ../source/hw/vu.c
./test_vu_micro
```

`test_gif_texture.c` covers texturing for the triangle rasterizer
(task #85): PRIM's TME bit and TEX0's TBP0/TBW/TFX fields. Since this
project has no texture-upload path yet, "textures" are just
pre-existing GS memory content, filled directly via
`gs_mem_write_psmct32()` before the test packet runs. 10 checks: DECAL
fully replaces the vertex color with the texture sample; MODULATE's
exact per-channel blend math against hand-computed expected values
(including an intentionally-truncating case); real per-pixel UV
interpolation across a 3-texel gradient texture, sampled exactly at
each vertex's own screen coordinate (where the barycentric weights are
exactly 1/0/0 by construction - a merely-nearby sample point can snap
to the wrong texel under nearest-neighbor sampling, unlike Gouraud
color's continuous blend); and a TME=0 regression proving flat-shaded
triangles are unaffected by the new texturing code path. Task #88
added `PRIM_FST_MASK` to the 3 textured-PRIM constructions here (this
test never touches ST/Q, so FST=1/UV mode is what it always actually
intended - see task #88's own test file below for why this mattered).

```sh
gcc -I../include -I../source -o test_gif_texture tests/test_gif_texture.c
./test_gif_texture
```

`test_gif_stq_sprite.c` covers task #88: perspective-correct (ST+Q,
PRIM's FST=0) texture coordinates on triangles, and SPRITE texturing
(previously flat-color only). 15 checks: an exact-centroid test
(barycentric weights of exactly 1/3,1/3,1/3 for ANY triangle -
vertices (0,0)/(9,0)/(0,9) give integer centroid (3,3)) with differing
per-vertex Q (1/1/4) proves genuine 1/Q perspective correction is
happening (samples texel 4) rather than a plain-affine fallback
(which would wrongly give texel 3); a second case with equal Q
everywhere confirms the math correctly reduces to the plain-affine
answer when there's nothing to correct for; SPRITE texturing via a new
`rasterize_sprite()` (identity-mapped UV over a 2-axis gradient
texture, exact midpoint sample); and a SPRITE TME=0 regression. Caught
a real test-construction bug while writing this: `GS_REG_UV`'s real
packing puts BOTH U and V in the first A+D word (`GIFRegUV: u16 U;
u16 V; u32 _PAD3` - word1 is pure padding), not one value per word
like ST/RGBAQ/XYZ2 - confirmed against PCSX2's own GS/GSRegs.h. This
was a test bug only; `gif.c`'s own (unchanged) UV handling was already
correct.

```sh
gcc -I../include -I../source -o test_gif_stq_sprite tests/test_gif_stq_sprite.c
./test_gif_stq_sprite
```

Note: as ee_core.c has grown more hw/ dependencies (dma.c, gs.c,
gif.c, gs_mem.c, and now sif.c since sif_init()/sif_mmio_read32/
write32 are wired into ee_core_init() and the memory bus), tests that
link ee_core.c directly need all of these on the command line now -
see the updated commands above.

`test_sif.c` covers the EE-side SIF/SBUS mailbox registers
(`source/hw/sif.c`, addresses 0x1000F200-0x1000F260): MSCOM plain
read/write, MSFLAG OR-on-write, SMFLAG AND-NOT-on-write
(write-1-to-clear, same idea as GS_CSR), and CTRL's read-side fixed
0xF0000102 OR mask plus its internal bit-0x100 lock flag. Register
semantics are ported directly from PCSX2's `HwWrite.cpp`/
`HwRead.cpp`/`Hw.cpp` (SBUS_F2xx cases), not reinvented.

This test caught a real subtlety while writing it: CTRL's read-side
mask (0xF0000102) already has bit 0x100 set as part of the fixed
mask, so every read of that register shows bit 0x100 set regardless
of the register's actual internal state - an initial version of this
test tried to observe the lock-flag clear via `sif_mmio_read32()` and
failed, because the read path can never show that bit as clear. Real
hardware/PCSX2 behavior; the fix was to check the internal state
(`g_sif.ctrl`) directly in the test instead, which is what the test
does now. Self-contained (`#include`s `sif.c` directly, no extra
link deps):

```sh
gcc -I../include -I../source -o test_sif tests/test_sif.c
./test_sif
```

`test_system_handshake.c` is the first test that exercises TWO cores
at once - it hand-encodes a small MIPS program for each of the EE and
the IOP that perform a real SIF mailbox handshake (EE writes MSCOM +
sets MSFLAG bit 0; IOP polls MSFLAG, reads MSCOM, echoes it into
SMCOM, sets SMFLAG bit 0; EE polls SMFLAG, then reads SMCOM back) and
runs them through `system_init()`/`system_run_interleaved()`
(`source/core/system.c`) exactly as `main.c` now does at boot.
Neither side's poll loop can complete without the other side actually
having run in between - if the interleaved scheduler didn't really
alternate between cores, this test would hit its slice cap and fail
cleanly (`system_run_interleaved()` returns 0) instead of hanging.
All 9 checks passed on the first run. Uses two independent fake BIOS
images, one per core, purely for test convenience (real hardware
shares one physical ROM - `main.c` does share one real image between
both `system_init()` arguments for the actual boot path; giving each
core its own independent instruction stream here just keeps the test
simple). Needs the full EE+IOP+hardware-model dependency set linked
in:

```sh
gcc -I../include -I../source -o test_system_handshake tests/test_system_handshake.c ../source/core/ee/ee_core.c ../source/core/iop/iop_core.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_excb.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c
./test_system_handshake
```


`test_iop_intc.c` covers the IOP interrupt controller register model
(`source/hw/iop_intc.c`, I_STAT/I_MASK/I_CTRL at
0x1F801070-0x1F801078). Semantics ported directly from PCSX2's
`ps2/Iop/IopHwWrite.cpp`/`IopHwRead.cpp`: I_STAT write ANDs the
written value in (write-0-to-clear, the OPPOSITE polarity from
GS_CSR/SIF SMFLAG's write-1-to-clear elsewhere in this project),
I_MASK is plain read/write, and I_CTRL has a read-side effect - the
register is cleared to 0 as a side effect of being read (a one-shot
latch, the first register in this project where the interesting
behavior is on the read path rather than the write path). Also
covers `iop_intc_raise()`, the hook future peripheral models will use
to set a pending-interrupt bit. 11/11 checks passed on the first run.
Self-contained (`#include`s `iop_intc.c` directly):

```sh
gcc -I../include -I../source -o test_iop_intc tests/test_iop_intc.c
./test_iop_intc
```

Note: iop_core.c now depends on iop_intc.c too (wired into
iop_mem_read32/write32 alongside the SIF mirror), so test_iop_core.c
and test_system_handshake.c both need it linked in - see the updated
commands above.


`test_iop_dma.c` covers the IOP DMA controller register block
(`source/hw/iop_dma.c`): per-channel MADR/BCR/CHCR/TADR roundtrip and
address-decoding isolation (including confirming the channel-5 gap -
there is no such channel on real hardware - correctly claims nothing),
and the DMA_ICR/DMA_ICR2 bit-level write logic ported from PCSX2's
`ps2/Iop/IopHwWrite.cpp`: bits 0-23 (force-IRQ bit 15, per-channel
enable bits 16-22, master-enable bit 23) are plainly overwritten by
each write, bits 24-30 (per-channel pending flags) are write-1-to-clear
(and can only ever be CLEARED by software, never SET - only real DMA
completion events set them, which this register-stub doesn't
simulate), and bit 31 (master IRQ flag) is recomputed on every write
from the force bit and the enable/flag overlap.

This test caught a real subtlety in itself while being written: an
early version tried to "set" a flag bit (24-30) by writing a 1 to it,
expecting that to both set AND immediately test the master-bit logic
in one write - that doesn't match real hardware, since flag bits are
write-1-to-CLEAR only and can never be set via a register write at
all (only an actual DMA completion event sets them, which isn't
modeled here). Fixed by directly poking the underlying state
(`g_dma.icr`) to simulate a hardware-originated flag, the same
approach `tests/test_sif.c` uses for SIF's SMFLAG, then performing a
separate write that doesn't target that bit to trigger the master-bit
recompute against the pre-set flag. 20/20 checks pass. Self-contained (`#include`s `iop_dma.c` directly):

```sh
gcc -I../include -I../source -o test_iop_dma tests/test_iop_dma.c
./test_iop_dma
```

Note: iop_core.c now depends on iop_dma.c too (wired into
iop_mem_read32/write32 alongside SIF and INTC), so test_iop_core.c
and test_system_handshake.c both need it linked in - see the updated
commands above.


`test_iop_timers.c` covers the IOP counter/timer register stub
(`source/hw/iop_timers.c`, T0-T5 COUNT/MODE/TARGET registers across
the two real hardware address windows 0x1F801100-0x1F80112B and
0x1F801480-0x1F8014AB). This is deliberately a PLAIN register stub -
no real counting/gating/target-IRQ behavior is modeled (see
iop_timers.h for why: real counter behavior needs to tick forward in
sync with instruction execution, a meaningfully bigger effort than a
register latch) - so the test is correspondingly simple: per-counter
roundtrip across both address windows, cross-counter isolation, and
confirming that addresses inside a counter's 12-byte register window
that AREN'T one of the 3 known offsets (e.g. base+2) are correctly
unclaimed rather than silently aliased to the wrong register, plus
the gap between the two hardware address windows also being
unclaimed. 10/10 checks pass. Self-contained (`#include`s
`iop_timers.c` directly):

```sh
gcc -I../include -I../source -o test_iop_timers tests/test_iop_timers.c
./test_iop_timers
```

Note: iop_core.c now depends on iop_timers.c too (wired into
iop_mem_read32/write32 alongside SIF, INTC, and DMA), so
test_iop_core.c and test_system_handshake.c both need it linked in -
see the updated commands above.


`test_iop_hle_bios.c` covers the IOP BIOS syscall trap
(`source/hw/iop_hle_bios.c`, wired into `iop_core.c`'s `iop_step()`).
This is the classic PS1/PS2 A0/B0/C0 BIOS call convention - real code
puts a function number in $t1 and does `JAL 0xA0` (or 0xB0/0xC0);
this project's trap intercepts execution reaching one of those three
addresses instead of decoding whatever bytes happen to be there,
logs the call, sets $v0 to a generic default (0), and redirects
straight to $ra as if a real `JR $ra` had executed. See
`include/core/hw/iop_hle_bios.h` for why specific per-function-number
behavior is deliberately NOT implemented (this project doesn't have a
verified reference for real PS1/PS2 BIOS function semantics, and
PCSX2's own actual HLE - `IopBios.cpp` - takes a completely different,
much more involved approach that depends on parsing a REAL running
BIOS's internal structures).

This test hand-encodes a small IOP program (ADDIU to set $t1, JAL
0xA0, a delay-slot NOP, then code that copies $v0 and BREAKs) and
caught a genuine subtlety while being written: JAL is a MIPS J-type
(pseudo-direct) jump whose target address is built from the CURRENT
pc's upper 4 bits plus a 26-bit immediate field - it can only reach
addresses within the same 256MB segment as the JAL instruction
itself. An early version of this test placed its program at the IOP's
BIOS reset vector (0xBFC00000, segment 0xB) the same way
`test_iop_core.c` does, which made the JAL's computed target become
0xB00000A0 instead of the intended 0xA0 - not a bug in the trap
handler, just a test-setup mismatch with how real IOP code actually
calls these traps (from RAM, segment 0, which shares its segment with
the near-zero trap addresses). Fixed by writing the test program into
IOP RAM and starting execution at address 0 instead. All 9 checks
pass. Needs the full IOP hardware-model dependency set linked (same
as `test_iop_core.c`):

```sh
gcc -I../include -I../source -o test_iop_hle_bios tests/test_iop_hle_bios.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_excb.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c
./test_iop_hle_bios
```

Note: iop_core.c now depends on iop_hle_bios.c too (iop_step() calls
it before fetching/decoding any real instruction), so
test_iop_core.c and test_system_handshake.c both need it linked in -
see the updated commands above.

`test_iop_hle_bios_functions.c` covers the real A0-table BIOS calls
added to `source/hw/iop_hle_bios.c` this round (task #86): ABS/LABS,
STRCAT/STRNCAT, STRCMP/STRNCMP, STRCPY/STRNCPY, STRLEN, BCOPY/BZERO,
MEMCPY/MEMSET/MEMMOVE, INITHEAP, FLUSHCACHE, and EXIT/_EXIT - see
`include/core/hw/iop_hle_bios.h` for the psx-spx citation and exact
scope (this closes the "no verified reference" gap for this pure-
computation subset of the A0 table only; module loading and anything
touching files/devices/threads/CD-ROM/memory-cards remains out of
scope). Calls `iop_hle_bios_try_handle()` directly with hand-set
registers/memory rather than hand-encoding MIPS programs for every
case - these calls don't need real instruction-level control flow to
exercise. 26 checks, all passing, including: BCOPY's reversed
`(src,dst,len)` argument order (vs MEMCPY's `(dst,src,len)`); MEMMOVE
deliberately matching psx-spx's documented ";Bugged" real-hardware
behavior (a plain forward byte-copy, NOT overlap-safe) with the
expected corrupted result computed independently in the test rather
than by calling the implementation under test; INITHEAP's
bookkeeping-only recording; FLUSHCACHE's no-op guarantee; an
unimplemented function number correctly falling back to the generic
default without incrementing `known_calls_handled`; and EXIT halting
the core with a descriptive reason. Needs the same link set as
`test_iop_hle_bios.c`:

```sh
gcc -I../include -I../source -o test_iop_hle_bios_functions tests/test_iop_hle_bios_functions.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_excb.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c
./test_iop_hle_bios_functions
```

`test_iop_hle_modules.c` covers the IOP module registry
(`source/hw/iop_hle_modules.c`) - a plain, project-owned bookkeeping
scaffold, NOT a port of real IRX module parsing (see
`include/core/hw/iop_hle_modules.h` for the full scope explanation).
Register/query roundtrip, re-registering an existing name updates its
version in place rather than duplicating the entry, unknown names
correctly report as not registered, and basic input validation (NULL/
empty names rejected, over-long names truncated rather than
overflowing). 14/14 checks pass. Self-contained (`#include`s
`iop_hle_modules.c` directly):

```sh
gcc -I../include -I../source -o test_iop_hle_modules tests/test_iop_hle_modules.c
./test_iop_hle_modules
```

Note: iop_core.c now also depends on iop_hle_modules.c (initialized
alongside the other IOP hardware models in iop_core_init(), though
nothing in the executed instruction path calls into it yet - see its
header comment) - test_iop_core.c and test_system_handshake.c both
need it linked in too; the commands above are already updated.

`test_bios_loader.c` covers `source/core/bios_loader.c`'s ROMDIR
scanning/parsing logic, using an entirely SYNTHETIC, hand-built
ROMDIR fixture - it does NOT embed or ship any real PS2 BIOS bytes
(see the test file's own header comment, and
`data/pcsx2/bios/README.txt`). This test exists because a real bug
was found and fixed after the project's user provided a real,
legally-dumped BIOS image (SCPH-10000, from their own PS2) for local
testing: the loader used to assume the ROMDIR table always lives at
a fixed file offset (0x100), which is simply wrong - the real
SCPH-10000 dump has it at file offset 0x2700. Fixed by scanning for
the well-known, universal RESET+ROMDIR name signature instead of
trusting a fixed offset, and correcting the payload-offset math to
start from file offset 0 (module data is packed sequentially from
the start of the file; the ROMDIR table's own bytes serve as the
payload for its own entry, which is why its file position always
equals the preceding entry's aligned size - not a fixed constant).
This test's synthetic fixture deliberately places its own ROMDIR
table at yet another offset (48) to prove the fix is a genuine scan,
not a second hardcoded offset that happens to match one known real
dump. 5/5 checks pass. Self-contained (`#include`s `bios_loader.c`
directly):

```sh
gcc -I../include -I../source -o test_bios_loader tests/test_bios_loader.c
./test_bios_loader
```

`test_ee_cop0_special.c` covers `ee_core.c`'s COP0 "CO"-format
instructions - RFE, ERET, EI, DI - added directly in response to real
BIOS testing (SCPH-10000, see docs/STATUS.md): the EE interpreter used
to halt on the very first of these it hit. Real MIPS COP0 encodes
these not via the `rs` field (like MTC0/MFC0) but via a 6-bit `funct`
field once `rs`'s top bit is set (`rs & 0x10`) - dispatch had to be
restructured to check `instr & 0x3F` in that case, matching PCSX2's
own `tbl_COP0_C0[64]` table (`R5900OpcodeTables.cpp`). Semantics
ported directly from PCSX2's `COP0.cpp`:
- RFE (`0x42000010`): shifts Status's 3-level KU/IE bit-stack (bits
  0-5) RIGHT by 2. Real note: RFE is not actually implemented on real
  EE hardware per PCSX2's own table (it maps to COP0_Unknown there),
  but the SCPH-10000 BIOS's PS1-backward-compatibility boot path
  executes it anyway, so it's modeled here to let that path progress.
- ERET (`0x42000018`): branches to ErrorEPC (cop0[30]) and clears
  Status.ERL if ERL was set, otherwise branches to EPC (cop0[14]) and
  clears Status.EXL. Has NO branch delay slot, unlike ordinary
  branches - the instruction right after it must never execute.
- EI/DI (`0x42000038`/`0x42000039`): set/clear Status.EIE (bit 16),
  gated by `_EDI || EXL || ERL || KSU==0` - when the gate condition
  isn't satisfied, the write is silently ignored.

9/9 checks pass, covering all three sub-cases plus ERET's no-delay-
slot behavior and EI/DI's gating in both the allowed and blocked
case. After this fix, the real-BIOS diagnostic went from halting at
~99K instructions to running past 5 million without hitting another
unimplemented opcode. Needs the same link set as the other ee_core.c
tests:

```sh
gcc -I../include -I../source -o test_ee_cop0_special tests/test_ee_cop0_special.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_cop0_special
```

`test_iop_syscall.c` covers `iop_core.c`'s SYSCALL exception handling
(MIPS I opcode, SPECIAL funct `0x0C`), ported from PCSX2's
`psxException()` in `R3000A.cpp`. Also added directly in response to
real BIOS testing: the IOP used to halt unconditionally the moment it
hit a real SYSCALL instruction. Now it sets Cause.ExcCode (bits 2-6,
pre-shifted value `0x20` = ExcCode 8/"Syscall"), sets EPC to the
SYSCALL instruction's own address (branch-delay-slot BD-bit handling
is explicitly NOT modeled - documented simplification), vectors PC to
`0xBFC00180` if Status.BEV is set (or `0x80000080` otherwise), and
shifts Status's KU/IE stack LEFT by 2 - the opposite direction from
EE's RFE. Also fixed `iop_core_init()` to set Status.BEV=1 on reset
(`0x00400000`), matching real hardware/PCSX2's `psxReset()` - it was
previously left at 0 via `memset`, which is wrong.

This test also confirmed something easy to get wrong when writing
expectations for it: `iop_core_run()`'s halt path (used here by the
BREAK instruction placed at the exception vector) returns before its
own trailing `instructions_executed++`, same as every `halt()` call
in this codebase - so a 2-instruction program (SYSCALL, then BREAK at
the vector) reports `instructions_executed == 1`, not 2. Not a bug;
just something to know before writing the assertion.

UPDATED (Round 29 continued, 29th change, task #149): after
`iop_core.c`'s BREAK case was changed to auto-return (`$v0=0`,
RFE-equivalent Status pop, `pc=EPC+4`) whenever it's reached with
`Cause.ExcCode==8` still set (an unresolved real syscall falling
through to the still-unclaimed exception vector - see STATUS.md's
29th finding), this test's original single-`iop_core_run()` scenario
became structurally identical to that real case and started hanging
(the BREAK it places at the vector no longer halts, and the
auto-return then resumes into an all-zero/NOP memory region with no
other halt condition, so `iop_core_run()`'s loop never terminates).
Rewritten into two explicit `iop_core_step()` phases: phase 1 single-
steps just the SYSCALL and checks the real vectoring state (Cause/EPC/
pc/Status.BEV) before the vector's own BREAK ever runs; phase 2
single-steps that BREAK and checks the new auto-return behavior
(`halted==0`, `$v0==0`, `pc==0xBFC00004` i.e. EPC+4). 9/9 checks pass
(up from 5/5 - the extra checks cover the new auto-return path
explicitly). Needs the same link set as `test_iop_core.c`:

```sh
gcc -I../include -I../source -o test_iop_syscall tests/test_iop_syscall.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_excb.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c
./test_iop_syscall
```

`test_iop_hle_exception_install.c` covers the one narrow, documented
exception to `iop_hle_bios.c`'s "every A0/B0/C0 call gets a generic
default" rule: InstallExceptionHandlers (C0h/0x07). Added after real
BIOS testing (SCPH-10000, see docs/STATUS.md's "Investigating the new
IOP halt" section) traced a new IOP halt back to this exact gap - the
generic default-return stub was leaving the IOP's hardware-mandated
general exception vector (RAM address 0x80) empty, so a later real
SYSCALL exception jumped into unpopulated memory and eventually
executed garbage. Fixed by having the C0h/0x07 call locate the real,
well-known 16-byte exception-handler trampoline (`LUI $k0,0 / ADDIU
$k0,$k0,<addr> / JR $k0 / NOP` - documented at
https://psx-spx.consoledev.net/kernelbios/) inside the loaded BIOS ROM
itself (a distinctive, unambiguous 3-of-4-words byte signature - only
the ADDIU immediate varies by BIOS revision) and copying those exact,
version-correct bytes to address 0x80 (mirrored to address 0, per
that same document's "Garbage Area" note) - rather than hardcoding an
assumed immediate from the public documentation's example values.

This test builds a synthetic BIOS image (no real BIOS bytes) with the
signature planted at an arbitrary offset with a deliberately
distinctive jump-target immediate (0x0ABC), and confirms: the
signature is found, the exact bytes (including the distinctive
immediate) land at both address 0x80 and address 0, and - importantly
- that a DIFFERENT C0-table function number does NOT trigger the
install (this is a narrow, specific exception, not a blanket behavior
change). 9/9 checks pass.

This test also caught a real test-design mistake while being written
(the same category of mistake as `test_iop_hle_bios.c`'s JAL-segment
issue, but new: an address collision this time, not addressing
range): an early version placed its test program at IOP RAM address
0, which directly overlaps the "Garbage Area" mirror (0x00-0x0F) that
this very feature writes to - the install was overwriting the test's
own BREAK instruction before it ever executed, hanging the test
(`iop_core_run()` has no slice cap and loops until halted). Fixed by
moving the test program to address 0x1000, comfortably clear of every
reserved region in the documented BIOS RAM map. Needs the same link
set as `test_iop_core.c`:

```sh
gcc -I../include -I../source -o test_iop_hle_exception_install tests/test_iop_hle_exception_install.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_excb.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c
./test_iop_hle_exception_install
```

`test_ee_lqsq.c` covers `ee_core.c`'s LQ/SQ (128-bit load/store,
primary opcodes 0x1E/0x1F), ported from PCSX2's `R5900OpcodeImpl.cpp`.
Address is masked to 16-byte alignment (real hardware ignores the low
4 bits rather than faulting); LQ skips the read entirely when rt==$0,
matching PCSX2's own interpreter exactly (unlike other loads elsewhere
in this file, which still perform a discarded read for its memory side
effects even when the destination is `$0`). 8/8 checks: a full 128-bit
roundtrip through a deliberately unaligned SQ offset (proving the
16-byte mask), confirming bytes just past the aligned 16-byte target
are left untouched (no overrun), and the rt==$0 no-read behavior.
Needs the same link set as the other `ee_core.c` tests:

```sh
gcc -I../include -I../source -o test_ee_lqsq tests/test_ee_lqsq.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_lqsq
```

Added directly after real-BIOS testing showed the EE halting on
exactly this gap - but implementing it revealed that halt point never
actually moves (see `docs/STATUS.md`'s "LQ/SQ implemented" section for
the bigger, more important finding this led to: the EE's real-BIOS
progress was never legitimately 53 million instructions in the first
place).

`test_ee_fpu2.c` covers `ee_core.c`'s newer COP1/FPU additions: SQRT.S,
RSQRT.S, MAX.S, MIN.S, and BC1F/BC1T (branch on FP condition flag),
all ported from PCSX2's `FPU.cpp`. 13/13 checks, covering a real
hardware quirk worth remembering: SQRT.S's source operand is **Ft**
(the `rt` field), not Fs - `Fs` is unused for this instruction, unlike
the usual convention. Also covers SQRT.S/RSQRT.S's documented special
cases (negative input takes `sqrt(fabs())` instead of producing NaN;
RSQRT.S with a zero/denormal divisor returns `+Fmax` instead of
infinity), MAX.S/MIN.S's bit-level signed-int comparison trick (which
only differs from a naive float compare when BOTH operands are
negative - both a both-negative and a mixed-sign case are tested), and
BC1F/BC1T actually gating on the condition flag in both directions
(confirms `BC1T` does NOT branch when the flag is clear, not just that
`BC1F`/`BC1T` branch when expected - proving these aren't accidentally
unconditional jumps). Needs the same link set as the other `ee_core.c`
tests:

```sh
gcc -I../include -I../source -o test_ee_fpu2 tests/test_ee_fpu2.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_fpu2
```

`test_ee_fpu3.c` covers `ee_core.c`'s FPU accumulator (ACC) family:
ADDA.S, SUBA.S, MULA.S, MADD.S, MSUB.S, MADDA.S, MSUBA.S, all ported
from PCSX2's `FPU.cpp`. 19/19 checks. Since no MIPS instruction reads
ACC directly, ACC is read back out via a trailing
`MADD.S(fd, f_zero, f_zero)` (== `fd = ACC + 0*0` == `fpuDouble(ACC)`).
Basic checks confirm each opcode's arithmetic (ADDA.S/SUBA.S/MULA.S
writing ACC; MADD.S/MSUB.S reading ACC into `fd`; MADDA.S/MSUBA.S
updating ACC in place). The last test is the important one: it
constructs a case - ACC preset to an overflow-clamped `-Fmax` via
`ADDA.S(-3.4e38, -3.4e38)`, and an `fs*ft` product that overflows to
`+infinity` (`1e30 * 1e30`) - where MADD.S and MADDA.S are given
IDENTICAL inputs but produce DIFFERENT results, because MADD.S clamps
the intermediate product through `fpuDouble()` before adding to ACC
(`-Fmax + Fmax == 0.0` exactly) while MADDA.S adds the raw, unclamped
native product first and only clamps the final sum afterward
(`-Fmax + (raw +infinity) == +infinity`, then clamped to `+Fmax`).
This proves the double-`fpuDouble()`-pass asymmetry documented in
`ee_core.c`'s case comments is a real, observable behavioral
difference, not just a documentation footnote. Needs the same link
set as the other `ee_core.c` tests:

```sh
gcc -I../include -I../source -o test_ee_fpu3 tests/test_ee_fpu3.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_fpu3
```

Still not implemented on the COP1 side: BC1FL/BC1TL ("likely"
branches - this project has no likely-branch infrastructure yet for
ANY branch, integer or FP), and the FPU exception-cause control-
register flags (only the condition flag needed for BC1 is modeled).

`test_ee_mmi_compare.c` covers `ee_core.c`'s MMI compare/max/min/abs
opcode family: PCGTW/PCGTH/PCGTB and PMAXW/PMAXH (MMI0 sub-table),
PABSW/PCEQW/PMINW/PADSBH/PABSH/PCEQH/PMINH/PCEQB (MMI1 sub-table), all
ported from PCSX2's `MMI.cpp`. 32/32 checks. Operands are planted
directly into EE RAM (`st->ram`) and loaded via LQ (128-bit load),
since these are whole-register SIMD lane ops - same approach as
`test_ee_lqsq.c`. Covers: the compare opcodes producing an all-1s/
all-0s mask result rather than a boolean 0/1 (matching real hardware's
SIMD-compare convention, checked in both the true and false direction
for each width); PMAXW/PMAXH/PMINW/PMINH using a genuine signed
comparison (a mixed-sign case where the "expected" max/min wouldn't
survive a naive unsigned/bit-pattern compare); PABSW/PABSH's real
hardware quirk where INT32_MIN/INT16_MIN (0x80000000/0x8000, which
have no positive representation at their own width) clamp to
INT32_MAX/INT16_MAX instead of overflowing back to themselves; and
PADSBH's deliberate asymmetry - confirmed that its low 4 halfword
lanes really do compute PSUBH (rs-rt) while its high 4 lanes compute
PADDH (rs+rt), not a uniform 8-lane op. Needs the same link set as the
other `ee_core.c` tests:

```sh
gcc -I../include -I../source -o test_ee_mmi_compare tests/test_ee_mmi_compare.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_mmi_compare
```

Still not implemented on the MMI side: the other ~42 opcodes
(saturated arithmetic, QFSRV, PMADDW/H family, PINTH/PINTEH, PROT3W,
PEXEH/PEXEW/PEXCH/PEXCW, PMFHL/PMTHL clamping variants).

`test_ee_mmi_sat.c` covers `ee_core.c`'s MMI0 saturated arithmetic
(PADDSW/PSUBSW/PADDSH/PSUBSH/PADDSB/PSUBSB) and PEXT5/PPAC5 (GS
5551-pixel-format unpack/pack), all ported from PCSX2's `MMI.cpp`.
21/21 checks. Same LQ-based RAM-planting approach as
`test_ee_mmi_compare.c`. Each saturated op is checked in both a
normal (non-saturating) case and both overflow and underflow cases,
confirming the clamp targets the correct signed min/max for that lane
width rather than wrapping. PEXT5/PPAC5 are checked with a specific
5551 pixel (R=0x1F, G=0x03, B=0x00, A=1) verified channel-by-channel
after PEXT5, then round-tripped back through PPAC5 to confirm the
original 16-bit pixel value is recovered exactly. Needs the same link
set as the other `ee_core.c` tests:

```sh
gcc -I../include -I../source -o test_ee_mmi_sat tests/test_ee_mmi_sat.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_mmi_sat
```

This completes all defined MMI0 sub-opcodes. Still not implemented on
the MMI side: the other ~34 opcodes (QFSRV, PMADDW/H family,
PINTH/PINTEH, PROT3W, PEXEH/PEXEW/PEXCH/PEXCW, PMFHL/PMTHL clamping
variants, PMULTUW/PDIVUW/PMADDUW and other MMI2/MMI3 arithmetic).

`test_ee_mmi_permute.c` covers `ee_core.c`'s MMI2/MMI3 permute/
interleave opcode family: PINTH, PINTEH, PEXEH, PEXCH, PEXEW, PEXCW,
PREVH, PCPYH, PROT3W - all ported from PCSX2's `MMI.cpp`. 32/32
checks. Operands use distinct, position-identifiable lane values
(e.g. halfword lane N = 0x50+N for Rs / 0x60+N for Rt) so a wrong
permutation shows up immediately as the wrong value landing in the
wrong lane, rather than accidentally passing due to a repeated value.
Specifically distinguishes: PINTH (takes ALL of Rt's lanes plus Rs's
UPPER 4 lanes) from PINTEH (takes only the EVEN-indexed lanes of
BOTH Rs and Rt - a real, easy-to-confuse distinction); and PEXEH from
PEXCH, and PEXEW from PEXCW (each "E"/"C" pair swaps a DIFFERENT lane
pair - 0/2 for "E", 1/2 for "C" - checked so a swapped case doesn't
silently pass the other's test). Needs the same link set as the
other `ee_core.c` tests:

```sh
gcc -I../include -I../source -o test_ee_mmi_permute tests/test_ee_mmi_permute.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_mmi_permute
```

Still not implemented on the MMI side: QFSRV (needs the SA hardware
register and MTSA/MTSAB/MTSAH to set it, none of which exist yet),
the remaining MMI2/MMI3 HI/LO-touching arithmetic (PMADDW/H, PMSUBW/H,
PMULTW/H, PDIVW/PDIVBW, PMULTUW/PDIVUW/PMADDUW), and PMFHL/PMTHL
clamping variants.

`test_ee_mmi_pvshift.c` covers `ee_core.c`'s PSLLVW/PSRLVW (MMI2
variable-shift word-pair opcodes), ported from PCSX2's `MMI.cpp`.
6/6 checks. Confirms the shift amount is masked to 5 bits (an amount
of 35 behaves identically to 3), that PSLLVW's 32-bit result is
sign-extended to 64 bits when bit 31 is set (proving it isn't a plain
zero-extending shift), and that PSRLVW is a genuinely LOGICAL right
shift - `0x80000000 >> 4` gives `0x08000000`, not `0xF8000000` -
even though the final 32-bit result is still sign-extended to 64 bits
afterward like every other GPR result in this file (the two facts
don't contradict: no sign propagates *during* the shift, but the
*result* of the shift is sign-extended same as any other 32-bit
value stored into a GPR). Needs the same link set as the other
`ee_core.c` tests:

```sh
gcc -I../include -I../source -o test_ee_mmi_pvshift tests/test_ee_mmi_pvshift.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_mmi_pvshift
```

Still not implemented on the MMI side: QFSRV (needs the SA hardware
register and MTSA/MTSAB/MTSAH to set it, none of which exist yet),
the remaining MMI2/MMI3 HI/LO-touching arithmetic (PMADDW/H, PMSUBW/H,
PMULTW/H, PDIVW/PDIVBW, PMULTUW/PDIVUW/PMADDUW), and PMFHL/PMTHL
clamping variants.

`test_ee_cop0_prid.c` covers `ee_core.c`'s COP0 PRId (register 15,
Processor Revision Identifier) initialization - the fix for the root
cause identified in "EE JALR investigation, round 5" (see
docs/STATUS.md). Before this fix, `cop0[15]` was left at 0 by
`ee_core_init()`'s `memset()`; the real BIOS's very first instruction
reads this register and immediately branches on it to pick between two
completely different early-boot code paths, so leaving it at the wrong
value sent this project's interpreter down a path that never reaches
the real vector-install routine that populates low RAM. 4/4 checks:
confirms `cop0[15] == 0x00002e20` right after `ee_core_init()` (ported
directly from PCSX2's own `R5900.cpp`), confirms `MFC0 $t0,$15` reads
it back correctly and sign-extended, and includes a sanity check
spelling out the exact real-BIOS branch condition (`SLTI $at,$k0,89`)
this fixes.

```sh
gcc -I../include -I../source -o test_ee_cop0_prid tests/test_ee_cop0_prid.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_cop0_prid
```

`test_ee_cop0_tlb.c` covers `ee_core.c`'s real R5900 TLB support -
TLBR/TLBWI/TLBWR/TLBP (ported from PCSX2's `COP0.cpp`) plus the new
`ee_tlb_translate()` helper that `ee_mem_ptr()` now uses for any
address below `0x80000000` (KUSEG), where real hardware requires a
TLB entry rather than a fixed physical mask. This test was written as
part of "EE JALR investigation, round 5" continuation, once the COP0
PRId fix (see `test_ee_cop0_prid.c`) let boot progress far enough to
reach real TLBWI calls. 15 checks: TLBWI writes the current
PageMask/EntryHi/EntryLo0/EntryLo1 into the indexed `tlb[]` entry;
TLBR reads a `tlb[]` entry back into those same COP0 registers (with
the exact masking real hardware applies); TLBP finds a matching entry
by VPN2 (+ASID/Global) and sets Index accordingly, or sets Index's
sign bit when no entry matches; a full KUSEG SW/LW round-trip through
a manually-installed TLB entry proves address translation actually
lands on the correct physical RAM offset; and a KUSEG TLB-miss case
confirms a real TLB Refill exception fires with the correct Cause/EPC/
BadVAddr/Status.EXL/pc-vector bookkeeping (this last case was rewritten
in "round 7" once real exception delivery existed to test against -
it originally asserted the miss just reads as 0, which was the honest
placeholder behavior before that work; single-stepped rather than run
to completion since this synthetic program has no real exception
handler installed, so running to a BREAK that will never come would
hang forever).

```sh
gcc -I../include -I../source -o test_ee_cop0_tlb tests/test_ee_cop0_tlb.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_cop0_tlb
```

`test_ee_exceptions.c` covers `ee_core.c`'s real MIPS exception
delivery - `ee_raise_exception()`/`ee_raise_tlb_exception()`, ported
from PCSX2's `cpuException()`/`cpuTlbMiss()` in `R5900.cpp` - added in
"EE JALR investigation, round 7" once real-BIOS boot needed it to get
past the `$sp=0x70003eb0` TLB miss wall round 6 left off at (see
docs/STATUS.md). 16 checks across 6 scenarios: a faulting store gets
Cause.ExcCode=TLBS (distinct from a faulting load's TLBL); a fault
inside a branch-delay slot gets Cause.BD set and EPC pointing at the
branch itself, not the delay-slot instruction (this needed real
delay-slot tracking added to `branch_pending`, previously an unused
field - see the file's own top comment); an instruction-fetch fault
happens before the bogus fetched word is even decoded; Status.BEV
correctly selects the RAM vs. ROM vector base; a nested exception
(Status.EXL already 1) freezes EPC and forces the general vector
regardless of the new fault's own ExcCode; and the `exc_raised_this_step`
per-instruction guard (white-box test, calling the raise function
directly twice within one simulated "instruction") correctly ignores
the second call - this guard exists because SWL/SWR's internal
read-then-write of the same address would otherwise raise two
conflicting exceptions for a single guest instruction.

```sh
gcc -I../include -I../source -o test_ee_exceptions tests/test_ee_exceptions.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_exceptions
```

`test_ee_scratchpad_count.c` covers two fixes from "EE JALR
investigation, round 8" (see docs/STATUS.md), both found via a second
live PCSX2 trace: the R5900 Scratchpad RAM (SPR) hardware bypass and
the COP0 Count free-running counter. 12 checks: a SW/LW round-trip
through the *upper* half of the fixed `0x70000000-0x70003FFF` window
(exactly where the real BIOS's kernel stack pointer lands) works with
**no TLB entry installed at all**, proving the dedicated hardware-
bypass path in `ee_mem_ptr()` is what resolves it, not TLB translation;
exact boundary checks (`0x70000000`/`0x70003FFC` map into the new
`scratch[]` buffer, `0x6FFFFFFC`/`0x70004000` do not and correctly fall
through to the normal KUSEG TLB-miss path instead); and COP0 Count
advancing by exactly 1 between two consecutive `MFC0` reads (previously
static forever, hanging a real BIOS delay loop at `pc=0x9FC42500`).
Note: this scratchpad fix also required updating
`tests/test_ee_cop0_tlb.c`'s own KUSEG-translation test case, which had
picked `0x70000000` as its "generic KUSEG address" example before the
scratchpad's special, TLB-bypassing nature was known - moved to
`0x71000000` to keep testing genuine TLB translation.

```sh
gcc -I../include -I../source -o test_ee_scratchpad_count tests/test_ee_scratchpad_count.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_scratchpad_count
```

`test_ee_timer_interrupt.c` covers "EE JALR investigation round 9":
real EE Timer (Count==Compare) interrupt delivery, the direct
follow-up once round 8's Scratchpad RAM + Count fixes let the real
SCPH-10000 BIOS boot reach pc=0xBFC0092C - a genuine "wait for
interrupt" idle loop (`j $` right after a `Compare=1` setup) that this
project had never implemented any interrupt delivery for. 32 checks
across 6 cases: a basic fire (Count reaching Compare with every gating
bit enabled actually takes a real Interrupt exception - ExcCode 0,
Cause.IP7 set, Status.EXL set, EPC at the next not-yet-executed
instruction, Cause.BD clear); two gating checks (Status.IE=0 and
Status.IM7=0 each independently block the interrupt from being taken
even though it still latches); a branch-delay-slot deferral (if Count
reaches Compare during a taken branch's own step, the interrupt must
NOT be taken until after the delay slot also executes, with EPC ending
up at the branch's target - not the branch or the delay slot); a
Compare-write acknowledgment (writing a new Compare value clears the
latched Cause.IP7 pending bit, the real documented MIPS mechanism a
handler uses to re-arm the timer for its next tick); and an "overshoot
still latches" case that reproduces the exact real SCPH-10000
instruction sequence verbatim (`MTC0 Count,0` then, two instructions
later, `MTC0 Compare,1`) - Count has already advanced past 1 by the
time Compare is written, so an exact-equality Count==Compare check
would silently miss the match forever. See docs/STATUS.md's "round 9"
section for the full design: why latching Cause.IP7 unconditionally
every instruction (via Count>=Compare, not ==) and only deferring the
actual TAKING of the interrupt across delay slots was necessary, and
why every test program below deliberately arms Compare before
enabling Status.IE (both registers reset to 0, so Count>=Compare is
trivially true from the very first instruction any program executes -
harmless in practice since real code always arms Compare first while
interrupts are still masked, exactly like the real BIOS does).
Live re-verification against the real BIOS after this fix found a
further, more precise wall - see docs/STATUS.md and docs/ROADMAP.md's
"round 9" sections.

```sh
gcc -I../include -I../source -o test_ee_timer_interrupt tests/test_ee_timer_interrupt.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_timer_interrupt
```

`test_mch.c` covers `source/hw/mch.c` - the MCH_RICM/MCH_DRD RDRAM
auto-init register pair, added in "round 11" as the root-cause fix for
the round 10 investigation's wrong-branch bug. Ported from the
documented PS2Tek/PCSX2 reference logic (see `include/core/hw/mch.h`
for the full citation). 22 checks: MCH_RICM always reads back 0
regardless of what was written; the SA=0x21 (INIT) SDEVID enumeration
sequence returns 0x1F for the first `MCH_RDRAM_DEVICES` (2) reads then
0 forever after; a SA=0x21/SBC=0x1 "reset strobe" write resets the
enumeration counter back to 0 (but the strobe write itself leaves
MCH_RICM's SOP field non-zero, so a read immediately after the strobe
still returns 0 - matching the reference exactly; a real BIOS re-
selects SA=0x21 with SBC=0 before actually reading results, the same
two-step pattern the very first enumeration in this test uses); the
reset-via-strobe is gated off when MCH_DRD's bit 7 is set; SA=0x23/
0x24 (CNFGA/CNFGB) return their fixed 0x0D0D/0x0090 values; SA=0x40
echoes back `MCH_RICM & 0x1F`; SOP!=0 always reads back 0 regardless
of SA; unrelated addresses aren't claimed.

```sh
gcc -I../include -I../source -o test_mch tests/test_mch.c
./test_mch
```

`test_ee_hw_kseg_masking.c` covers a real bug found alongside the MCH
fix in "round 11": `ee_core.c`'s `dma_mmio_*`/`sif_mmio_*`/`mch_mmio_*`
dispatch used to compare the RAW, unmasked virtual address against
physical-style register constants, which only ever matched a bare
KUSEG-style literal address (like this project's own pre-existing
`test_ee_dma_bus.c` happens to construct) and NEVER matched a real
KSEG1 (`0xB000_xxxx`, uncached) or KSEG0 (`0x9000_xxxx`, cached)
access - the address forms real BIOS/game code actually uses for
hardware registers. Fixed via a new `ee_hw_mmio_addr()` helper that
masks KSEG0/1 addresses to their physical form before the dispatch
checks (KUSEG addresses pass through unchanged, preserving the
existing tests). 4 checks, each running a real tiny CPU program (SW/LW
through the actual `ee_core.c` memory bus, not calling the register
module directly): a KSEG1 SIF MSCOM round-trip, the same via KSEG0,
and a KSEG1 round-trip through the new MCH registers reproducing the
exact address forms (`0xB000F430`/`0xB000F440`) the real BIOS boot
path that motivated this whole round uses.

```sh
gcc -I../include -I../source -o test_ee_hw_kseg_masking tests/test_ee_hw_kseg_masking.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_hw_kseg_masking
```

`test_ee_daddi.c` covers DADDI/DADDIU (primary opcodes 0x18/0x19),
found missing ("unimplemented primary opcode 0x19") once the MCH fix
above let real BIOS boot progress roughly 100x further than before,
into RAM-resident code that uses this instruction pair. 3 checks:
ordinary 64-bit add/subtract via immediate, and specifically that a
negative immediate sign-extends across the *full* 64-bit register
(not truncated to 32 bits, the mistake a naive copy-from-ADDIU
implementation would make). Like this project's existing ADDI/ADDIU
(which share one code path and don't implement ADDI's overflow trap),
DADDI/DADDIU are implemented identically here too - a documented,
consistent simplification, not a new inconsistency.

```sh
gcc -I../include -I../source -o test_ee_daddi tests/test_ee_daddi.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_daddi
```

`test_ee_cop2_ctrl.c` covers "round 12" - COP2 (VU0 macro mode)
control-register transfer instructions (MFC2/CFC2/MTC2/CTC2), added
after real BIOS boot (unblocked by round 11's MCH_RICM/MCH_DRD fix)
reached a real init sequence doing a read-modify-write on FBRST
(control register 28, confirmed via a live PCSX2 disassembly) via
cfc2/ori/ctc2 - halting cleanly on "unimplemented primary opcode 0x12"
since this project had zero COP2 dispatch before this round. Scope:
only the 32-bit control-register transfer family is implemented as
plain storage (no VU0/VU1 execution state exists to act on real
FBRST/Status/etc. side effects - see `ee_core.h`'s `cop2_ctrl[]`
comment). 4 checks: CFC2 reads back exactly what CTC2 wrote to FBRST;
the underlying state array actually holds it; MFC2/MTC2 round-trip
through an arbitrary control register; an unwritten register reads
back 0. The actual VU0 vector datapath (QMFC2/QMTC2 128-bit moves,
vector arithmetic like VISWR/VADD/VSUB/etc., dispatched via the 6-bit
funct field once `rs`'s top bit is set) is confirmed NOT implemented
and is the next honest wall - see docs/STATUS.md's "round 12" section.

```sh
gcc -I../include -I../source -o test_ee_cop2_ctrl tests/test_ee_cop2_ctrl.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_cop2_ctrl
```

`test_ee_cop2_vu0.c` covers "round 13" - the actual VU0 vector
datapath: `QMFC2`/`QMTC2` (128-bit GPR<->VF transfers), `VSUB.xyzw`
(3-operand vector float subtract), `VISWR`/`VSQI` (VU0-local-memory
stores). 10 checks: QMTC2/QMFC2 round-trip the full 128 bits exactly;
VSUB self-subtract yields exactly zero in every lane; VF00 reads back
the real-hardware-hardwired `(0,0,0,1.0f)` pattern and writes to it are
silently discarded; VISWR stores the correct VI value into the correct
VU0-mem lane (checked for two different lanes at the same address);
VSQI stores all 4 VF lanes to the correct address and post-increments
the address register afterward.

```sh
gcc -I../include -I../source -o test_ee_cop2_vu0 tests/test_ee_cop2_vu0.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_cop2_vu0
```

`test_ee_cop2_arith2.c` covers Round 29 continued's 10th change -
extending round 13's VU0 macro-mode vector datapath with `VADD`/`VMUL`
(the same 3-operand full-vector shape as the already-implemented
`VSUB`) and `VIADDI` (an immediate integer add closing a gap
previously flagged next to `VIADD`/`VISUB`/`VIAND`/`VIOR`'s own
comment). Also adds first-time coverage for `VIADD`/`VISUB`/`VIAND`/
`VIOR` themselves (implemented in round 13 but never covered by a
host-native test until now). 11 checks: VADD/VMUL compute the correct
per-lane float results; a single-lane destmask (`VADD.x`) only writes
that one lane; the VI-register integer ALU family produces the correct
results; VIADDI's real sign-extension bit-trick (ported from PCSX2's
own `_vuIADDI`, not a plain two's-complement extend) is verified with
both a positive and a negative immediate; VIADDI to VI0 is a no-op.

```sh
gcc -I../include -I../source -o test_ee_cop2_arith2 tests/test_ee_cop2_arith2.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_cop2_arith2
```

`test_ee_ldl_ldr_sdl_sdr.c` covers "round 13"'s `LDL`/`LDR`/`SDL`/`SDR`
(64-bit unaligned load/store-left/right - the doubleword analog of
this project's existing `LWL`/`LWR`/`SWL`/`SWR`). 6 checks: the
canonical `LDL(addr+7)+LDR(addr)` idiom reconstructs a planted
doubleword both at an 8-byte-aligned address and at a genuinely
misaligned one that crosses an 8-byte block boundary; an `SDL`+`SDR`
round-trip via the same idiom writes and reads back correctly.

```sh
gcc -I../include -I../source -o test_ee_ldl_ldr_sdl_sdr tests/test_ee_ldl_ldr_sdl_sdr.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c
./test_ee_ldl_ldr_sdl_sdr
```

`test_iop_pc_guard.c` covers "round 14" - the new IOP PC fetch-sanity
guard in `iop_step()`. Round 14 found a live BIOS boot path where the
IOP executes a genuine `JALR $ra,$s1` whose target only a real IOP
module/IRX loader (out of scope, see `iop_hle_modules.c`) would ever
populate; before this round, fetching from such an address silently
returned 0 (a NOP) forever, letting the IOP "wander" through unmapped
memory for tens of millions of steps before coincidentally halting on
a confusing, unrelated-looking illegal opcode picked up from the SIF
register mirror's non-zero reset default. 7 checks: a deliberately
wild JALR halts within a handful of steps with a message naming both
the escape and the exact offending address; a JALR to a real, valid
BIOS ROM address still works exactly as before (correct link-register
value), proving the guard doesn't break legitimate control flow.

```sh
gcc -I../include -I../source -o test_iop_pc_guard tests/test_iop_pc_guard.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_excb.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c
./test_iop_pc_guard
```

`test_z_buffer.c` covers task #89 (task 6): the Z-buffer / depth-test
implementation added to `source/hw/gif.c` - real `ZBUF_1`/`TEST_1` A+D
registers (`GIFRegZBUF`'s ZBP/ZMSK, `GIFRegTEST`'s ZTE/ZTST, cross-
checked against PCSX2's GS/GSRegs.h), genuine per-vertex Z (from
XYZ2's real Z word), barycentric Z interpolation for triangles, flat
"second-vertex" Z for SPRITE, and the 4 real `GS_ZTST` compare modes
(NEVER/ALWAYS/GEQUAL/GREATER). IMPORTANT: unlike every other test in
this project, this file builds genuine PACKED-mode GIF packets by hand
for its XYZ2 vertices (GIFTag + a dedicated XYZ2-only loop, X in word0/
Y in word1/Z in word2) instead of using the A+D convention every other
test/demo uses - this project's pre-existing A+D XYZ2 convention
(baked into every other test and main.c before this round) has no room
left for a real Z value, an honestly-scoped gap explained in
`include/core/hw/gif.h`'s `tri_z` field comment. 20 checks: ZBUF_1/
TEST_1 register parsing; a centroid-based proof (task #88's technique,
reapplied to Z) that 3 distinct per-vertex Z values genuinely
interpolate rather than default to a constant; GEQUAL rejecting a
farther fragment and accepting a nearer-or-equal one against a
previously-stored Z; NEVER rejecting unconditionally; ZMSK leaving
color writable while the Z buffer itself stays untouched (independently
re-confirmed by testing against the stale stored Z afterward); the
`zbuf_configured` safety gate (this project's own concept, not real
hardware) proving a Z buffer that's never configured behaves exactly
as it did before this round; and SPRITE's flat "completing vertex" Z
convention together with its own GREATER-mode depth test.

```sh
gcc -I../include -I../source -o test_z_buffer tests/test_z_buffer.c ../source/hw/gif.c ../source/hw/gs_mem.c
./test_z_buffer
```

`test_gif_line.c` covers POINT/LINE/LINE_STRIP rasterization (task:
"GS coverage breadth", item 5) added to `source/hw/gif.c` -
`rasterize_point()`/`rasterize_line()`. Like `test_z_buffer.c`, XYZ2
vertices use genuine PACKED-mode GIF packets (real per-vertex Z, not
this project's A+D XYZ2 convention which has no room for Z). 17
checks: a single flat-color POINT with no interpolation; a flat-shaded
LINE using the real "last vertex" color convention (cross-checked
against PCSX2's `GSDrawScanline::CSetupPrim`); a Gouraud-shaded LINE
proving genuine real per-pixel linear interpolation (not a flat fill)
via distinct red/blue endpoints and a roughly-half-way midpoint check;
LINE_STRIP's real rolling 2-vertex-window continuation (3 vertices ->
2 connected segments, same shape as TRIANGLE_STRIP); and a LINE whose
Z fails the real depth test over a pre-populated Z-buffer, proving
color/Z stay completely untouched. NOTE: ZBUF_1's ZBP is a real 9-bit
hardware field (0-511, masked in `apply_ad_write`'s `GS_REG_ZBUF_1`
case) - this test picks a ZBP value that stays both in-range and far
enough from the drawn line's own X span to avoid `gs_mem`'s simplified
flat-addressing scheme aliasing the Z-buffer test pixel with an
actual drawn color pixel (a real trap this round's own test-writing
fell into first, documented here so it isn't rediscovered blindly).

```sh
gcc -I../include -I../source -o test_gif_line tests/test_gif_line.c ../source/hw/gif.c ../source/hw/gs_mem.c
./test_gif_line
```

`test_iop_elf.c` covers task #92: `source/hw/iop_elf.c`, a real
ELF32/MIPS "IRX" module loader with genuine relocation processing
(R_MIPS_32/26/HI16/LO16, cross-checked against ps2dev/ps2sdk's public
`irx.h` and the community "PS2 BIOS in Rust" book - see
`include/core/hw/iop_elf.h` for the full citation trail). Builds a
fully synthetic (non-copyrighted) ELF32/MIPS module at runtime via
`build_synthetic_module()` - no real BIOS bytes appear anywhere in
this file. 19 checks: successful load with correct entry/load_addr/
load_end, bss zero-fill for a segment where memsz > filesz, all 4
relocation types produce byte-exact expected values (manually pre-
computed), a real export table and a real import table are both
found with correct names/counts, and a malformed image (bad ELF magic)
is rejected with a clear error rather than silently accepted.

```sh
gcc -I../include -I../source -o test_iop_elf tests/test_iop_elf.c ../source/hw/iop_elf.c ../source/core/iop/iop_core.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_modules.c ../source/hw/iop_module_loader.c ../source/hw/iop_spu2.c ../source/hw/iop_excb.c
./test_iop_elf
```

`test_vu_micro.c` covers task #87 (control flow) and task #94 (real
opcode table, this round). See `include/core/hw/vu.h` and
`source/hw/vu_opcodes.h` for the full scope/citation trail (the
original Sony VU Instruction Manual). 22 checks total: the original
control-flow set (E-bit/I-bit/branch-delay-slot mechanism, TPC
advance, the 65536-instruction safety cap, VU0/VU1 memory separation)
PLUS 12 new checks added this round validating real arithmetic/
branch/load-store *results*, not just control flow - real ADD,
real ADDA-then-MADD proving the accumulator genuinely round-trips,
real ADDbc lane broadcast, real ADDQ using the Q register, a real
unconditional branch whose delay-slot instruction executes and whose
skipped instruction doesn't (with an exact instruction-count check),
real IADD 16-bit integer arithmetic, and a real SQ/LQ quadword round-
trip through VU1 data memory. One pre-existing assertion was updated
(not loosened, see the file's own header comment): an all-zero
instruction word is no longer "no real opcode" now that real decode
exists - it's a genuinely matched (if degenerate/no-effect) ADDbc/LQ
pair.

```sh
gcc -I../include -I../source -o test_vu_micro tests/test_vu_micro.c ../source/core/ee/ee_core.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gs_mem.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/sif.c ../source/hw/mch.c ../source/hw/vu.c
./test_vu_micro
```

`test_iop_spu2.c` covers task #95 (SPU2 register scaffold, "time
permitting"). See `include/core/hw/iop_spu2.h` for the honest scope
note (a register-file scaffold, not audio synthesis - no per-register
voice/ADSR/volume semantics, no synthesis or DMA pipeline). 10 checks:
direct unit tests of the scaffold's own read16/write16/read32/write32
(in-range vs. out-of-range address boundaries, write-then-readback
persistence, a 32-bit write visible via two adjacent 16-bit reads),
plus integration tests confirming the scaffold is actually reachable
through `iop_core.c`'s real `iop_mem_read16`/`write16`/`read32`/
`write32` (the LH/SH/LW/SW path a real IOP program would use) without
accidentally swallowing unrelated ordinary IOP RAM addresses.

```sh
gcc -I../include -I../source -o test_iop_spu2 tests/test_iop_spu2.c ../source/core/iop/iop_core.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c ../source/hw/iop_excb.c
./test_iop_spu2
```

`test_iop_rfe.c` covers Round 22's RFE (Restore From Exception, COP0
CO-format funct=0x10) fix - a genuine, previously-undocumented gap
found while starting the user-directed "all IOP problems" sweep:
`iop_core.c`'s COP0 dispatch only ever handled MFC0/MTC0, so any real
exception handler that tried to RFE-then-return would have hit an
"unimplemented COP0 sub-opcode" halt. See `docs/STATUS.md`'s "Round
22" section for the full citation trail (ported from PCSX2's
`R3000A.cpp` `psxException()` RFE case). 3 checks, hand-derived
bit-for-bit: an initial `Status` low-6 value chosen so every bit
position is distinguishable (`0b000101`) becomes `0b010100` after the
existing SYSCALL exception-entry push, then `0b010101` after the new
RFE - exactly matching a hand-traced application of the real formula
`Status = (Status & ~0xF) | ((Status & 0x3C) >> 2)`. Also confirms
`Cause` is untouched by RFE (only `Status` is architecturally
affected) and that execution actually continues past RFE to a
following `BREAK` (proving the CO-format path no longer falls into
the "unimplemented" halt default).

```sh
gcc -I../include -I../source -o test_iop_rfe tests/test_iop_rfe.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c ../source/hw/iop_excb.c
./test_iop_rfe
```

`test_iop_hw_interrupt.c` covers Round 22's real hardware-interrupt
delivery fix - see `docs/STATUS.md`'s "Round 22" section for the full
citation trail (psx-spx's interrupts page, explicitly confirmed to
apply to the PS2 IOP: every peripheral IRQ routes through ONE single
CPU line, Cause.bit10/IP2, non-latching, gated by Status.bit10/IM2 and
Status.bit0/IEc). 8 checks across two scenarios: (1) a real program
that raises a pending `I_STAT` bit via `iop_intc_raise()` (simulating
a peripheral), then writes `I_MASK` through an actual `SW` instruction
- proving the interrupt correctly preempts the very next instruction
the instant the enabling write makes `(I_STAT & I_MASK)` nonzero
(EPC, two never-executed marker instructions, Cause.ExcCode/IP2, and
the exact post-push `Status` value are all checked bit-for-bit); (2) a
second program proving NO interrupt fires when `Status.IEc` is 0, even
with `I_STAT & I_MASK` already nonzero from the very start - the
marker instruction executes normally and the instruction count before
the (expected, unrelated) halt is exact.

```sh
gcc -I../include -I../source -o test_iop_hw_interrupt tests/test_iop_hw_interrupt.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c ../source/hw/iop_excb.c
./test_iop_hw_interrupt
```

`test_iop_excb.c` covers Round 22's real RAM[0x100] exception-handler
priority-chain mechanism (`source/hw/iop_excb.c`) - see
`docs/STATUS.md`'s "Round 22" section for the full citation trail
(psx-spx kernelbios.md's "BIOS Interrupt/Exception Handling" section:
"Priority Chains", "C(02h) - SysEnqIntRP", "C(03h) - SysDeqIntRP").
18 checks: the Table-of-Tables pointer/size fields at RAM[0x100]/
RAM[0x104], all 4 priority chains starting empty, real head-insertion
order (newest-first) across two chained nodes with byte-exact next-
pointer linking, chain isolation between different priorities, real
first-element removal, the documented non-first-element SysDeqIntRP
bug modeled as a counted no-op (not silently dropped, not a fabricated
outcome), out-of-range priority handled safely, and the real C0-table
`C(02h)` HLE trap end-to-end (register-convention parameter reads via
$a0/$a1, correct chain mutation, correct return-to-$ra).

```sh
gcc -I../include -I../source -o test_iop_excb tests/test_iop_excb.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c ../source/hw/iop_excb.c
./test_iop_excb
```

`test_gs_alpha.c` covers GS Round 23: the GS alpha unit - alpha test
(`TEST_1`'s `ATE`/`ATST`/`AREF`/`AFAIL`) and alpha blending (`ALPHA_1`,
gated by `PRIM`'s new `PRIM_ABE_MASK` bit) - cross-checked against
PCSX2's GS/GSRegs.h and GSDrawScanline.cpp via a dedicated research
pass (see `docs/STATUS.md`'s "GS Round 23" section for the full
citation trail, including one detail sourced from a developer forum
post rather than primary source, flagged as such there). 13 checks,
using SPRITE draws via this project's established A+D-mode XYZ2
convention: `ATST_NEVER` discards every fragment; `ATST_GEQUAL`
passing vs. failing cases against a specific `AREF`; all 4 `AFAIL`
outcomes (`KEEP`/`FB_ONLY`/`ZB_ONLY`/`RGB_ONLY`, including
`RGB_ONLY`'s old-framebuffer-alpha-byte preservation); a hand-computed
50% alpha blend (`A`=`Cs`,`B`=`Cd`,`C`=`Af`,`D`=`Cd`,`FIX`=64) of an
opaque red fragment over an opaque blue background, verified
channel-by-channel against the real truncating-divide blend equation
(no rounding bias - R=127 not 127.5, B=128 not 127, since truncation
direction differs by operand sign); and a regression check that
`ALPHA_1` being configured has zero effect when `PRIM.ABE`=0.

```sh
gcc -I../include -I../source -o test_gs_alpha tests/test_gs_alpha.c ../source/hw/gif.c ../source/hw/gs_mem.c
./test_gs_alpha
```

`test_gs_clut.c` covers GS Round 24: CLUT/paletted textures (PSMT8/
PSMT4), driven by TEX0's PSM/CBP/CPSM/CSA fields. **Citation-honesty
note**: this round's live source-fetch research pass hit a session
limit before it could run, so the CLUT addressing scheme and the
PSMT8 CSM1 index-swizzle below are sourced from established PS2 GS
knowledge rather than a fresh primary-source citation trail this
round (see `docs/STATUS.md`'s "GS Round 24" section for the full
caveat). 6 checks, using TRIANGLE + UV-mode (FST=1) single-texel
sampling (reusing `test_gif_texture.c`'s own established convention):
PSMT4 basic lookup against a known CLUT entry; PSMT4 with CSA=2
proving bank selection actually changes which palette is used;
PSMT8 index 8 resolving through the real CSM1 swizzle (bits 3/4
swapped) to CLUT entry 16; the symmetric PSMT8 index 16 resolving to
entry 8; a PSMT8 index (3) with both swizzle bits already clear,
confirming it's left unaffected; and a PSMCT32 regression check
proving the default (unset) PSM samples directly, CLUT path not
engaged.

```sh
gcc -I../include -I../source -o test_gs_clut tests/test_gs_clut.c
./test_gs_clut
```

`test_gs_swizzle.c` covers GS Round 25: real PSMCT32 page/block-
swizzled addressing (`gs_mem_swizzle_addr32()` and its read/write
wrappers), added as a separate, additive API alongside the
pre-existing simplified-linear `gs_mem` functions - see
`include/core/hw/gs_mem.h`'s extended comment for why this isn't a
drop-in replacement of the pipeline's addressing (existing tests'
`bp` picks aren't valid real-hardware pointers under real addressing)
and `source/hw/gs_mem.c` for the real page/block table and this
round's citation-honesty note (live source-fetch research hit a
session limit again this round - mitigated by a structural no-
collision test property below). 10 checks: hand-derived known-value
checks against the documented 8x4 block-index table (pixels (0,0),
(8,0), (0,8), and the page's last pixel (63,31) all land at their
expected byte offsets); `bp` behaving as a real page unit (+8192
bytes between `bp=0`/`bp=1`); a 2-page-wide buffer's second page and
second row landing at correct offsets; a no-collision property across
a full page's 2048 pixels; a full-page write/read round-trip; and a
check that the new function and the pre-existing linear one genuinely
disagree at a non-degenerate coordinate (not accidentally aliased).

```sh
gcc -I../include -I../source -o test_gs_swizzle tests/test_gs_swizzle.c ../source/hw/gs_mem.c
./test_gs_swizzle
```

`test_gs_reglist_image.c` covers GS Round 26: REGLIST and IMAGE GIF
transfer modes, previously entirely unimplemented (any non-PACKED tag
was byte-skipped with zero interpretation). See
`include/core/hw/gif.h`'s `GS_REG_BITBLTBUF`/`TRXPOS`/`TRXREG`/
`TRXDIR` comments and `source/hw/gif.c`'s `process_one_packet()`
REGLIST/IMAGE branches for the full scope and this round's citation-
honesty note (live source-fetch research hit a session limit again
this round, same caveat as Rounds 24-25). 15 checks: REGLIST with an
even register count (2 registers, 1 qword) verifying both land
correctly; REGLIST with an odd count (3 registers, 2 qwords, second
qword's upper half being real padding) verifying all 3 registers
apply AND that the packet immediately following still parses
correctly (a real byte-accounting bug found and fixed this round -
the old fallback wrongly assumed REGLIST's span equaled NLOOP qwords);
a full host-to-local IMAGE transfer (3x2 pixel rectangle) verifying
every pixel lands correctly including RRW-boundary wrapping and
auto-deactivation once the rectangle fills; and an IMAGE packet with
no prior TRXDIR trigger, verifying gs_mem stays untouched while the
stream stays in sync for the packet after.

```sh
gcc -I../include -I../source -o test_gs_reglist_image tests/test_gs_reglist_image.c
./test_gs_reglist_image
```

`test_gs_context2.c` covers GS Round 27: GS Context 2 (dual-context)
support, previously entirely unimplemented - only context 1 existed
and PRIM's CTXT bit was never even parsed. See
`include/core/hw/gif.h`'s `PRIM_CTXT_MASK`/`GS_REG_FRAME_2`/
`XYOFFSET_2`/`TEX0_2`/`TEST_2`/`ALPHA_2`/`ZBUF_2` comments and
`source/hw/gif.c`'s `gs_activate_context()` for the full design and
this round's citation-honesty note (live source-fetch research hit a
session limit again this round, mitigated by an internal self-
consistency check across 6 already-added register-address pairs).
10 checks: two sprites at the same screen position with different
PRIM.CTXT bits landing in their own contexts' FRAME targets; a
configured-but-never-selected FRAME_2 having zero effect on context 1
and its own target buffer staying untouched; independent per-context
alpha test state (TEST_1=ATST_NEVER vs TEST_2=ATST_ALWAYS) proven via
opposite outcomes for the identical primitive; and an interleaved
ctx1/ctx2/ctx1 draw sequence proving neither context's state leaks
into or gets clobbered by the other.

```sh
gcc -I../include -I../source -o test_gs_context2 tests/test_gs_context2.c
./test_gs_context2
```

`test_gs_context2_mipmap.c` covers Round 29 continued's 15th change:
making TEX1/MIPTBP1/MIPTBP2 genuinely per-context (Round 28's mipmap
support was context-1-only, one of the specific gaps Round 27's own
dual-context work explicitly left open). Configures context 1 WITH
mipmapping engaged and context 2 WITHOUT (against the same base
texture and the same minifying screen size) and draws one SPRITE per
context: context 1 must sample its own configured mip level, context
2 must use the base level - proving the two contexts' mip
configuration is genuinely independent, not shared/leaking state. 6
checks (2 pixel-sampling outcomes + 4 direct permanent-storage checks).

```sh
gcc -I../include -I../source -o test_gs_context2_mipmap tests/test_gs_context2_mipmap.c -lm
./test_gs_context2_mipmap
```

## test_gs_mipmap.c (GS Round 28)

Tests TEX1/MIPTBP1/MIPTBP2 register parsing and SPRITE-only,
per-primitive mipmap LOD selection (real hardware also mipmaps
TRIANGLE - not implemented here, a documented gap). See
`include/core/hw/gif.h`'s `GS_REG_TEX1_1`/`MIPTBP1_1`/`MIPTBP2_1`/
`GS_MMIN_MIPMAP_THRESHOLD` comments and `source/hw/gif.c`'s
`rasterize_sprite()` mip-level-selection block for the full design
and this round's citation-honesty note (live source-fetch research
hit a session limit again this round, sourced from established
knowledge rather than a fresh citation trail, same caveat as Rounds
24-27). Scope: context 1 only, SPRITE only, per-primitive (not
per-pixel) nearest-single-level selection (no trilinear blending),
MTBA=1 auto-addressing falls back to level 0 (documented gap).

25 checks: TEX1 field round-trip including a negative (sign-extended)
K value; MIPTBP1/MIPTBP2's 6 mip levels' TBP/TBW round-trip including
the two fields that straddle the word0/word1 boundary; computed LOD
(LCM=0) selecting the correct level from a texture/screen size ratio
and actually sampling that level's distinct buffer; MXL clamping a
computed LOD down to the configured maximum; magnification (texture
smaller than screen) always using the base level; MMIN below the
mipmap threshold disabling mipmapping entirely; fixed LOD (LCM=1)
overriding the computed formula via K; MTBA=1 safely falling back to
the base level; and a regression check that an unconfigured TEX1
(MXL=0 default) behaves exactly as before this round.

```sh
gcc -I../include -I../source -o test_gs_mipmap tests/test_gs_mipmap.c -lm
./test_gs_mipmap
```

`test_gs_mipmap_triangle.c` covers Round 29 continued's 14th change:
extending Round 28's mipmap support (previously SPRITE-only) to
TRIANGLE, using the exact same per-primitive (not per-pixel/
trilinear) LOD-selection formula, with the triangle's screen-space
bounding box standing in for SPRITE's well-defined width/height (a
triangle has no single natural "size" otherwise). 3 checks: computed
LOD samples the correct mip level; MXL clamps a computed LOD down to
the configured maximum; magnification (texture smaller than the
bounding box) always uses the base level.

```sh
gcc -I../include -I../source -o test_gs_mipmap_triangle tests/test_gs_mipmap_triangle.c -lm
./test_gs_mipmap_triangle
```

`test_iop_kmem_alloc.c` covers Round 29's real B(00h)
`alloc_kernel_memory(size)` bump allocator (`source/hw/iop_hle_bios.c`)
and the companion fix in `source/hw/iop_excb.c` - see `docs/STATUS.md`'s
"Round 29 continued" section for the full citation trail (psx-spx's
BIOS RAM Map: "0000E000h 2000h Kernel Memory; ExCBs, EvCBs, and TCBs
allocated via B(00h)"). Live tracing against the user's real
SCPH-10000 dump found that genuine, executing BIOS ROM code calls this
function via a thunk-table tail call (`jr`, not `jal`/`jalr`), and
previously always got the generic default return value (0, "alloc
failed") since this project had no real B0-function-0 case, which is
why `RAM[0x100]` never got populated even though the real BIOS code
responsible for populating it is demonstrably present and running.

19 checks: the bump pointer starts at the documented Kernel Memory
region base; a real allocation returns that base and advances the
pointer by exactly the (already-aligned) requested size; an unaligned
size request is correctly rounded up to 4-byte alignment; a request
that would overflow the documented 0x2000-byte region fails cleanly
($v0=0) without wrapping or corrupting memory, while still counting as
a handled call (matching real hardware's malloc-style failure
convention); the real B0-table dispatch wiring (register-convention
$a0 size read, $v0 return, correct return-to-$ra); and `iop_excb.c`'s
`chain_head_addr()` correctly following a DYNAMIC RAM[0x100] value
(simulating the real allocator having placed the ExCB array somewhere
other than the old hardcoded constant) with a safe fallback to the old
constant when RAM[0x100] is still 0 (preserving every pre-Round-29
test's assumptions).

```sh
gcc -I../include -I../source -o test_iop_kmem_alloc tests/test_iop_kmem_alloc.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c ../source/hw/iop_excb.c
./test_iop_kmem_alloc
```

`test_iop_syscall_handler.c` covers Round 29 continued's second real
fix: C(01h) EnqueueSyscallHandler(priority) and B(18h) ResetEntryInt().
See `include/core/hw/iop_hle_bios.h`'s `IOP_HLE_C0_ENQUEUESYSCALLHANDLER`
and `IOP_HLE_B0_RESET_ENTRY_INT` comments, and `docs/STATUS.md`'s
"Round 29 continued" section, for the full citation trail: live
tracing against the user's real SCPH-10000 dump found the real BIOS
calls both of these functions right after B(00h) succeeds, and found
the real dispatcher/ReturnFromException code this round's hand-
assembled MIPS trampoline is cross-checked against.

26 checks: ResetEntryInt correctly writes the real, ROM-confirmed
jmp_buf pointer constant (0x00006C34) into RAM[0x7520] and returns it
in $v0; EnqueueSyscallHandler installs a real, position-independent
MIPS trampoline into the Kernel Memory bump allocator exactly once
(reused, not re-installed, on subsequent calls) and enqueues a real
ExCB chain node (via the already-real, Round-22 SysEnqIntRP mechanism)
at the requested priority whose first-function field points at it. The
strongest checks actually EXECUTE the installed trampoline bytes
through the real IOP interpreter (not just inspect them): a simulated
EnterCriticalSection syscall (saved $a0==1) correctly clears SR bits 2
and 10 and returns 1 in $v0 (both were set beforehand, matching
psx-spx's documented return rule) before ending up at the real
ReturnFromException address (0x00000f30); a simulated ExitCriticalSection
(saved $a0==2) correctly sets both bits; and a non-syscall exception
(Cause.ExcCode != 8) correctly returns 0 via a plain `jr $ra` without
touching SR at all, matching the real dispatcher's "let the next chain
element try" contract.

```sh
gcc -I../include -I../source -o test_iop_syscall_handler tests/test_iop_syscall_handler.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c ../source/hw/iop_excb.c
./test_iop_syscall_handler
```

`test_iop_hook_entry_int.c` covers Round 29 continued's 5th finding/fix:
A(13h) setjmp(buf) and B(19h) HookEntryInt(addr). See
`include/core/hw/iop_hle_bios.h`'s `IOP_HLE_A0_SETJMP` and
`IOP_HLE_B0_HOOK_ENTRY_INT` comments, and `docs/STATUS.md`'s "Round 29
continued (5th finding)" section, for the full citation trail: live
call-tracing against the user's real SCPH-10000 dump found the real
BIOS calls these two functions back-to-back with the SAME address
(a0=0x8004fd50 in both, confirmed live) to install its own fallback
recovery point at RAM[0x7520] instead of leaving the kernel's default
struct (0x00006C34) installed forever. 18 checks: setjmp's real
12-word ra/sp/fp/s0-7/gp save; HookEntryInt's RAM[0x7520] write and
return value; the real setjmp+HookEntryInt pairing (same address)
correctly overriding the default; and ResetEntryInt/HookEntryInt
remaining independent of each other (calling one doesn't corrupt the
other's effect).

```sh
gcc -I../include -I../source -o test_iop_hook_entry_int tests/test_iop_hook_entry_int.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c ../source/hw/iop_excb.c
./test_iop_hook_entry_int
```

`test_iop_device_registration.c` covers Round 29 continued's 6th
change: A(96h) AddCDROMDevice() and A(97h) AddMemCardDevice(),
implemented per explicit user request as real, active, queryable
device registration state (not demo/no-op stubs). See
`include/core/hw/iop_hle_bios.h`'s `IOP_HLE_A0_ADDCDROMDEVICE` and
`IOP_HLE_A0_ADDMEMCARDDEVICE` comments for why this project does not
(yet) fabricate an in-RAM DCB struct write (no citable, byte-exact
layout found for it). 17 checks: both flags start unregistered; each
function genuinely and independently flips its own flag; calls are
counted; both are idempotent (repeat calls stay safe, matching real
hardware's own "already registered" behavior).

```sh
gcc -I../include -I../source -o test_iop_device_registration tests/test_iop_device_registration.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c ../source/hw/iop_excb.c
./test_iop_device_registration
```

`test_iop_cdvd.c` covers the CDVD (disc drive) register scaffold -
ROADMAP section 7's "CDVD - disc/BIOS-boot-media emulation" item. See
`include/core/hw/iop_cdvd.h` for the full design rationale: register
offsets and power-on defaults are ported directly from PCSX2's own
`pcsx2/CDVD/CDVD.cpp` `cdvdReset()`/`cdvdRead()` (GPL-3.0), matching
the specific case this project currently needs - a diskless BIOS-only
boot. 19 checks: real power-on defaults (STATUS=tray-open,
READY=drive-ready, TYPE=no-disc, INTR_STAT=0); ERROR's real
read-clears-on-read behavior; BREAK always reading 0; NCMD being
latched and triggering a plausible completion IRQ instead of staying
busy forever; the real 4KB-page register mirroring PCSX2's own
`psxHw4Read8/Write8` implements; and out-of-range addresses being
correctly rejected.

```sh
gcc -I../include -o test_iop_cdvd tests/test_iop_cdvd.c ../source/hw/iop_cdvd.c
./test_iop_cdvd
```

`test_ee_cop2_arith3.c` covers Round 29 continued's 16th change -
extending the VADD/VMUL/VSUB row (Round 13's VSUB, Round 29
continued's 10th change's VADD/VMUL) with `VMAX` (funct 0x2B) and
`VMINI` (funct 0x2F), the same 3-operand full-vector shape, ported
from PCSX2's own `VUops.cpp` `_vuMAX`/`_vuMINI` (a plain float max/min
comparison per lane, no NaN/signed-zero special handling). 4 checks:
`VMAX.xyzw` computes the correct per-lane max; `VMINI.xyzw` computes
the correct per-lane min; a single-lane destmask (`VMAX.x`) only
writes that one lane.

```sh
gcc -I../include -I../source -o test_ee_cop2_arith3 tests/test_ee_cop2_arith3.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c -lm
./test_ee_cop2_arith3
```

`test_ee_cop2_arith4.c` covers Round 29 continued's 17th change -
completing the VADD/VMUL/VMAX/VSUB/VMINI SPECIAL1 row (funct
0x28-0x2F) with its three remaining, accumulator-based siblings:
`VMADD` (funct 0x29), `VMSUB` (funct 0x2D), `VOPMSUB` (funct 0x2E).
Confirmed against a real PCSX2 upstream reference clone's
`R5900OpcodeTables.cpp` row (VADD, VMADD, VMUL, VMAX, VSUB, VMSUB,
VOPMSUB, VMINI = funct 0x28..0x2F sequential) and `VUops.cpp`'s
`_vuOpMADD`/`_vuOpMSUB`/`_vuOPMSUB` semantics. VMADD/VMSUB read the
existing VU0 macro-mode accumulator (`vu0_acc[4]`, already wired for
VU microcode) as a third operand: `FD[lane] = ACC[lane] +-
FS[lane]*FT[lane]`. VOPMSUB is the cross-product-shaped outer-product
multiply-subtract, always writing exactly xyz (no destmask field, w
untouched). 5 checks: VMADD.xyzw and VMSUB.xyzw per-lane results
verified against hand-computed values; VMADD.x (single-lane destmask)
only writes that one lane; VOPMSUB's cross-product-shaped result
verified against hand-computed values.

```sh
gcc -I../include -I../source -o test_ee_cop2_arith4 tests/test_ee_cop2_arith4.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c -lm
./test_ee_cop2_arith4
```

`test_ee_cop2_broadcast.c` covers Round 29 continued's 18th change -
the FT-lane-broadcast forms of the already-implemented full-vector
arithmetic row: `VADDx/y/z/w` (funct 0x00-0x03), `VSUBx/y/z/w`
(0x04-0x07), `VMAXx/y/z/w` (0x10-0x13), `VMINIx/y/z/w` (0x14-0x17),
`VMULx/y/z/w` (0x18-0x1B). Confirmed against a real PCSX2 upstream
reference clone's `R5900OpcodeTables.cpp` (SPECIAL1 table's first 4
rows) and `VUops.cpp`'s `applyBinaryMACOpBroadcast`: `FD[lane] =
FS[lane] OP FT.<bc-lane>` for every lane selected by destmask, where
`<bc-lane>` is fixed by the specific opcode (not by destmask). 7
checks: one representative op from each of the 5 families (VADDy,
VSUBx, VMULz, VMAXw, VMINIx) verified against hand-computed broadcast
results; a single-lane destmask (`VADDy.x`) only writes that one
lane. `VMADDx/y/z/w`/`VMSUBx/y/z/w` (ACC-based broadcast) and
`VMULq`/`VMAXi`/`VMULi`/`VMINIi` (Q/I-register broadcast) remain out
of scope - a separate follow-up.

```sh
gcc -I../include -I../source -o test_ee_cop2_broadcast tests/test_ee_cop2_broadcast.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c -lm
./test_ee_cop2_broadcast
```

`test_ee_cop2_broadcast2.c` covers Round 29 continued's 19th change -
completing the funct 0x00-0x1F broadcast row with its last two op
families: `VMADDx/y/z/w`/`VMSUBx/y/z/w` (funct 0x08-0x0F, the
ACC-based broadcast forms - `FD[lane] = ACC[lane] +-
FS[lane]*FT.<bc-lane>`) and `VMULq`/`VMAXi`/`VMULi`/`VMINIi` (funct
0x1C-0x1F, which have no FT operand at all - confirmed against a real
PCSX2 upstream reference clone's `DisR5900asm.cpp` - and instead
broadcast the scalar `Q`/`I` control register, `cop2_ctrl[22]`/
`cop2_ctrl[21]` per PCSX2's `VU.h` `REG_Q`/`REG_I`). 7 checks: `Q`/`I`
set via `CTC2`, `ACC` poked directly (no macro-mode "write ACC"
opcode exists yet); `VMADDy`/`VMSUBx` broadcast-with-accumulator
results verified; `VMULq`/`VMAXi`/`VMULi`/`VMINIi` each verified
against hand-computed values.

```sh
gcc -I../include -I../source -o test_ee_cop2_broadcast2 tests/test_ee_cop2_broadcast2.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c -lm
./test_ee_cop2_broadcast2
```

`test_ee_cop2_unary.c` covers Round 29 continued's 20th change - the
COP2SPECIAL2 unary/data-movement cluster: `VABS` (idx=29),
`VITOF0/4/12/15` (idx=16-19), `VFTOI0/4/12/15` (idx=20-23), `VMOVE`
(idx=48), `VMR32` (idx=49). Confirmed against a real PCSX2 upstream
reference clone that these ops encode the DESTINATION in the FT field
position and the SOURCE in FS (`DisR5900asm.cpp`'s `P_VABS`/etc print
`FT, FS` - the opposite of the arithmetic row's FD/FS/FT roles).
`VITOF`/`VFTOI` are ported bit-exact from PCSX2's `VUops.cpp`
`intToFloat<Offset>`/`floatToInt<Offset>` templates (including
`floatToInt`'s denormal-range saturation), not a plain C cast. 8
checks: `VABS` computes `|VF1|`; `VMOVE` copies unchanged; `VMR32`
rotates lanes (`FT.x=FS.y`, etc); `VITOF4`/`VITOF12` scale raw int32
bit patterns by 2^-4/2^-12; `VFTOI0`/`VFTOI4` truncate floats to int
(optionally pre-scaled); a single-lane destmask (`VABS.x`) only writes
that one lane.

```sh
gcc -I../include -I../source -o test_ee_cop2_unary tests/test_ee_cop2_unary.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c -lm
./test_ee_cop2_unary
```

`test_ee_cop2_acc.c` covers Round 29 continued's 21st change - the
COP2SPECIAL2 accumulator-writing family: every op that writes
`vu0_acc[4]` instead of `VF[fd]`. Full-vector forms `VADDA`(idx40)/
`VMADDA`(41)/`VMULA`(42)/`VSUBA`(44)/`VMSUBA`(45); `VOPMULA`(46, the
outer-product multiply variant of `VOPMSUB` - writes ACC directly, no
existing-ACC read, xyz only); `VNOP`(47, true no-op); representative
broadcast forms `VADDAy`(idx1)/`VMADDAx`/`VMULAq`(idx28, broadcasts
`Q`)/`VSUBAi` (broadcasts `I`). All confirmed against a real PCSX2
upstream reference clone's `VUops.cpp` (`applyBinaryMACOp`/
`applyTernaryMACOp` and their `Broadcast` variants templated on
`MACOpDst::Acc`). 7 checks across 6 independent fresh-core sub-tests
(so `ACC` always starts at a known zeroed state): `VADDA` computes
`ACC=VF1+VF2`; `VMULA` seeds `ACC`, then `VMADDA` accumulates onto it
(round-trips through the real ACC read-modify-write); `VSUBA`
computes `ACC=VF1-VF2`; `VOPMULA` overwrites a sentinel ACC value with
the cross-product-shaped outer product; `VMULAq` broadcasts `Q`, and
the following `VNOP` provably changes nothing; `VADDAy` broadcasts a
single FT lane.

```sh
gcc -I../include -I../source -o test_ee_cop2_acc tests/test_ee_cop2_acc.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c -lm
./test_ee_cop2_acc
```

`test_ee_cop2_lqisqd.c` covers Round 29 continued's 22nd change - the
remaining VU0 local-memory access family: `VLQI` (idx52, load-
quadword-post-increment), `VLQD` (idx54, load-quadword-pre-decrement),
`VSQD` (idx55, store-quadword-pre-decrement). Also verifies a real bug
found and fixed alongside this change: `VSQI` (idx53) was
unconditionally storing all 4 lanes regardless of destmask - confirmed
via a real PCSX2 upstream reference clone's `DisR5900asm.cpp` that
`VSQI` genuinely has an xyzw suffix like every other CO-format op.
Field-role convention (ported from PCSX2's `VUops.cpp` `_vuLQI`/
`_vuLQD`/`_vuSQD`): for loads, the address VI register lives in the FS
field position and the destination VF register lives in FT - the
opposite of VSQI/VSQD, where the address lives in FT and the source
VF register lives in FS. 8 checks: a full VSQI store followed by a
single-lane VSQI store (proving the destmask fix - the untouched lanes
stay 0) and confirming VI10 was post-incremented twice; `VLQI` reads
the full store back and post-increments its own address register;
`VLQD` pre-decrements and reads back the single-lane store exactly
(X=2, Y=0); `VSQD` pre-decrements and its store round-trips correctly
through a follow-up `VLQI`.

```sh
gcc -I../include -I../source -o test_ee_cop2_lqisqd tests/test_ee_cop2_lqisqd.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c -lm
./test_ee_cop2_lqisqd
```

`test_ee_cop2_mtir.c` covers Round 29 continued's 23rd change - the
integer<->float raw-bit-move family: `VMTIR` (idx60), `VMFIR`
(idx61), `VILWR` (idx62), ported from a real PCSX2 upstream reference
clone's `VUops.cpp` `_vuMTIR`/`_vuMFIR`/`_vuILWR`. `VMTIR` truncates
the raw 32-bit bit pattern of `VF[fs][Fsf]` to its low 16 bits into
`VI[ft]` - `Fsf` is not a separate field, confirmed via
`DisR5900asm.cpp`'s `dest_fsf()` macro to live in the same two bits
as this decoder's `destmask` value's low 2 bits, just reinterpreted
as a lane index. `VMFIR` broadcasts the sign-extended 16-bit `VI[fs]`
value (raw bits, not a float conversion) into destmask-selected
`VF[ft]` lanes. `VILWR` reads the low 16 bits of VU0 mem at quadword
index `VI[fs]` into `VI[ft]`, single lane selected by destmask (same
convention as `VISWR`). 4 checks: `VMTIR` truncates a planted raw bit
pattern; `VMFIR.xz` broadcasts a sign-extended negative 16-bit value
into exactly the X and Z lanes, leaving Y untouched; `VILWR.z` reads
back a planted 16-bit value from a specific VU0 mem lane.

```sh
gcc -I../include -I../source -o test_ee_cop2_mtir tests/test_ee_cop2_mtir.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c -lm
./test_ee_cop2_mtir
```

`test_ee_cop2_rreg.c` covers Round 29 continued's 24th change - the
VU0 "R register" LFSR pseudo-random generator: `VRINIT` (idx66),
`VRGET` (idx65), `VRNEXT` (idx64), `VRXOR` (idx67), ported bit-exact
from a real PCSX2 upstream reference clone's `VUops.cpp`
`_vuRINIT`/`_vuRGET`/`AdvanceLFSR`/`_vuRNEXT`/`_vuRXOR`. `R` is
control register index 20 - no new state needed, already reachable
via the existing `vu0_vi_read`/`write` helpers - and is always kept
in the float-bit-pattern range `[1.0,2.0)` (exponent/sign fixed at
`0x3F800000`, only the low 23 mantissa bits vary). 5 checks (against
a host-side reference `AdvanceLFSR` model, not hand-derived bit
patterns): `VRINIT` seeds `R` and `VRGET` reads it back unchanged;
`VRNEXT` advances the LFSR once and broadcasts the new value into all
4 lanes; a second `VRNEXT` advances again (proving it's not
idempotent); `VRXOR` XORs `R`'s mantissa with a VF lane's raw bits and
re-clamps to the `[1.0,2.0)` pattern.

```sh
gcc -I../include -I../source -o test_ee_cop2_rreg tests/test_ee_cop2_rreg.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c -lm
./test_ee_cop2_rreg
```

`test_ee_cop2_div.c` covers Round 29 continued's 25th change - `VDIV`
(idx56), `VSQRT` (idx57), `VRSQRT` (idx58), `VWAITQ` (idx59), the
division/sqrt family that produces the Q register value (this
project's `cop2_ctrl[22]`), ported from a real PCSX2 upstream
reference clone's `VUops.cpp` `_vuDIV`/`_vuSQRT`/`_vuRSQRT`/
`_vuWAITQ`. `Fsf`/`Ftf` are independent 2-bit lane selectors living in
destmask's low/high 2 bits respectively; `VSQRT` uses only `Ftf` (no
FS operand at all). Divide-by-zero produces a signed `FLT_MAX` bit
pattern rather than a real IEEE infinity, matching real PS2 hardware.
Researching this confirmed `VWAITQ` is a true no-op even in PCSX2
itself (no latency modeled), resolving this project's own earlier-
documented concern about needing to model the Q register's "busy"
timing - none is needed. 6 checks: `VDIV` computes a normal division
and also the `0/0` divide-by-zero clamp; `VSQRT` computes `sqrt(|x|)`;
`VRSQRT` computes a normal `fs/sqrt(ft)` and also the `ft=0,fs!=0`
clamp; `VWAITQ` provably changes nothing.

```sh
gcc -I../include -I../source -o test_ee_cop2_div tests/test_ee_cop2_div.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c -lm
./test_ee_cop2_div
```

`test_ee_cop2_clip.c` covers Round 29 continued's 26th change -
`VCLIPw` (idx31), the last remaining VU0 macro-mode gap identified
this session. Judges `|VF[fs].x|`, `|VF[fs].y|`, `|VF[fs].z|` against
`|VF[ft].w|` via a raw 32-bit signed-integer sign-flip XOR trick (not
a float comparison), ported bit-exact from a real PCSX2 upstream
reference clone's `VUops.cpp` `_vuCLIP`. Unlike every other VU0 op
this session, `VCLIPw` needed genuinely new reachable state - the
CLIP flag register - resolved by reusing control-register slot 18
(`REG_CLIP_FLAG` in PCSX2's `VU.h`), which this decoder's generic
`CFC2`/`MTC2`/`QMTC2` paths already handle for any register index, so
no new field was required. 3 checks: a `VCLIPw` call producing a
known 6-bit judgment pattern, and a second call proving the 6-bit
shift-in history behavior (`clipflag = (old << 6) | new_bits`).

```sh
gcc -I../include -I../source -o test_ee_cop2_clip tests/test_ee_cop2_clip.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/vif.c ../source/hw/vu.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/mch.c -lm
./test_ee_cop2_clip
```

`test_iop_loadcore_panic_bypass.c` covers Round 29 continued's 28th
change - `is_loadcore_panic_loop()` in `iop_module_loader.c` (task
#124/#132/#148, see docs/STATUS.md's 27th/28th findings). Real
LOADCORE module-loader code reaches a genuine, deliberate real-BIOS
panic sequence (`lui $v1,0x8000; addiu $v0,zero,2; sb $v0,($v1);
j <self>`) when its own internal multi-phase module/library self-
registration list turns up empty - a real gap this project cannot
safely fabricate a fix for (the real dispatch that WOULD read
registration entries calls through a genuine `jalr`, so a wrong guess
doesn't fail safely). Instead, this recognizes the exact panic
instruction sequence by its literal encoded bytes and treats reaching
it exactly like a module returning through this loader's own
trampoline: advances to the next module in the real IOPBTCONF list.
9 checks: the exact real byte signature is recognized; two negative
controls (wrong base register in the `sb`; a jump that doesn't loop
back to the `sb` instruction) are correctly rejected, proving the
match isn't overbroad; and the actual interpreter-facing entry point
(`iop_module_loader_try_handle()`) advances to the next module without
halting when the signature is reached.

```sh
gcc -I../include -I../source -o test_iop_loadcore_panic_bypass tests/test_iop_loadcore_panic_bypass.c ../source/core/iop/iop_core.c ../source/hw/iop_elf.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_hle_modules.c ../source/hw/iop_excb.c ../source/hw/iop_cdvd.c ../source/hw/iop_spu2.c
./test_iop_loadcore_panic_bypass
```

`test_iop_tge.c` covers `iop_core.c`'s TGE (Trap if Greater or Equal,
SPECIAL funct 0x30) implementation, added task #150. Found as an
"unimplemented SPECIAL funct 0x30" halt at `pc=0x800000AC` after task
#149's syscall-return fix let a second real syscall fall through the
still-unclaimed general exception vector and re-walk that region down
a different path, reaching a genuine TGE instruction. Real MIPS trap
semantics: if signed `rs >= rt`, raises a Trap exception
(`Cause.ExcCode=13`, pre-shifted to bits 2-6 as `0x34`), `EPC` set to
the TGE's own address, PC vectors per `Status.BEV` - mirrors this
file's existing SYSCALL exception delivery exactly, just a different
ExcCode/trigger. If the condition is false, TGE is a pure no-op (no
exception, no delay slot, no side effect at all). 13 checks across
both outcomes: trap-taken path verifies Cause/EPC/PC vectoring;
trap-not-taken path verifies Cause/EPC are completely untouched and
execution falls through normally to a following marker instruction.

Real-BIOS follow-up (see docs/STATUS.md's 30th finding): with TGE
implemented, this specific halt is gone, but host-native testing
against the actual SCPH-10000 BIOS shows the IOP does not make
further real boot progress either - it settles into a tight,
non-halting loop cycling through roughly 11 instructions in the
`0x80000080`-`0x800000A8` range forever (a real syscall re-issued,
returning the same stub `0` via task #149's fix, apparently not
satisfying whatever condition the calling code is polling for, so it
retries indefinitely). This is the same class of finding as
#124/#132's LOADCORE registration list: an honest architectural stop,
not a crash, and not further pursued this round since it would
require implementing whatever real kernel service this repeated
syscall actually expects.

```sh
gcc -I../include -I../source -o test_iop_tge tests/test_iop_tge.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_cdvd.c ../source/hw/iop_hle_modules.c ../source/hw/iop_excb.c ../source/hw/iop_module_loader.c ../source/hw/iop_elf.c ../source/hw/iop_spu2.c
./test_iop_tge
```

`test_iop_trap_stub_bypass.c` covers Round 29 continued's 32nd change
- `is_unconditional_trap_stub()` in `iop_module_loader.c` (task
#151/#152, see docs/STATUS.md's 29th/30th/31st findings). Real
syscalls from later-loaded modules (first observed: INTRMANP calling
ExitCriticalSection) fall through to the still-unclaimed general
exception vector, which LOADCORE's own real init installs with a
ten-instruction real prologue ending in an unconditional TGE (Trap if
Greater or Equal, rs==rt so it always traps) - the same underlying
architectural gap as the LOADCORE panic loop (task #124/#132/#148),
just reached through a real syscall path instead of a direct
self-jump, and previously causing an infinite, non-halting recursion
(confirmed via repeated single-step sampling showing zero state
change cycle after cycle). This project cannot safely fabricate a
real registration entry to fix the underlying gap (same reasoning as
the panic-loop bypass), so instead it recognizes the exact
byte-for-byte prologue plus the STRUCTURAL shape of "always traps"
(not one hardcoded trap "code" value - the same stub template was
observed reused nearby with a different code field) and treats
reaching it exactly like the panic-loop bypass: advances to the next
module in the boot list. 10 checks: the exact signature is recognized
(using a deliberately different register/code choice than the real
BIOS's own encoding, proving the match is structural); three negative
controls (a near-miss in the prologue's base register; a CONDITIONAL
trap, rs != rt; a different SPECIAL funct, TEQ, at the same position)
are all correctly rejected; and the actual interpreter-facing entry
point (`iop_module_loader_try_handle()`) advances to the next module
without halting when the signature is reached.

Real-BIOS result (see docs/STATUS.md's 32nd finding): this bypass,
combined with the 31st change's front-loading refactor, took the real
SCPH-10000 boot from getting permanently stuck after ~3 modules to
successfully loading all 29/29 real IOPBTCONF modules, resolving
355/355 imports, running 15 of them to full completion, and safely
bypassing 14 dead-end recursions (1 via the original panic-loop
bypass, 13 via this new one) - the boot sequence now reaches its own
natural, honest end-of-list halt instead of spinning forever.

```sh
gcc -I../include -I../source -o test_iop_trap_stub_bypass tests/test_iop_trap_stub_bypass.c ../source/core/iop/iop_core.c ../source/hw/iop_elf.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_hle_modules.c ../source/hw/iop_excb.c ../source/hw/iop_cdvd.c ../source/hw/iop_spu2.c
./test_iop_trap_stub_bypass
```

`test_iop_module_loader_bootinfo.c` was updated for task #151/#155:
boot_info[0x18]/[0x1C] are no longer honest-zero placeholders - see
`build_real_registration_list()`'s header comment in
`iop_module_loader.c` and docs/STATUS.md's 35th finding. The test now
checks the real word-count-minus-one/pointer-array format this round
reverse-engineered from a live PCSX2 reference debugger, instead of
asserting these offsets stay zero.

`test_iop_registration_walk_panic_bypass.c` covers task #157:
`is_registration_walk_panic_loop()`, a THIRD distinct real panic-tail
detector (see docs/STATUS.md's 36th finding). Once task #155's real
registration list is in place, live real-BIOS testing showed LOADCORE
genuinely walks the real entries (no immediate rejection - directional
confirmation of the 34th/35th findings' format understanding) but
still lands in a second, different "write a status byte, then spin
forever" idiom reached from a different real call site than the
original panic loop (task #148) - only the tail 3 words repeat here
(`sb $v0,($v1)` / `j <self>` / nop), without the original's own inline
`lui $v1,0x8000`/`addiu $v0,zero,2` setup, so the two detectors are
correctly non-overlapping (verified by an explicit check that
`is_loadcore_panic_loop()` does NOT fire on this new tail). 10 checks:
recognition of the real signature; three negative controls (wrong SB
base register; jump to a different address; non-nop delay slot); the
distinctness check against the original detector; and confirmation
that `iop_module_loader_try_handle()` advances to the next module
without halting.

Real-BIOS result (see docs/STATUS.md's 36th finding): net effect on
the actual task #151 goal is neutral, honestly - SIFCMD and SIFINIT
still hit the exact same (now differently-named) dead end as before;
modules_run_to_completion is back to 15 (matching the pre-task-155
milestone, since without this bypass it had regressed to 1). One
incidental, unexplained difference was observed: SIF_MSFLG now reads
0x00010000 instead of 0x0, though SIF_MSCOM/SIF_SMCOM/SIF_SMFLG remain
0 and the EE remains in its known SIF-polling steady state.

```sh
gcc -I../include -I../source -o test_iop_registration_walk_panic_bypass tests/test_iop_registration_walk_panic_bypass.c ../source/core/iop/iop_core.c ../source/hw/iop_elf.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_hle_modules.c ../source/hw/iop_excb.c ../source/hw/iop_cdvd.c ../source/hw/iop_spu2.c
./test_iop_registration_walk_panic_bypass
```

`test_iop_rfe.c` was updated for task #156: `iop_core.c`'s BREAK
"unimplemented syscall fallback" heuristic (task #149, the 29th
change) now also requires `st->exception_pending` (see `iop_core.h`'s
field comment) to be set, not just `Cause.ExcCode==8` alone - fixing a
real, reproducible infinite loop this project's own regression testing
found: any BREAK reached after an RFE-terminated syscall handler,
where Cause still happened to read 8 from the OLD, already-handled
exception (RFE never touches Cause - only Status), previously
mis-fired this fallback and resumed at the stale old EPC+4 instead of
halting. `exception_pending` is set at every real exception-entry
site (hardware interrupt, SYSCALL, TGE) and cleared by RFE, precisely
capturing "has this Cause value been acknowledged yet". `test_iop_rfe`
now additionally verifies the SYSCALL-then-RFE-then-BREAK sequence
halts cleanly on BREAK instead of hanging; `test_iop_syscall.c` (the
task #149 scenario: BREAK immediately after an UNHANDLED syscall, no
intervening RFE) continues to verify the fallback still correctly
fires in that case.
