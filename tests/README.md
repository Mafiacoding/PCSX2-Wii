# Tests

`test_ee_core.c` is a host-native unit test for `ee_core.c` (compiled
with your regular host `gcc`, not devkitPPC - it's for fast iteration
on interpreter correctness, not part of the Wii build/Makefile).

It hand-encodes a tiny MIPS/EE instruction sequence, runs it through
the interpreter, and checks register results against known-correct
values (cross-checked against PCSX2's own semantics for the opcodes
covered). Run it with:

```sh
gcc -I../include -I../source -o test_ee tests/test_ee_core.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
LWL/LWR yet). Run it the same way:

```sh
gcc -I../include -I../source -o test_iop tests/test_iop_core.c
./test_iop
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
gcc -I../include -I../source -o test_ee_dma tests/test_ee_dma_bus.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
gcc -I../include -I../source -o test_ee_unaligned tests/test_ee_unaligned.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
gcc -I../include -I../source -o test_ee_fpu tests/test_ee_fpu.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
Needs gs_mem.c linked too:

```sh
gcc -I../include -I../source -o test_gif tests/test_gif.c ../source/hw/gs_mem.c
./test_gif
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
