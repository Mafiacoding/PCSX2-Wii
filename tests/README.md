# Tests

`test_ee_core.c` is a host-native unit test for `ee_core.c` (compiled
with your regular host `gcc`, not devkitPPC - it's for fast iteration
on interpreter correctness, not part of the Wii build/Makefile).

It hand-encodes a tiny MIPS/EE instruction sequence, runs it through
the interpreter, and checks register results against known-correct
values (cross-checked against PCSX2's own semantics for the opcodes
covered). Run it with:

```sh
gcc -I../include -I../source -o test_ee tests/test_ee_core.c
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
gcc -I../include -I../source -o test_ee_dma tests/test_ee_dma_bus.c ../source/hw/dma.c
./test_ee_dma
```
