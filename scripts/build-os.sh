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

# ---- 构建戳：记下这次成功构建时各子模块的 SHA ----
#
# 为什么需要：MIPS 闸门（verify-mips-e2e / verify-frames）跑的是 build-os-mips 里的
# 二进制，而那个二进制**可能是上一次构建的**。实测踩过：qzjs 升到 960f24d5 之后
# MIPS 构建其实失败了（polyfill 要用目标架构的 qjsc，x86 宿主 Exec format error），
# 而 verify-mips-e2e 拿 15:04 的旧二进制跑出 10/10 全绿。
#
# 那种绿比红危险：它让人以为 MIPS 那侧是验过的。子模块 gitlink 变了但二进制没跟上，
# 只看「闸门全绿」是发现不了的。
#
# 所以戳记的是 **SHA** 而不是时间戳：时间戳只能证明「比某个文件新」，而真正要
# 回答的是「这个二进制是从哪份依赖构建出来的」。
STAMP="$BUILD_DIR/.build-stamp"
{
  echo "qzjs=$(git -C qzjs rev-parse HEAD 2>/dev/null || echo none)"
  echo "lvgl=$(git -C third_party/lvgl rev-parse HEAD 2>/dev/null || echo none)"
  echo "uvrpc=$(git -C third_party/uvrpc rev-parse HEAD 2>/dev/null || echo none)"
} > "$STAMP"

echo
echo "artifacts:"
ls -la "$BUILD_DIR/qzos-host" "$BUILD_DIR/qzjs-rt" 2>/dev/null || ls -la "$BUILD_DIR/qzos-host"
file "$BUILD_DIR/qzos-host" | sed 's/^/  /'
echo "run (native, pbm frames):"
echo "  QZ_DISPLAY=pbm QZ_RPC_SOCK=none QZ_JS_DIR=os/js ./$BUILD_DIR/qzos-host"
