#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tools/apply-runtime-patches.py
cmake -S third_party/PS2Recomp/ps2xIOP -B build/iop-tests -DPS2X_IOP_BUILD_TESTS=ON
cmake --build build/iop-tests -j "${JOBS:-4}"
ctest --test-dir build/iop-tests --output-on-failure
