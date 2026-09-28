#!/usr/bin/env bash
# Cross-build qzjs for the C1 Slim / MP-D261 (MIPS32r2 LE, hard-float, musl static).
# Usage: scripts/build-mips.sh [extra cmake args...]
set -euo pipefail
cd "$(dirname "$0")/.."

[ -x .tools/zig/zig ] || scripts/fetch-tools.sh

export PATH="$PWD/.tools:$PATH"
BUILD_DIR="${BUILD_DIR:-build-mips}"

cmake -S qzjs -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/mipsel-zig.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DQZ_PROFILE=minimal \
  "$@"
ninja -C "$BUILD_DIR" -j"$(nproc)"

echo
echo "artifacts:"
ls -la "$BUILD_DIR"/qzjs "$BUILD_DIR"/qzjs-rt
file "$BUILD_DIR"/qzjs | sed 's/^/  /'
