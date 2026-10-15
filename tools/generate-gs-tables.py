#!/usr/bin/env python3
"""Translate the pinned runtime's GS address tables into Vulkan shader constants."""
import pathlib
import re
import sys

source, destination = map(pathlib.Path, sys.argv[1:])
lines = ['// Generated from PS2Recomp GS address tables; see THIRD_PARTY.md.']
for filename in ['ps2_gs_psmct32.h', 'ps2_gs_psmct16.h', 'ps2_gs_psmt8.h', 'ps2_gs_psmt4.h']:
    for name, body in re.findall(r'(?:constexpr|const) uint\w+_t (\w+)\[[^=]+\s*=\s*\{(.*?)\};', (source / filename).read_text(), re.S):
        values = re.findall(r'\b\d+\b', re.sub(r'//[^\n]*', '', body))
        lines.append(f'const uint {name}[{len(values)}] = uint[](' + ','.join(values) + ');')
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_text('\n'.join(lines) + '\n')
