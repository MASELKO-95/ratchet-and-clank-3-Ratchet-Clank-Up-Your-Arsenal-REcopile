#!/usr/bin/env python3
"""Fetch pinned tools without resetting an existing checkout."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PINS = {
    "PS2Recomp": ("https://github.com/ran-j/PS2Recomp.git", "2c5fbb9389e11dd95693385969490c9e8e6f57b4"),
    "wrench": ("https://github.com/chaoticgd/wrench.git", "75f6d86ad055dad5955cf9d771dd8146cbf7d6f6"),
    "raylib": ("https://github.com/raysan5/raylib.git", "c1ab645ca298a2801097931d1079b10ff7eb9df8"),
    "FidelityFX-FSR": ("https://github.com/GPUOpen-Effects/FidelityFX-FSR.git", "a21ffb8f6c13233ba336352bdff293894c706575"),
}

def main():
    for name, (url, revision) in PINS.items():
        target = ROOT / "third_party" / name
        if target.exists():
            actual = subprocess.check_output(["git", "-C", str(target), "rev-parse", "HEAD"], text=True).strip()
            if actual != revision:
                raise SystemExit(f"{name} is at {actual}; expected {revision}. Existing files were left untouched.")
            print(name + ": pinned checkout already present")
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--no-checkout", url, str(target)], check=True)
        subprocess.run(["git", "-C", str(target), "checkout", "--detach", revision], check=True)
        print(name + ": " + revision)

if __name__ == "__main__": main()
