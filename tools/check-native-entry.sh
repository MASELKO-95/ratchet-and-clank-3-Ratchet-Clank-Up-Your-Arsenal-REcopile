#!/usr/bin/env bash
# Compile only the translated boot entry; this does not link or run the game.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tools
c++ -std=c++20 -msse4.1 -mavx2 -O0 \
  -I.local/recomp/boot/output \
  -Ithird_party/PS2Recomp/ps2xRuntime/include \
  -Ithird_party/PS2Recomp/ps2xRuntime/src/lib/Kernel \
  -Ithird_party/PS2Recomp/ps2xIOP/include \
  -c .local/recomp/boot/output/sub_0013ADA8_0x13ada8.cpp \
  -o build/tools/rac3-boot-entry.o
file build/tools/rac3-boot-entry.o
