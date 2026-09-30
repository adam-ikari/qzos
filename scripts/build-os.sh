#!/usr/bin/env bash
# Build qzos-host (qzjs + LVGL + Fusion Pixel fonts + uvrpc).
#
# Usage:
#   scripts/build-os.sh            # native build (dev / pbm output)
#   scripts/build-os.sh --mips     # cross build for C1 Slim / MP-D261
#   scripts/build-os.sh -- <extra cmake args...>
#
# Env:
#   BUILD_DIR  override build dir (default build-os / build-os-mips)
set -euo pipefail
cd "$(dirname "$0")/.."

TARGET=native
if [ "${1:-}" = "--mips" ]; then
  TARGET=mips
  shift
fi
[ "${1:-}" = "--" ] && shift

if [ "$TARGET" = "mips" ]; then
  [ -x .tools/zig/zig ] || scripts/fetch-tools.sh
  export PATH="$PWD/.tools:$PATH"
  BUILD_DIR="${BUILD_DIR:-build-os-mips}"
  TOOLCHAIN=(-DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/mipsel-zig.cmake")
else
  BUILD_DIR="${BUILD_DIR:-build-os}"
  TOOLCHAIN=()
fi

if command -v ninja >/dev/null 2>&1; then GEN=(-G Ninja); else GEN=(); fi

cmake -S os -B "$BUILD_DIR" "${GEN[@]}" \
  "${TOOLCHAIN[@]}" \
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
  "$@"
if [ ${#GEN[@]} -gt 0 ]; then
  ninja -C "$BUILD_DIR" -j"$(nproc)"
else
  cmake --build "$BUILD_DIR" -j"$(nproc)"
fi

echo
echo "artifacts:"
ls -la "$BUILD_DIR/qzos-host" "$BUILD_DIR/qzjs-rt" 2>/dev/null || ls -la "$BUILD_DIR/qzos-host"
file "$BUILD_DIR/qzos-host" | sed 's/^/  /'
echo "run (native, pbm frames):"
echo "  QZ_DISPLAY=pbm QZ_RPC_SOCK=none QZ_JS_DIR=os/js ./$BUILD_DIR/qzos-host"
