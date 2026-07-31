#!/usr/bin/env python3
"""Minimal ELF32 (big-endian, PowerPC) -> DOL converter.

Standalone build-tool replacement for devkitPro's elf2dol, since the
base_tools bundle in the uploaded devkitPPC tarball was empty. DOL is
the executable format used by GameCube/Wii loaders (HBC, Dolphin).
Implements the well-documented DOL header layout: 7 text + 11 data
section slots (offset/addr/size arrays), one bss region, and an entry
point, all big-endian uint32.
"""
import struct
import sys

SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHT_NOBITS = 8

def read_elf(path):
    with open(path, 'rb') as f:
        data = f.read()

    assert data[0:4] == b'\x7fELF', "not an ELF file"
    ei_class = data[4]
    ei_data = data[5]
    assert ei_class == 1, "expected ELF32"
    assert ei_data == 2, "expected big-endian ELF (PowerPC)"

    (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags,
     e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from('>HHIIIIIHHHHHH', data, 16)

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
         sh_link, sh_info, sh_addralign, sh_entsize) = struct.unpack_from(
            '>IIIIIIIIII', data, off)
        sections.append({
            'type': sh_type, 'flags': sh_flags, 'addr': sh_addr,
            'offset': sh_offset, 'size': sh_size,
        })

    return data, e_entry, sections


def convert(elf_path, dol_path):
    data, entry, sections = read_elf(elf_path)

    text_secs = []
    data_secs = []
    bss_start = None
    bss_end = None

    for s in sections:
        if not (s['flags'] & SHF_ALLOC) or s['size'] == 0:
            continue
        if s['type'] == SHT_NOBITS:
            start = s['addr']
            end = s['addr'] + s['size']
            bss_start = start if bss_start is None else min(bss_start, start)
            bss_end = end if bss_end is None else max(bss_end, end)
            continue
        if s['flags'] & SHF_EXECINSTR:
            text_secs.append(s)
        else:
            data_secs.append(s)

    if len(text_secs) > 7:
        sys.exit(f"error: {len(text_secs)} text sections, DOL supports max 7")
    if len(data_secs) > 11:
        sys.exit(f"error: {len(data_secs)} data sections, DOL supports max 11")

    header = bytearray(256)

    # Layout: file offsets start right after the 256-byte header, packed
    # sequentially, each 32-byte aligned (matches standard elf2dol behavior).
    cur_off = 256
    file_chunks = []  # (dol_offset, bytes)

    def place(sec_list, off_table_pos, addr_table_pos, size_table_pos):
        nonlocal cur_off
        for idx, s in enumerate(sec_list):
            cur_off = (cur_off + 31) & ~31
            struct.pack_into('>I', header, off_table_pos + idx * 4, cur_off)
            struct.pack_into('>I', header, addr_table_pos + idx * 4, s['addr'])
            struct.pack_into('>I', header, size_table_pos + idx * 4, s['size'])
            file_chunks.append((cur_off, data[s['offset']:s['offset'] + s['size']]))
            cur_off += s['size']

    place(text_secs, 0x00, 0x48, 0x90)
    place(data_secs, 0x1C, 0x64, 0xAC)

    if bss_start is not None:
        struct.pack_into('>I', header, 0xD8, bss_start)
        struct.pack_into('>I', header, 0xDC, bss_end - bss_start)

    struct.pack_into('>I', header, 0xE0, entry)

    total_size = cur_off
    out = bytearray(total_size)
    out[0:256] = header
    for off, chunk in file_chunks:
        out[off:off + len(chunk)] = chunk

    with open(dol_path, 'wb') as f:
        f.write(out)

    print(f"wrote {dol_path}: {len(text_secs)} text section(s), "
          f"{len(data_secs)} data section(s), "
          f"bss={'none' if bss_start is None else hex(bss_start)+'..'+hex(bss_end)}, "
          f"entry={hex(entry)}, size={total_size} bytes")


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} in.elf out.dol")
    convert(sys.argv[1], sys.argv[2])
