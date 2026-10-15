#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash tools/build-native.sh
cmake --build build/native --target ratchet-ee-tests ratchet-hardware-tests ratchet-mpeg-tests -j "${JOBS:-4}"
ctest --test-dir build/native -R '^ratchet_(ee_interrupt|hardware|mpeg)_tests$' --output-on-failure --no-tests=error
