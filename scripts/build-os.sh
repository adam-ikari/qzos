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

  # 交叉构建需要 polyfill 工具链（内嵌字节码是构建产物），而它要调 qjsc ——
  # 默认取的是本构建目录里那个 qjsc，那是 **MIPS** 二进制，x86 宿主跑不了
  # （Exec format error），整个构建第一步就死。
  #
  # 所以这里显式给一个宿主可运行的 qjsc。顺序不能反：得先有宿主构建产出，
  # 才拿得到它的 qjsc —— 而它正是 polyfill 的 npm 工具链被装上的那次构建
  # （node + esbuild 只在宿主侧需要）。
  if [ ! -x build-os/deps/quickjs-ng/qjsc ]; then
    echo "==> MIPS build needs a host qjsc for the polyfill step; building the host first" >&2
    BUILD_DIR=build-os "$0"
    [ -x build-os/deps/quickjs-ng/qjsc ] || {
      echo "host qjsc still missing at build-os/deps/quickjs-ng/qjsc" >&2; exit 1; }
  fi
  # node + esbuild 是 polyfill 工具链。缺了它，qzjs 的 CMake 会去找那份已生成
  # 的 polyfill_default.c；新版本里那是 gitignore 的产物，fresh clone 上不存在。
  if [ ! -d qzjs/polyfill/node_modules/esbuild ]; then
    command -v npm >/dev/null 2>&1 || {
      echo "polyfill toolchain missing (no node_modules/esbuild) and npm not on PATH." >&2
      echo "run: npm --prefix qzjs/polyfill ci" >&2; exit 1; }
    echo "==> installing polyfill toolchain (node + esbuild)"
    npm --prefix qzjs/polyfill ci >/dev/null
  fi
  TOOLCHAIN+=(-DQZ_QJSC_HOST="$PWD/build-os/deps/quickjs-ng/qjsc")
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
