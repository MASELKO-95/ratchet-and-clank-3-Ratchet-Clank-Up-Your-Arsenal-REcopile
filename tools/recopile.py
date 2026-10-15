#!/usr/bin/env python3
"""Direct RE:COPILE Edition entry point; no separate graphical launcher."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ISO = ROOT / ".local/disc/Ratchet-and-Clank-3-SCES-52456.iso"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    configure = commands.add_parser("configure")
    configure.add_argument("--iso", type=Path, required=True)
    configure.add_argument("--owned", action="store_true", required=True)
    commands.add_parser("prepare")
    commands.add_parser("status")
    run = commands.add_parser("run")
    run.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command == "configure":
        source = args.iso.expanduser().resolve()
        if not source.is_file() or source.stat().st_size != 4464115712:
            parser.error("Expected your complete PAL SCES-52456 image (4464115712 bytes).")
        ISO.parent.mkdir(parents=True, exist_ok=True)
        if ISO.exists() or ISO.is_symlink():
            if ISO.resolve() != source: parser.error("A different local image is already configured; it was left untouched.")
        else:
            ISO.symlink_to(source)
        print(json.dumps({"edition": "RE:COPILE Edition", "iso": str(source), "gameplay_verified": False}))
    elif args.command == "prepare":
        if not ISO.is_file(): parser.error("Configure your own disc image first.")
        program = shutil.which("7z") or shutil.which("7zz")
        if not program: parser.error("Install 7-Zip to extract your own image.")
        files = ROOT / ".local/disc/files"
        files.mkdir(parents=True, exist_ok=True)
        subprocess.run([program, "x", "-y", "-o" + str(files), str(ISO)], check=True)
        subprocess.run([sys.executable, str(ROOT / "tools/prepare-boot.py"), str(files)], check=True)
        subprocess.run([sys.executable, str(ROOT / "tools/extract-irx.py"), str(ISO)], check=True)
    elif args.command == "status":
        subprocess.run([sys.executable, str(ROOT / "tools/status-native.py")], check=True)
    else:
        forwarded = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
        subprocess.run(["bash", str(ROOT / "tools/run-native.sh"), *forwarded], cwd=ROOT, check=True)

if __name__ == "__main__": main()
