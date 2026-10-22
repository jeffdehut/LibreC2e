#!/bin/bash
# Cross-builds LibreC2e (opencreatures1.exe) for 64-bit Windows with MinGW-w64, in
# build-windows/. On a Mac: brew install mingw-w64 ninja (the toolchain file assumes
# Homebrew's location; change CMAKE_FIND_ROOT_PATH in it for other systems).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cmake -S "$ROOT" -B "$ROOT/build-windows" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DCMAKE_TOOLCHAIN_FILE="$ROOT/packaging/mingw-w64-x86_64.cmake"
cmake --build "$ROOT/build-windows" --target opencreatures1
mkdir -p "$ROOT/build-windows/Licenses/LibreCreatures"
cp "$ROOT/LICENSE" "$ROOT/COPYING" "$ROOT/NOTICE.md" "$ROOT/AUTHORS" "$ROOT/build-windows/Licenses/"
cp "$ROOT/third_party/librecreatures/LICENSE" "$ROOT/third_party/librecreatures/NOTICE.md" "$ROOT/build-windows/Licenses/LibreCreatures/"
echo "Built $ROOT/build-windows/opencreatures1.exe"
