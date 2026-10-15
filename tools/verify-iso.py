#!/usr/bin/env python3
"""Check complete ISO size, compare visible files to the disc, and save SHA-256.

This checks structure and visible content, not a second read of every disc sector.
The sector copy must also finish without read errors.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def read_at(stream, offset, length):
    stream.seek(offset)
    data = stream.read(length)
    if len(data) != length:
        raise ValueError(f'Truncated ISO at {offset:#x}')
    return data


def records(data, block_size):
    pos = 0
    while pos < len(data):
        length = data[pos]
        if length == 0:
            pos = (pos // block_size + 1) * block_size
            continue
        if length < 34 or pos + length > len(data):
            raise ValueError('Invalid directory record')
        record = data[pos:pos + length]
        name = record[33:33 + record[32]]
        lba, size = struct.unpack_from('<I', record, 2)[0], struct.unpack_from('<I', record, 10)[0]
        yield name, lba, size, record[25]
        pos += length


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    parser.add_argument('--disc', type=Path, default=ROOT / '.local/disc/files')
    args = parser.parse_args()
    checked = []
    with args.iso.open('rb') as stream:
        pvd = read_at(stream, 16 * 2048, 2048)
        if pvd[:7] != b'\x01CD001\x01':
            raise ValueError('Expected ISO9660 primary volume descriptor')
        block_size = struct.unpack_from('<H', pvd, 128)[0]
        if block_size != 2048:
            raise ValueError('Expected 2048-byte DVD sectors')
        volume_size = struct.unpack_from('<I', pvd, 80)[0] * block_size
        if args.iso.stat().st_size != volume_size:
            raise ValueError(f'Incomplete/oversized ISO: expected {volume_size} bytes')
        root_lba = struct.unpack_from('<I', pvd, 158)[0]
        root_size = struct.unpack_from('<I', pvd, 166)[0]
        visited = set()

        def walk(lba, size, parent):
            if lba in visited:
                raise ValueError('Directory cycle')
            visited.add(lba)
            for name, child_lba, child_size, flags in records(read_at(stream, lba * block_size, size), block_size):
                if name in (b'\0', b'\1'):
                    continue
                filename = name.decode('ascii').split(';')[0]
                relative = parent / filename
                if child_lba * block_size + child_size > volume_size:
                    raise ValueError(f'Out-of-range extent: {relative}')
                if flags & 2:
                    walk(child_lba, child_size, relative)
                else:
                    data = read_at(stream, child_lba * block_size, child_size)
                    # UDF preserves mixed case; ISO9660 names are uppercase.
                    source = args.disc
                    for component in relative.parts:
                        matches = [p for p in source.iterdir() if p.name.casefold() == component.casefold()]
                        if len(matches) != 1:
                            raise ValueError(f'Missing/ambiguous disc path: {relative}')
                        source = matches[0]
                    if data != source.read_bytes():
                        raise ValueError(f'Disc mismatch: {relative}')
                    checked.append(dict(path=str(relative), bytes=child_size,
                                        sha256=hashlib.sha256(data).hexdigest()))

        walk(root_lba, root_size, Path())
        stream.seek(0)
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    checksum = args.iso.with_suffix('.iso.sha256')
    checksum.write_text(f'{digest}  {args.iso.name}\n')
    report = dict(iso=str(args.iso.resolve()), bytes=volume_size, sha256=digest,
                  visible_files_matching_disc=checked,
                  note='Full sector copy + size check + visible file comparison; no full second disc read or emulator boot test')
    reports = ROOT / '.local/reports'
    reports.mkdir(parents=True, exist_ok=True)
    (reports / 'iso.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'ISO verified: {volume_size} bytes, {len(checked)} files match the disc')
    print(f'SHA-256: {digest}')


if __name__ == '__main__':
    main()
