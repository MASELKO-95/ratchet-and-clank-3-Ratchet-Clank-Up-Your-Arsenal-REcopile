#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
task_root="$PWD"
python3 tools/apply-runtime-patches.py
cmake -S third_party/PS2Recomp -B build/native \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -g1 -DNDEBUG' \
  -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF \
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF \
  -DPS2X_ENABLE_DEBUG_UI=OFF -DPS2X_ENABLE_SCCACHE=OFF \
  -DPS2X_ENABLE_RUNTIME_LOGS=ON -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF \
  -DPS2X_RUNNER_UNITY_BUILD_BATCH_SIZE=16 \
  -DFETCHCONTENT_SOURCE_DIR_RAYLIB="$task_root/third_party/raylib" \
  -DRATCHET_PROJECT_ROOT="$task_root" \
  -DCMAKE_PROJECT_PS2RetroX_INCLUDE="$task_root/cmake/AttachRatchet.cmake"
cmake --build build/native --target ps2EntryRunner -j "${JOBS:-4}"
