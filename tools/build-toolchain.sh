#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S third_party/PS2Recomp -B build/toolchain \
  -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RUNTIME=OFF \
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF
cmake --build build/toolchain --target ps2_analyzer ps2_recomp -j "${JOBS:-4}"
mkdir -p build/tools
c++ -O2 -std=c++17 -pthread -Ithird_party/wrench/src/engine \
  tools/unpack-wad.cpp third_party/wrench/src/engine/compression.cpp \
  -o build/tools/unpack-wad
