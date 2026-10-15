#!/usr/bin/env python3
"""Run the native experiment on a virtual display and summarize actual blockers."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def missing_imports(log_text):
    matches = set(re.findall(
        r'\[IOP\] unhandled import ([A-Za-z0-9_]+):(\d+) version=(0x[0-9a-fA-F]+)', log_text))
    return [dict(library=library, ordinal=int(ordinal), version=version)
            for library, ordinal, version in sorted(matches, key=lambda item: (item[0], int(item[1]), item[2]))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=10)
    parser.add_argument('--name', default='native-smoke')
    parser.add_argument('--dump-memory', action='store_true', help='Save EE/IOP RAM after stopping')
    parser.add_argument('--screenshot', action='store_true', help='Save the final window image')
    parser.add_argument('--press', action='append', default=[], metavar='SECONDS:KEY',
                        help='Send a timed key to the virtual window, e.g. 12:x or 14:Return')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600 or not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
        parser.error('Invalid seconds or report name')
    for press in args.press:
        match = re.fullmatch(r'(\d+(?:\.\d+)?):(Return|space|[xzcvqewasd]|Up|Down|Left|Right)', press)
        if not match or float(match[1]) + 0.2 >= args.seconds:
            parser.error('Each --press must be SECONDS:KEY before the time limit')
    logs = ROOT / '.local/logs'
    reports = ROOT / '.local/reports'
    logs.mkdir(parents=True, exist_ok=True)
    reports.mkdir(parents=True, exist_ok=True)
    log_path = logs / f'{args.name}.log'
    command = [str(ROOT / 'build/native/ps2xRuntime/ratchet-recomp'),
               '--elf', str(ROOT / '.local/input/rac3-pal-boot.elf'),
               '--iso', str(ROOT / '.local/disc/Ratchet-and-Clank-3-SCES-52456.iso'),
               '--files', str(ROOT / '.local/disc/files'), '--seconds', str(args.seconds)]
    if args.dump_memory:
        command += ['--dump-memory', str(logs / f'{args.name}-memory')]
    if args.screenshot:
        command += ['--screenshot', str(logs / f'{args.name}.png')]
    if args.press:
        driver = [sys.executable, str(ROOT / 'tools/drive-native.py')]
        for press in args.press:
            driver += ['--press', press]
        command = driver + ['--'] + command
    command = ['xvfb-run', '-a'] + command
    hard_timeout = False
    with log_path.open('w') as log:
        proc = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=True)
        try:
            proc.wait(timeout=args.seconds + 20)
        except subprocess.TimeoutExpired:
            hard_timeout = True
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
    text = log_path.read_text(errors='replace')
    blockers = [line for line in text.splitlines() if re.search(
        r'Error during program execution|\[ratchet\] Fatal|\[missing-function\]|'
        r'\[guest-branch:missing-target\]|call budget exhausted|'
        r'Function not found|function not found|Unimplemented PS2 stub called|'
        r'Failed to initialize|Failed to load|ERROR:|\[ps2xIOP:error\]|\[IOP\] unhandled import', line)]
    blockers = list(dict.fromkeys(blockers))
    # SDK diagnostics may omit newlines. Keep the failing branch without the
    # unrelated controller/memory-card messages preceding its marker.
    blockers = [line[line.index('[guest-branch:missing-target]'):]
                if '[guest-branch:missing-target]' in line else line for line in blockers]
    state = re.findall(r'\[ratchet:state\][^\r\n]*', text)
    disc_states = re.findall(r'\[ratchet:disc-state\][^\r\n]*', text)
    disc_state = dict(re.findall(r'(0x[0-9a-f]+)=(0x[0-9a-f]+)', disc_states[-1])) if disc_states else {}
    counters = {name: int(value) for name, value in re.findall(
        r'\b(dma|gif|gs|vif)=(\d+)', state[-1])} if state else {}
    gs_history_lines = re.findall(r'\[ratchet:gs-history\][^\r\n]*', text)
    gs_history = {name: int(value) for name, value in re.findall(
        r'(tags|registers|draws|image_transfers)=(\d+)', gs_history_lines[-1])} if gs_history_lines else {}
    time_limit = '[ratchet] Time limit reached' in text
    bindings = re.findall(r'\[ratchet:rpc-bind\] sid=(0x[0-9a-f]+) server=(0x[0-9a-f]+)', text)
    latest_bindings = dict(bindings)
    pending_bindings = [sid for sid, server in latest_bindings.items() if int(server, 16) == 0]
    if state and 'pc=0x3a8978 ' in state[-1] and '0x11' in pending_bindings:
        blockers.append('Boot stalled waiting for stash RPC SID 0x11; server not registered')
    if time_limit and pending_bindings:
        blockers.append('Time limit reached with unregistered RPC servers: ' + ', '.join(pending_bindings))
    if time_limit and int(disc_state.get('0x1ccfd4', '0'), 16) & 0xffff:
        blockers.append('Time limit reached while the guest was waiting for disc read completion')
    if time_limit and counters.get('gif') == 0 and counters.get('gs') == 0 and gs_history.get('tags', 0) == 0:
        blockers.append('Time limit reached without observed GIF/GS activity')
    iop = re.findall(r'\[ratchet:iop\][^\r\n]*', text)
    transfers = re.findall(r'\[ratchet:module-transfer\][^\r\n]*', text)
    report = dict(binary=str(ROOT / 'build/native/ps2xRuntime/ratchet-recomp'),
                  exit_code=proc.returncode, hard_timeout=hard_timeout,
                  time_limit_reached=time_limit,
                  boot_elf_loaded='[ratchet] Boot ELF loaded' in text,
                  runtime_returned='[ratchet] Runtime returned' in text,
                  state=state[-1] if state else None,
                  hardware_counters=counters, disc_state=disc_state,
                  gs_history=gs_history,
                  memory_dump=str(logs / f'{args.name}-memory') if args.dump_memory else None,
                  screenshot=str(logs / f'{args.name}.png') if args.screenshot else None,
                  input_events=re.findall(r'\[ratchet:input\][^\r\n]*', text),
                  iop=iop[-1] if iop else None,
                  module_transfers=transfers, pending_rpc_bindings=pending_bindings,
                  missing_iop_imports=missing_imports(text),
                  blockers=blockers[:20], log=str(log_path),
                  gameplay_validated=False)
    (reports / f'{args.name}.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return int(hard_timeout or proc.returncode != 0 or bool(blockers) or not report['boot_elf_loaded'])


if __name__ == '__main__':
    raise SystemExit(main())
