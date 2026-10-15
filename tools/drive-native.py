#!/usr/bin/env python3
"""Send timed keyboard input to the native game inside the same Xvfb display."""
import argparse
import re
import shutil
import subprocess
import time


def parse_press(value):
    match = re.fullmatch(r'(\d+(?:\.\d+)?):(Return|space|[xzcvqewasd]|Up|Down|Left|Right)', value)
    if not match:
        raise argparse.ArgumentTypeError('Use SECONDS:KEY, e.g. 12:x or 14:Return')
    return float(match[1]), match[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--press', type=parse_press, action='append', default=[])
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command or not shutil.which('xdotool'):
        parser.error('A game command and xdotool are required')
    started = time.monotonic()
    proc = subprocess.Popen(command)
    try:
        for after, key in sorted(args.press):
            while proc.poll() is None and time.monotonic() < started + after:
                time.sleep(min(0.1, max(0.0, started + after - time.monotonic())))
            if proc.poll() is not None:
                break
            result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(proc.pid)],
                                    capture_output=True, text=True)
            windows = result.stdout.splitlines()
            if result.returncode or not windows:
                raise RuntimeError('Native game window was not found')
            subprocess.run(['xdotool', 'windowfocus', '--sync', windows[0]], check=True)
            print(f'[ratchet:input] seconds={after:g} key={key}', flush=True)
            subprocess.run(['xdotool', 'keydown', key], check=True)
            try:
                time.sleep(0.2)
            finally:
                subprocess.run(['xdotool', 'keyup', key], check=True)
        return proc.wait()
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == '__main__':
    raise SystemExit(main())
