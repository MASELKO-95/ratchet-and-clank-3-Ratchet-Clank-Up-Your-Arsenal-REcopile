#!/usr/bin/env python3
"""Heuristic boot recompilation experiment; does not build/run a game port."""
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def apply_entry_points(contents):
    """Preserve callbacks omitted by the heuristic analyzer on every generation."""
    manifest = json.loads((ROOT / 'native/boot-entry-points.json').read_text())
    addresses = sorted({int(address, 16) for group in manifest['groups']
                        for address in group['addresses']})
    if any(address <= 0 or address % 4 for address in addresses):
        raise ValueError('Guest entry points must be aligned nonzero addresses')
    additions = ''.join(f'  "guest_callback@0x{address:08X}",\n' for address in addresses)
    contents, count = re.subn(r'(entry_points\s*=\s*\[\n)',
                             lambda match: match[1] + additions, contents, count=1)
    if count != 1:
        raise RuntimeError('Analyzer config is missing the entry_points list')
    return contents


def logged(command, path):
    with path.open('w') as log:
        result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'Tool exited {result.returncode}; see {path}')


def main():
    workspace = ROOT / '.local/recomp/boot'
    workspace.mkdir(parents=True, exist_ok=True)
    config = workspace / 'config.toml'
    logged([str(ROOT / 'build/toolchain/ps2xAnalyzer/ps2_analyzer'),
            str(ROOT / '.local/input/rac3-pal-boot.elf'), str(config)], workspace / 'analyzer.log')
    # Use instruction handlers rather than speculative changes to original code.
    contents = config.read_text().replace('patch_cop0 = true', 'patch_cop0 = false')
    contents = contents.replace('patch_cache = true', 'patch_cache = false')
    contents = apply_entry_points(contents)
    config.write_text(contents)
    output = workspace / 'output'
    previous = {path: (path.read_bytes(), path.stat()) for path in output.glob('*') if path.is_file()}
    logged([str(ROOT / 'build/toolchain/ps2xRecomp/ps2_recomp'), str(config)], workspace / 'recompiler.log')
    # Leave unchanged generated files untouched from the compiler's point of
    # view, so adding a callback does not rebuild every translated routine.
    for path, (data, stat) in previous.items():
        if path.is_file() and path.read_bytes() == data:
            os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
    files = list(output.rglob('*.cpp'))
    if not files:
        raise RuntimeError('Recompiler returned success without C++ output')
    report = dict(stage='C++ generation only; no native game executable validated',
                  method='Heuristic analyzer; Ghidra function map still required',
                  cpp_files=len(files), cpp_bytes=sum(p.stat().st_size for p in files),
                  config=str(config), output=str(output))
    (ROOT / '.local/reports/recomp-boot.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
