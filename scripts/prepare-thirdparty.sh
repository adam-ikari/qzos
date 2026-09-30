#!/usr/bin/env bash
# scripts/prepare-thirdparty.sh — clone 后补齐 uvrpc 构建所需的东西
#
# 上游源码一律走 submodule（不提交进本仓），但 uvrpc 的构建有三样东西
# 不在它自己的仓库里，必须由 qzos 提供：
#
#   1. deps/flatcc   —— uvrpc 上游用 git submodule 引用 flatcc，但它的
#                       顶层 CMake 依赖链在本项目不可用（mimalloc 默认、
#                       强制输出目录），所以我们 vendor 一份到 os/third_party/
#   2. deps/uthash   —— 同上，单头文件
#   3. generated/    —— flatcc 从 schema/rpc.fbs 生成的 reader/builder 头，
#                       生成器（flatcc）只在宿主机跑一次，产物提交入库
#
# 这三样都在 os/third_party/ 下（已随本仓提交），本脚本负责把生成头
# 放到 uvrpc 期望的位置，并做一次一致性检查。
#
# 用法:  bash scripts/prepare-thirdparty.sh
set -euo pipefail
cd "$(dirname "$0")/.."

UV=third_party/uvrpc
VENDOR=os/third_party

[ -d "$UV" ] || { echo "missing $UV — run: git submodule update --init --recursive" >&2; exit 1; }
[ -d "$VENDOR/flatcc" ] || { echo "missing $VENDOR/flatcc (should be committed)" >&2; exit 1; }

echo "==> uvrpc submodule: $(git -C "$UV" rev-parse --short HEAD 2>/dev/null || echo 'not a git repo')"

# 1) flatcc / uthash 已在 os/third_party，CMake 直接指那里，无需复制。
# 2) 生成头：uvrpc 的 src/*.c 用 #include "rpc_reader.h" 之类，需要在
#    include 路径上。我们已把 os/third_party/uvrpc-generated 加进
#    qzos_uvrpc 的 PUBLIC include 目录（见 os/CMakeLists.txt），所以同样
#    不需要复制——这里只做存在性检查。
missing=0
for f in rpc_builder.h rpc_reader.h flatbuffers_common_builder.h flatbuffers_common_reader.h; do
  if [ -f "$VENDOR/uvrpc-generated/$f" ]; then
    echo "    ok  $f"
  else
    echo "    MISSING $f" >&2
    missing=1
  fi
done
[ "$missing" -eq 0 ] || {
  echo "generated headers incomplete — regenerate with scripts/gen-uvrpc-schema.sh" >&2
  exit 1
}

# 3) 上游 uvrpc 若自带 generated/（旧 checkout 残留），提示清理以免
#    编译时误用过期生成头。
if [ -d "$UV/generated" ]; then
  echo
  echo "note: $UV/generated exists (not used; qzos uses $VENDOR/uvrpc-generated)."
  echo "      Safe to leave; CMake does not reference it."
fi

cat <<'EOF'

Third-party ready. Next:
  scripts/test-display.sh                      # pure-logic unit tests
  scripts/build-os.sh                          # native build
  scripts/build-os.sh --mips                   # cross build (C1 Slim)
EOF
