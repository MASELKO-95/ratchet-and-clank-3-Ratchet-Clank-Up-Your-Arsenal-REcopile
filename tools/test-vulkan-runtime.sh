#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash tools/build-native.sh
cmake --build build/native --target ratchet-vulkan-tests -j "${JOBS:-4}"
ctest --test-dir build/native -R '^ratchet_vulkan_tests$' --output-on-failure --no-tests=error
