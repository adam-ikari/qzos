#!/usr/bin/env bash
# scripts/gen-uvrpc-schema.sh — 从 uvrpc 的 schema 重新生成 flatcc 头
#
# ## 为什么需要这个脚本
#
# `os/third_party/uvrpc-generated/` 里的头是**提交入库**的产物：flatcc 编译器
# 只在宿主机上跑一次，之后 clone 的人不该为了编译宿主而先编译一个代码生成器。
# 但只要 uvrpc 升级到给 schema 加了东西（比如 4ed752a 起 `uvrpc_flatbuffers.c`
# 开始 `#include "rpc_verifier.h"`，那是 `-v` 开关生成的），入库的产物就过期，
# 表现为编译期 `fatal error: rpc_verifier.h: No such file or directory`。
#
# 这个脚本此前**被 `scripts/prepare-thirdparty.sh` 引用但根本不存在**——
# 也就是说「产物过期时怎么修」这条路是断的。现在补上。
#
# ## 为什么要自己编 flatcc
#
# uvrpc 通过 git submodule 引用 flatcc（`deps/flatcc`），而本仓只 vendor 了
# flatcc 的 **runtime**（`os/third_party/flatcc/src/runtime`）——那是宿主链接
# 需要的部分。**编译器**不在其中，所以这里按需把子模块拉下来编一份。
#
# ## 参数必须和上游一致
#
# 上游 `third_party/uvrpc/CMakeLists.txt` 里是：
#     flatcc -c -v -w -o <GENERATED_DIR> <RPC_SCHEMA>
# `-v` 就是生成 verifier 的那个开关。少写它 → 少一个头 → 编译失败；
# 多写它 → 产物和上游不一致，将来升级会对不上。
set -euo pipefail
cd "$(dirname "$0")/.."

UV=third_party/uvrpc
VENDOR=os/third_party/uvrpc-generated
FLATCC_DIR="$UV/deps/flatcc"
WORK=build-os/flatcc

[ -d "$UV" ] || { echo "missing $UV (submodule not checked out?)" >&2; exit 1; }

# ---- 1. 拿到 flatcc 编译器 ----
if [ ! -d "$FLATCC_DIR/src/cli" ]; then
  echo "==> flatcc 编译器不在位，初始化子模块"
  git -C "$UV" submodule update --init deps/flatcc
fi
[ -d "$FLATCC_DIR/src/cli" ] || {
  echo "flatcc 源码仍不可用：$FLATCC_DIR/src/cli" >&2; exit 1; }

# ---- 2. 编 flatcc（只需要编译器，runtime 已经在本仓 vendor）----
#
# 用 flatcc **自己的** CMake，而不是手写一条 cc 命令。手写的话要自己列全
# src/compiler/CMakeLists.txt 里的源文件（还有 external/hash/*.c），那份列表
# 会随 flatcc 版本烂掉，而烂掉的表现是「链接缺 fb_symbol_table_* 」这种
# 与本仓毫无关系的报错。
mkdir -p "$WORK"
if [ ! -x "$WORK/flatcc" ]; then
  echo "==> 编译 flatcc"
  # FLATCC_TEST 默认 ON，会把 gtest 之类一起构建——这里只要编译器，关掉。
  cmake -S "$FLATCC_DIR" -B "$WORK/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DFLATCC_TEST=OFF -DFLATCC_CXX_TEST=OFF -DFLATCC_INSTALL=OFF >/dev/null
  # 目标名是 flatcc_cli 而不是 flatcc：后者是静态库 libflatcc.a。
  cmake --build "$WORK/build" --target flatcc_cli -j"$(nproc 2>/dev/null || echo 2)" >/dev/null
  # 产物落在**源码树**的 bin/ 下，不在 build 目录里——flatcc 的 CMake 把
  # RUNTIME_OUTPUT_DIRECTORY 指到 ${PROJECT_SOURCE_DIR}/bin。找错位置的话
  # cp 会报 "cannot stat"，而那看起来像编译失败。
  cp "$FLATCC_DIR/bin/flatcc" "$WORK/flatcc"
fi
"$WORK/flatcc" --version 2>&1 | head -1 | sed 's/^/    /'

# ---- 3. 生成（参数与上游 CMakeLists 一致）----
echo "==> 生成 $VENDOR"
mkdir -p "$VENDOR"
"$WORK/flatcc" -c -v -w -o "$VENDOR" "$UV/schema/rpc.fbs"

# ---- 4. 存在性检查：少任何一个都在这里响亮失败 ----
# 不检查的话，缺的那个头要到编译 uvrpc 时才报，而那条错误信息
# （rpc_verifier.h: No such file or directory）完全看不出根因是「产物过期」。
missing=0
for f in rpc_reader.h rpc_builder.h rpc_verifier.h \
         flatbuffers_common_reader.h flatbuffers_common_builder.h; do
  if [ -f "$VENDOR/$f" ]; then echo "    ok  $f"
  else echo "    MISSING $f" >&2; missing=1; fi
done
[ "$missing" -eq 0 ] || { echo "生成不完整" >&2; exit 1; }

echo "==> done: $(ls "$VENDOR" | wc -l) 个头"
