#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
exec build/native/ps2xRuntime/ratchet-recomp \
  --elf .local/input/rac3-pal-boot.elf \
  --iso .local/disc/Ratchet-and-Clank-3-SCES-52456.iso \
  --files .local/disc/files "$@"
