#!/usr/bin/env python3
"""
Round 508: build a minimal bootable PS2 disc image (ISO9660 + SYSTEM.CNF + BOOT2 ELF)
for testing real OSDSYS disc-browser boot behavior.

Requires: pycdlib (pip install pycdlib)
Requires: a BOOT.ELF in the same directory (e.g. the Round 507 diagnostic ELF,
tools/round507-diag-elf/diag.elf, copied/renamed to BOOT.ELF).

Usage:
    python3 build_iso.py

Produces round508_test.iso in the current directory. This ISO is NOT committed
to the tracked repo (see leak-prevention note in README.md) - re-run this
script to regenerate it locally.
"""
import io
import os
import pycdlib

HERE = os.path.dirname(os.path.abspath(__file__))
SYSTEM_CNF = os.path.join(HERE, 'SYSTEM.CNF')
BOOT_ELF = os.path.join(HERE, 'BOOT.ELF')
OUT_ISO = os.path.join(HERE, 'round508_test.iso')

iso = pycdlib.PyCdlib()
# interchange level 1 = strict 8.3 ISO9660 filenames (matches SYSTEM.CNF/BOOT.ELF above)
iso.new(interchange_level=1, sys_ident='', vol_ident='ROUND508TEST', app_ident_str='PCSX2WII')

with open(SYSTEM_CNF, 'rb') as f:
    scnf = f.read()
with open(BOOT_ELF, 'rb') as f:
    boot_elf = f.read()

iso.add_fp(io.BytesIO(scnf), len(scnf), '/SYSTEM.CNF;1')
iso.add_fp(io.BytesIO(boot_elf), len(boot_elf), '/BOOT.ELF;1')

iso.write(OUT_ISO)
iso.close()
print(f"ISO built: {OUT_ISO}")
