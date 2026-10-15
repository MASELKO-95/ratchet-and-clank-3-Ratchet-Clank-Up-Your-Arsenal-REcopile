#!/usr/bin/env python3
"""Inventory UYA's hidden TOC (Wrench src/iso/table_of_contents.cpp).

Reads metadata only; does not extract assets or execute game code.
"""
import argparse
import json
from pathlib import Path
import struct

SECTOR = 2048
TOC_LBA = 1001
GLOBAL_TYPES = {0x648: 'mpeg', 0x48: 'misc', 0xbf0: 'bonus', 0xc30: 'space',
                0x398: 'armor', 0x2340: 'audio', 0x2ab0: 'hud', 0x3c8: 'gadget'}
LEVEL_TYPES = {0x60: 'level', 0x1818: 'audio', 0x26f0: 'scene'}


def inventory(iso):
    with iso.open('rb') as stream:
        stream.seek(16 * SECTOR)
        pvd = stream.read(SECTOR)
        if pvd[:7] != b'\x01CD001\x01' or pvd[40:72].rstrip(b' ') != b'RATCHETANDCLANK3':
            raise ValueError('Expected Ratchet & Clank 3 ISO9660 volume')
        volume_sectors = struct.unpack_from('<I', pvd, 80)[0]
        stream.seek(TOC_LBA * SECTOR)
        toc = stream.read(0x200000)
    if len(toc) != 0x200000:
        raise ValueError('Truncated table of contents')
    globals_, levels, pos = [], [], 0
    while pos + 8 <= len(toc):
        size, sector = struct.unpack_from('<2I', toc, pos)
        if size not in GLOBAL_TYPES:
            break
        if not 0 < sector < volume_sectors or pos + size > len(toc):
            raise ValueError('Invalid global header')
        globals_.append(dict(type=GLOBAL_TYPES[size], toc_offset=pos, file_lba=sector))
        pos += size
    if not globals_:
        raise ValueError('No UYA global headers found')
    for index in range(100):
        parts = []
        for slot in range(3):
            header_lba, sectors = struct.unpack_from('<2I', toc, pos + index * 24 + slot * 8)
            if header_lba == 0 and sectors == 0:
                continue
            header_offset = (header_lba - TOC_LBA) * SECTOR
            if header_offset < 0 or header_offset + 8 > len(toc):
                raise ValueError('Invalid level header reference')
            size, file_lba = struct.unpack_from('<2I', toc, header_offset)
            if size not in LEVEL_TYPES or header_offset + size > len(toc):
                raise ValueError(f'Unknown level header type {size:#x}')
            if file_lba == 0 or file_lba + sectors > volume_sectors:
                raise ValueError('Level archive outside volume')
            part = dict(type=LEVEL_TYPES[size], header_lba=header_lba,
                        file_lba=file_lba, sectors=sectors)
            if size == 0x60:
                part['level_id'] = struct.unpack_from('<I', toc, header_offset + 8)[0]
            parts.append(part)
        if parts:
            levels.append(dict(index=index, parts=parts))
    return dict(volume_sectors=volume_sectors, toc_lba=TOC_LBA,
                global_headers=globals_, level_archives=levels)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    args = parser.parse_args()
    report = inventory(args.iso)
    dest = Path(__file__).resolve().parents[1] / '.local/reports/toc.json'
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(report, indent=2) + '\n')
    print(f"TOC: {len(report['global_headers'])} global headers, "
          f"{len(report['level_archives'])} populated level entries; {dest}")


if __name__ == '__main__':
    main()
