#!/usr/bin/env python3
"""Check the publication tree, excluding local inputs and dependencies."""
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ALLOWED_ROOTS = {'native','cmake','patches','tools','LICENSES','.github'}
ALLOWED_FILES = {'README.md','LICENSE','NOTICE','THIRD_PARTY.md','.gitignore'}
EXTENSIONS = {'.py','.sh','.cpp','.h','.comp','.json','.cmake','.patch','.md','.txt','.yml'}

def main():
    paths = subprocess.check_output(['git','-C',str(ROOT),'ls-files','-z']).decode().split('\0')
    checked = []
    for name in filter(None, paths):
        relative = Path(name)
        if name not in ALLOWED_FILES and (relative.parts[0] not in ALLOWED_ROOTS or relative.suffix not in EXTENSIONS):
            raise SystemExit('Not part of the source publication: ' + name)
        path = ROOT / relative
        if path.is_symlink() or path.stat().st_size > 2_000_000:
            raise SystemExit('Unexpected link or oversized source file: ' + name)
        text = path.read_text(encoding='utf-8')
        home_prefix = chr(47) + 'home' + chr(47)
        if home_prefix in text:
            raise SystemExit('Private absolute path in: ' + name)
        if relative.suffix == '.py': compile(text, str(path), 'exec')
        if relative.suffix == '.sh': subprocess.run(['bash','-n',str(path)],check=True)
        if relative.suffix == '.json': json.loads(text)
        checked.append(name)
    if not checked: raise SystemExit('No staged/tracked source files to validate')
    print(f'Publication check: {len(checked)} source/license files; no ISO, BIOS, generated code or binaries')

if __name__ == '__main__': main()
