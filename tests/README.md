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
LWL/LWR yet). Needs `sif.c` linked too now that `iop_mem_read32/
write32` route the IOP-side SIF mirror window (0x1D000000-0x1D0000FF)
through it. Run it the same way:

```sh
gcc -I../include -I../source -o test_iop tests/test_iop_core.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_hle_modules.c
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
Self-contained (`#include`s both `gs_mem.c` and `gif.c` directly, no
extra link deps - a stale earlier version of this doc incorrectly
listed `../source/hw/gs_mem.c` as an extra link argument, which
actually fails with duplicate-symbol linker errors since it's already
pulled in via #include; fixed here):

```sh
gcc -I../include -I../source -o test_gif tests/test_gif.c
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
gcc -I../include -I../source -o test_system_handshake tests/test_system_handshake.c ../source/core/ee/ee_core.c ../source/core/iop/iop_core.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_hle_modules.c
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
gcc -I../include -I../source -o test_iop_hle_bios tests/test_iop_hle_bios.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_hle_modules.c
./test_iop_hle_bios
```

Note: iop_core.c now depends on iop_hle_bios.c too (iop_step() calls
it before fetching/decoding any real instruction), so
test_iop_core.c and test_system_handshake.c both need it linked in -
see the updated commands above.

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
gcc -I../include -I../source -o test_ee_cop0_special tests/test_ee_cop0_special.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
just something to know before writing the assertion. 5/5 checks pass,
including confirming SYSCALL has no delay slot (a NOP placed right
after it never executes) and that BEV correctly selects the
bootstrap vector. Needs the same link set as `test_iop_core.c`:

```sh
gcc -I../include -I../source -o test_iop_syscall tests/test_iop_syscall.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_hle_modules.c
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
gcc -I../include -I../source -o test_iop_hle_exception_install tests/test_iop_hle_exception_install.c ../source/hw/sif.c ../source/hw/iop_intc.c ../source/hw/iop_dma.c ../source/hw/iop_timers.c ../source/hw/iop_hle_bios.c ../source/hw/iop_hle_modules.c
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
gcc -I../include -I../source -o test_ee_lqsq tests/test_ee_lqsq.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
gcc -I../include -I../source -o test_ee_fpu2 tests/test_ee_fpu2.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
gcc -I../include -I../source -o test_ee_fpu3 tests/test_ee_fpu3.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
gcc -I../include -I../source -o test_ee_mmi_compare tests/test_ee_mmi_compare.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
gcc -I../include -I../source -o test_ee_mmi_sat tests/test_ee_mmi_sat.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
gcc -I../include -I../source -o test_ee_mmi_permute tests/test_ee_mmi_permute.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
gcc -I../include -I../source -o test_ee_mmi_pvshift tests/test_ee_mmi_pvshift.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
gcc -I../include -I../source -o test_ee_cop0_prid tests/test_ee_cop0_prid.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
gcc -I../include -I../source -o test_ee_cop0_tlb tests/test_ee_cop0_tlb.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
gcc -I../include -I../source -o test_ee_exceptions tests/test_ee_exceptions.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
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
gcc -I../include -I../source -o test_ee_scratchpad_count tests/test_ee_scratchpad_count.c ../source/hw/dma.c ../source/hw/gs.c ../source/hw/gif.c ../source/hw/gs_mem.c ../source/hw/sif.c
./test_ee_scratchpad_count
```
