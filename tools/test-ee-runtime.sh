#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash tools/build-native.sh
cmake --build build/native --target ratchet-ee-tests -j "${JOBS:-4}"
ctest --test-dir build/native -R '^ratchet_ee_interrupt_tests$' --output-on-failure --no-tests=error
