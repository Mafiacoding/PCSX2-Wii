# Round 507 diagnostic ELF

A small real PS2 homebrew ELF (built with ps2sdk / ps2dev) that boots under
real PCSX2 (via System > Start File...) and dumps genuine BIOS/kernel ground
truth to the on-screen debug console (`scr_printf`), one screen at a time:

- argc/argv (confirms real `host:` filesystem passthrough path)
- full `rom0:ROMVER` string (via `open()`/`read()`)
- `rom0:` directory listing (via `opendir()`/`readdir()`)
- `mc0:/` and `mc1:/` directory listings
- `sceCdGetDiskType()` / `sceCdStatus()`

Rationale: PCSX2's own OSD/BIOS render viewport has a persistent render-gap
bug (see Round 505/506 in docs/STATUS.md) that truncates long on-screen
strings (like uLaunchELF's Debug Info dump) at a fixed pixel width regardless
of aspect ratio. Printing our own text via ps2sdk's `scr_printf` console and
pacing it across multiple screens works around that, and - more importantly -
lets us query anything a real PS2 homebrew ELF can legitimately query via
kernel syscalls: this is real ground truth from the reference BIOS/emulator,
not inference from disassembly.

## Build

Requires the ps2dev toolchain (ee-gcc / ps2sdk). With `PS2DEV`/`PS2SDK`/`PATH`
set per the standard ps2dev README:

```
make
```

Produces `diag.elf`. Strip with `mips64r5900el-ps2-elf-strip --strip-all
diag.elf` before loading.

## Run

In real PCSX2: System > Start File... > select `diag.elf`. Screens auto-
advance (paced with `sleep()`); use System > Reset to rerun from the top.

## Round 507 findings (JP BIOS, ps2-0100J)

- `rom0:ROMVER == "0100JC20000117"` (len=16) - full string, uncut (vs the
  render-gap-truncated `"0100JC200..."` uLaunchELF showed in Round 506).
- `argv[0] == "host:C:\Users\box\pcsx2wii-round507-diag.elf"` - confirms real
  `host:` passthrough resolves to the actual Windows path we placed the ELF at.
- `opendir("rom0:")` + `readdir()` returns 0 entries, even though named opens
  like `rom0:ROMVER` succeed. Real behavioral finding: this ps2sdk/PCSX2
  combination's `rom0:` device does not support directory enumeration via the
  POSIX dirent API, only direct named-file open. Relevant scope note for our
  own emulator's rom0: implementation (Round 346) - it should not be expected
  to serve a rom0: directory listing either.
- `opendir("mc1:/")` fails ("opendir failed") - consistent with Round 456's
  finding of no memory card present in slot 2.
- `sceCdGetDiskType() = 0 (0x0)`, `sceCdStatus() = 10 (0xa)` - real "no disc
  mounted" status from live PCSX2 (no ISO loaded in this session).
