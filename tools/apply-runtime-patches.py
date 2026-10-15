#!/usr/bin/env python3
"""Apply the reviewed local runtime fix, preserving unrelated upstream edits."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
repo = ROOT / 'third_party/PS2Recomp'
patch = ROOT / 'patches/ps2recomp-iop-entry-arguments.patch'
command = ['git', '-C', str(repo), 'apply']
if subprocess.run(command + ['--reverse', '--check', str(patch)], capture_output=True).returncode == 0:
    print('Runtime patch already applied')
else:
    subprocess.run(command + ['--check', str(patch)], check=True)
    subprocess.run(command + [str(patch)], check=True)
    print('Runtime patch applied')
