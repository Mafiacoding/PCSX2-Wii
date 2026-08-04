# Sandbox-reset recovery procedure

This sandbox's `/tmp` is ephemeral and gets wiped periodically (a
recurring, previously-documented environmental limitation - see
docs/STATUS.md's various "Sandbox-reset recovery note" entries). This
file exists so any future round (or a fresh session) can get back to
a fully working state in a few commands, without having to
re-discover any of this from scratch.

**Nothing in this file is or references actual BIOS ROM bytes or disc
image contents** - only filenames, paths, and reconstruction commands,
per this project's standing leak-prevention rule (never commit, push,
or rsync BIOS/disc bytes).

## 1. The git repository itself

The authoritative, persistent copy of this repository's history lives
in the connected outputs folder's `pcsx2-wii/.git/` directory (this is
a real user-visible, persistent mount - NOT `/tmp`). After every round
that makes a real commit, the working steps are:

```sh
cp docs/STATUS.md docs/ROADMAP.md <outputs>/pcsx2-wii/docs/
rsync -a <tracked changed files> <outputs>/pcsx2-wii/<same paths>
rsync -a .git/ <outputs>/pcsx2-wii/.git/
```

To recover a fresh `/tmp` working clone after a reset:

```sh
mkdir -p /tmp/pcsx2-wii-git
rsync -a <outputs>/pcsx2-wii/.git/ /tmp/pcsx2-wii-git/.git/
cd /tmp/pcsx2-wii-git && git checkout -- .
```

## 2. devkitPPC/libogc Wii cross-compile toolchain

Already fully set up and persisted at
`<outputs>/build/devkitpro/` and `<outputs>/build/libogc-src/` - see
that directory's own `TOOLCHAIN_SETUP_NOTES.md` for the full history
of how it got there. Required every session (this does NOT survive a
`/tmp` reset since it only sets shell env vars, but the actual
toolchain FILES do survive, since they live under `<outputs>/`, not
`/tmp`):

```sh
export DEVKITPRO=<outputs>/build/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
export PATH=$DEVKITPPC/bin:$PATH
export LD_LIBRARY_PATH=$DEVKITPPC/lib:$LD_LIBRARY_PATH
cd /tmp/pcsx2-wii-git && make clean && make
```

Verified working as of Round 441 (clean rebuild, 0 warnings, produces
`pcsx2-wii-git.elf`/`.dol`).

## 3. Real BIOS + real disc test fixtures

The host-native scratch boot-trace driver (`driver_r313.c`, see its
own header comment) hardcodes two paths:

- `/tmp/round238_diag/bios.bin` - the real SCPH-10000 BIOS dump
- `/tmp/round238_diag/disc.iso` - the real Tekken Tag Tournament
  (Europe) (Demo) disc image

Both are reconstructible from the user's own original uploads (in the
connected uploads folder, read-only, persists across resets same as
any other connected folder):

```sh
mkdir -p /tmp/round238_diag
unzip -o -q "<uploads>/PS2_BIOS (1).zip" ps2-0100j-20000117.bin -d /tmp/round238_diag
mv /tmp/round238_diag/ps2-0100j-20000117.bin /tmp/round238_diag/bios.bin
cp "<uploads>/Tekken Tag Tournament (Europe) (Demo).bin" /tmp/round238_diag/disc.iso
```

Verified this round (Round 442): both reconstructed files are
byte-identical (SHA1 match) to the fixtures already in use.

## 4. Rebuilding the host-native scratch boot driver

```sh
cd /tmp/pcsx2-wii-git
SRCS=$(find source/core source/hw -name '*.c' ! -path '*/recompiler/*'; echo source/core/ee_elf_loader.c source/core/bios_loader.c source/core/system.c source/core/iso_loader.c)
SRCS=$(echo "$SRCS" | tr ' ' '\n' | sort -u | tr '\n' ' ')
gcc -O2 -g -no-pie -Iinclude -o /tmp/driver_r313 driver_r313.c $SRCS -lm
/tmp/driver_r313 run /tmp/ckpt.bin 45000000
```

(`resume` mode has a known, pre-existing, scratch-tool-only same-
binary checkpoint/resume crash - see docs/STATUS.md Round 307/312's
findings. Use fresh `run` invocations instead of chained `resume`s.)

## 5. Full regression suite

```sh
python3 /tmp/r441_tests/run_batch.py 0 40   # etc, in ~20-test batches
```

(the generic runner script itself is scratch-only and not persisted -
see this file's own header comment in `/tmp/r441_tests/run_batch.py`
if it still exists, or recreate it: for each `tests/test_*.c`, link
against every `source/**/*.c` file EXCEPT `source/main.c`,
`source/core/recompiler/*`, and any `.c` file the test itself
`#include`s inline.)
