#!/usr/bin/env python3
"""Extract UYA IOP modules using Wrench's UyaMiscWadHeader/UyaIrxHeader."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import shutil

ROOT = Path(__file__).resolve().parents[1]
NAMES = ['sio2man', 'mcman', 'mcserv', 'padman', 'mtapman', 'libsd', '989snd',
         'stash', 'inet', 'netcnf', 'inetctl', 'msifrpc', 'dev9', 'smap', 'libnetb',
         'ppp', 'pppoe', 'usbd', 'lgaud', 'eznetcnf', 'eznetctl', 'lgkbm']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('inventory', ROOT / 'tools/inventory-iso.py')
    inventory = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(inventory)
    toc = inventory.inventory(args.iso)
    misc = next(p for p in toc['global_headers'] if p['type'] == 'misc')
    with args.iso.open('rb') as stream:
        stream.seek(1001 * 2048 + misc['toc_offset'] + 0x10)
        sector, sectors = struct.unpack('<2I', stream.read(8))
        offset = (misc['file_lba'] + sector) * 2048
        if offset + sectors * 2048 > args.iso.stat().st_size:
            raise ValueError('IRX archive outside ISO')
        stream.seek(offset)
        packed = stream.read(sectors * 2048)
    dest = ROOT / '.local/input/irx'
    dest.mkdir(parents=True, exist_ok=True)
    packed_path, raw_path = dest / 'archive.wad', dest / 'archive.bin'
    packed_path.write_bytes(packed)
    subprocess.run([str(ROOT / 'build/tools/unpack-wad'), str(packed_path), str(raw_path)], check=True)
    raw = raw_path.read_bytes()
    if len(raw) < 0xb8:
        raise ValueError('Truncated UYA IRX header')
    modules = []
    for i, name in enumerate(NAMES):
        offset, size = struct.unpack_from('<2I', raw, 8 + i * 8)
        if not size:
            continue
        if offset < 0xb8 or offset + size > len(raw):
            raise ValueError(f'Module outside archive: {name}')
        binary = raw[offset:offset + size]
        if binary[:7] != b'\x7fELF\x01\x01\x01' or struct.unpack_from('<H', binary, 18)[0] != 8:
            raise ValueError(f'Expected MIPS ELF: {name}')
        output = dest / f'{name}.irx'
        output.write_bytes(binary)
        mounted = ROOT / '.local/disc/files/IRX'
        mounted.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(output, mounted / output.name)
        modules.append(dict(name=name, bytes=size, sha256=hashlib.sha256(binary).hexdigest(), file=str(output)))
    (ROOT / '.local/reports/irx.json').write_text(json.dumps(modules, indent=2) + '\n')
    print(f'Extracted {len(modules)} IOP module ELFs in {dest}')


if __name__ == '__main__':
    main()
