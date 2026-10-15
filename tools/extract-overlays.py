#!/usr/bin/env python3
"""Recover UYA level ELFs using Wrench's TOC and level-data layout."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / filename)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    args = parser.parse_args()
    toc = module('inventory_iso', 'inventory-iso.py').inventory(args.iso)
    prepare = module('prepare_boot', 'prepare-boot.py')
    layout = prepare.LAYOUT[7:14]  # Wrench DONOR_RAC_GC_UYA_LEVEL_ELF_HEADERS
    if args.iso.stat().st_size != toc['volume_sectors'] * 2048:
        raise ValueError('Finish copying the ISO before extracting all overlays')
    dest = ROOT / '.local/input/overlays'
    dest.mkdir(parents=True, exist_ok=True)
    report = []

    def read(stream, offset, size):
        stream.seek(offset)
        data = stream.read(size)
        if len(data) != size:
            raise ValueError('Truncated archive')
        return data

    with args.iso.open('rb') as stream:
        for level in toc['level_archives']:
            for part in level['parts']:
                if part['type'] != 'level':
                    continue
                header = read(stream, part['header_lba'] * 2048, 0x60)
                data_sector, data_sectors = struct.unpack_from('<2I', header, 0x10)
                if data_sector + data_sectors > part['sectors']:
                    raise ValueError('Data section outside level archive')
                data_base = (part['file_lba'] + data_sector) * 2048
                offset, size = struct.unpack('<2I', read(stream, data_base, 8))
                if size == 0:
                    continue
                if offset + size > data_sectors * 2048:
                    raise ValueError('Overlay outside level data')
                raw = read(stream, data_base + offset, size)
                elf, entry, sections = prepare.recover(raw, layout)
                output = dest / f"level-{level['index']:02d}.elf"
                output.write_bytes(elf)
                report.append(dict(index=level['index'], id=part['level_id'],
                                   entry_point=hex(entry), elf=str(output),
                                   raw_bytes=size, elf_bytes=len(elf),
                                   sha256=hashlib.sha256(elf).hexdigest(), sections=sections))
    reports = ROOT / '.local/reports'
    reports.mkdir(parents=True, exist_ok=True)
    (reports / 'overlays.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Recovered {len(report)} level overlay ELFs in {dest}')


if __name__ == '__main__':
    main()
