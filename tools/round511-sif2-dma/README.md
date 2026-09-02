# Round 511: real SIF2 (IOP DMA channel 2) inbound transfer engine

## Context

Round 510 falsified the "mid-run pad press" hypothesis and pointed the
diagnosis back at task #447 - the real blocker for OSDSYS's shared
per-frame idle-dispatch loop (`0x8000F768`-family) is that it waits on
`DMAC_STAT` bit 0x80 (SIF2 completion, channel 7 on the EE side) OR
`INTC_STAT` bit 0x2 (SBUS), and this project's own prior investigation
(Round 267) found "SIF2's DPCR enable bit is set... but
`dma_channel_kick()` is never called for it."

Per the user's explicit instruction ("do first 2 and after that 1" -
implement the SIF DMA inbound payload-copy path, then the persistent
IOP threading/scheduler), this round implements the first of those.

## Real source grounding

The user's own previously-uploaded, real 2002-era IOP SIFMAN
reimplementation source settles exactly which IOP DMA channel SIF2
uses and its real transfer semantics:

- `dmacman.h`: `#define DMAch_GPU 2 // SIF2 both directions` - IOP DMA
  channel 2 (PS1-legacy "GPU", base `0x1F8010A0`) is real hardware's
  dual-purpose SIF2 channel on PS2 - exactly this project's own
  existing `s_ranges[]` channel-2 mapping in `iop_dma.c`, previously
  just a plain register latch.
- `sifman.c`'s `sceSifSetSIF2DMA()`: the real kicker - sets MADR (source,
  masked to `0xFFFFFF`), BCR_size/BCR_count (low/high halfwords - the
  same BS*BA convention this project's existing SIF0 transfer function
  already used, independently confirmed matching), then CHCR with
  `DMAf_TR` (`0x01000000`, the same STR/start bit SIF0 already gates
  on) OR'd with `DMAf_DR` (`0x00000001`) when-and-only-when the caller
  requests `SIF_TO_EE` - i.e. DR=1 is the real, cited signal for the
  IOP-RAM-to-EE-RAM direction.
- `sifman_sceSifGPUInit()` (called from `sceSifInit()`, real IOP SIFMAN
  init): resets `DMAch_SIF2_CHCR` and enables DMA channel 2 via DPCR
  bit `0x800` - matches this project's own existing DPCR-enable
  observation from Round 267.

## Implementation

`source/hw/iop_dma.c`: new `iop_dma_sif2_try_transfer()`, a direct
sibling of the existing `iop_dma_sif0_try_transfer()` (Round 199),
wired into `iop_dma_mmio_write32()`'s CHCR write path for channel 2,
triggered only when both `DMAf_TR` and `DMAf_DR` are set in the
written value (the real, cited IOP-RAM-to-EE-RAM direction). Reuses
the existing `dma_channel_receive_quadwords(DMA_CHANNEL_SIF2, ...)`
inbound-write primitive (Round 198) - no new architecture, a
narrowly-scoped additive wire-up. The reverse direction (TR set, DR
clear - EE-to-IOP) is intentionally left as a plain register latch,
matching Round 199's own documented SIF1 scope limitation - no
citable source yet for what IOP-side buffer that direction should
target, so nothing is fabricated for it.

A hit-counter diagnostic, `iop_dma_get_sif2_transfer_count()`, was
added following this project's established convention, to let a boot
survey empirically confirm whether real guest IOP code ever actually
issues this kick.

## Result

`driver.c` runs a long organic BIOS boot (no disc, no trampoline) and
reports `DMAC_STAT`, the new SIF2 transfer counter, and EE PC at
sampled intervals. Across two independent runs totaling ~145,000,000
instructions, `iop_dma_get_sif2_transfer_count()` stayed at 0
throughout, and `DMAC_STAT` bit 0x80 was never set. `DMAC_STAT`'s
enable bits (`0x00a00000`) confirm both SIF0 (bit 21) and SIF2 (bit
23) are enabled - matching Round 267's finding - but no CHCR write
with both `DMAf_TR` and `DMAf_DR` set was ever observed in this trace
window. The EE's PC in this organic (no-disc) boot mode rests in a
different region (`0x00501xxx`-`0x00518xxx`, not the previously
well-documented `0x8000Cxxx` shared kernel loop family, which prior
rounds mostly reached via disc-mounted or trampoline paths) - a
resting point this round did not further characterize.

## Interpretation

The fix itself is correct and real-source-grounded, but **alone it is
not sufficient** to unblock OSDSYS's SIF2 wait condition, because no
guest code path in the currently-modeled boot trace ever issues the
real `sceSifSetSIF2DMA(SIF_TO_EE)` kick in the first place - this was
the honestly-anticipated outcome going in, not a surprise. This
directly motivates Round 512's persistent-IOP-threading effort: the
code that would call `sceSifSetSIF2DMA()` most plausibly lives inside
one of the loaded IOP modules' own ongoing service logic (SIFMAN or a
higher-level module reacting to an EE request), which this project's
IOP core currently never reaches because it halts once it finishes
running the fixed set of modules discovered at boot (task #92's
documented simplification) rather than staying alive as an
event-driven scheduler.

## Classification

Real, additive, citation-grounded fix - kept in tracked source
regardless of the negative result, since it closes a genuine, evidenced
gap in the DMA model (matching real hardware behavior for the direction
it's needed) and is exactly the kind of building block Round 512's
IOP-threading work will need once real guest code can reach the SIFMAN
service logic that would actually call it.
