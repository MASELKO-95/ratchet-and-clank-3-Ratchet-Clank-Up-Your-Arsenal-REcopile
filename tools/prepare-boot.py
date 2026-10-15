#!/usr/bin/env python3
"""Recover UYA boot ELF using the section layout documented by Wrench.

Reference: Wrench src/core/elf.cpp, DONOR_UYA_BOOT_ELF_HEADERS.
Boot and level layouts are validated separately.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
# name, section type, flags, alignment (Wrench UYA donor layout)
LAYOUT = [
    ('.reginfo', 0x70000006, 0, 4), ('.vutext', 1, 6, 16),
    ('core.text', 1, 6, 64), ('core.data', 1, 3, 128),
    ('core.rdata', 1, 2, 16), ('core.bss', 8, 0x10000003, 64),
    ('core.lit', 1, 0x10000003, 8), ('.lit', 1, 0x10000003, 64),
    ('.bss', 8, 0x10000003, 64), ('.data', 1, 3, 64),
    ('lvl.vtbl', 1, 2, 1), ('lvl.camvtbl', 1, 2, 1),
    ('lvl.sndvtbl', 1, 2, 1), ('.text', 1, 6, 64),
    ('patch.data', 0x70000006, 6, 4), ('legal.data', 1, 3, 4),
    ('mc1.data', 1, 3, 64), ('mc1.data', 1, 3, 64),
]


def recover(raw, layout=LAYOUT):
    sections, pos, entry = [], 0, None
    for name, expected_type, flags, alignment in layout:
        if pos + 16 > len(raw):
            raise ValueError('Truncated Ratchet section header')
        addr, size, kind, section_entry = struct.unpack_from('<4I', raw, pos)
        pos += 16
        if entry is None:
            entry = section_entry
        if kind != expected_type or section_entry != entry or size > len(raw) - pos:
            raise ValueError(f'Unexpected executable layout at {name}')
        if addr + size > 0x2000000:
            raise ValueError(f'Section outside EE memory: {name}')
        data = raw[pos:pos + size]
        if kind == 8 and any(data):
            raise ValueError(f'Nonzero NOBITS data: {name}')
        sections.append(dict(name=name, address=addr, size=size, type=kind,
                             flags=flags, alignment=alignment, data=data))
        pos += size
    if pos != len(raw):
        raise ValueError('Unexpected trailing data')
    if not any(s['flags'] & 4 and s['name'] != '.vutext' and
               s['address'] <= entry < s['address'] + s['size'] for s in sections):
        raise ValueError('Entry point is outside EE code')

    # A PT_LOAD for each allocatable section keeps addresses and zero fills explicit.
    loads = [s for s in sections if s['flags'] & 2]
    out = bytearray(52 + 32 * len(loads))
    strings = bytearray(b'\0')
    headers = [(0,) * 10]
    for s in sections:
        name_offset = len(strings)
        strings.extend(s['name'].encode() + b'\0')
        out.extend(b'\0' * (-len(out) % s['alignment']))
        s['offset'] = len(out)
        if s['type'] != 8:
            out.extend(s['data'])
        kind = 1 if s['name'] == 'patch.data' else s['type']
        headers.append((name_offset, kind, s['flags'], s['address'], s['offset'],
                        s['size'], 0, 0, s['alignment'], 1 if kind == 0x70000006 else 0))
    name_offset = len(strings)
    strings.extend(b'.shstrtab\0')
    string_offset = len(out)
    out.extend(strings)
    headers.append((name_offset, 3, 0, 0, string_offset, len(strings), 0, 0, 1, 0))
    out.extend(b'\0' * (-len(out) % 4))
    shoff = len(out)
    for header in headers:
        out.extend(struct.pack('<10I', *header))
    ident = b'\x7fELF\x01\x01\x01' + b'\0' * 9
    struct.pack_into('<16sHHIIIIIHHHHHH', out, 0, ident, 2, 8, 1, entry,
                     52, shoff, 0x20924001, 52, 32, len(loads), 40,
                     len(headers), len(headers) - 1)
    for i, s in enumerate(loads):
        # p_align=1 is valid, and avoids imposing page alignment on section offsets.
        permissions = 4 | (2 if s['flags'] & 1 else 0) | (1 if s['flags'] & 4 else 0)
        struct.pack_into('<8I', out, 52 + i * 32, 1, s['offset'], s['address'],
                         s['address'], 0 if s['type'] == 8 else s['size'],
                         s['size'], permissions, 1)
    return out, entry, [{k: v for k, v in s.items() if k != 'data'} for s in sections]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc', nargs='?', type=Path,
                        default=ROOT / '.local/disc/files')
    args = parser.parse_args()
    source = args.disc / 'SCES_524.56'
    cnf = (args.disc / 'SYSTEM.CNF').read_text()
    if 'SCES_524.56' not in cnf or source.read_bytes()[:4] != b'\x7fELF':
        raise ValueError('Expected SCES-52456 boot ELF')
    inputs = ROOT / '.local/input'
    inputs.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, inputs / source.name)
    raw_path = inputs / 'boot.ratchetexe'
    subprocess.run([str(ROOT / 'build/tools/unpack-wad'), str(source), str(raw_path)], check=True)
    output, entry, sections = recover(raw_path.read_bytes())
    elf = inputs / 'rac3-pal-boot.elf'
    elf.write_bytes(output)
    report = dict(serial='SCES-52456', region='PAL', source=str(source),
                  source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  boot_sha256=hashlib.sha256(output).hexdigest(),
                  entry_point=hex(entry), sections=sections,
                  status='Recovered boot ELF; game execution not validated')
    reports = ROOT / '.local/reports'
    reports.mkdir(parents=True, exist_ok=True)
    (reports / 'boot.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Recovered {elf}: entry={entry:#x}, sections={len(sections)}')


if __name__ == '__main__':
    main()
