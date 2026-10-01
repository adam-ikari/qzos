#!/usr/bin/env bash
# scripts/test-services.sh — 系统服务面注册表单测（纯逻辑，无设备）
#
# 这一层是「只有系统服务可以调用 C」这条约束的**可验证形态**：
# services.c 的注册表就是 C 能力的完整清单，注册表之外无 C 能力。
# 检查放在服务面侧而不是渲染桥侧——早期挂在 op_rpc 上，只挡住了那一条通道。
#
# 依赖 uvrpc + flatcc + qzjs 的 libuv（qzos_services_init 需要 loop），
# 所以这里链真实的 uvrpc 静态库，但**不启动宿主**，只测注册表与授权逻辑。
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=$(mktemp -d)
trap 'rm -rf "$BIN"' EXIT

# 复用 build-os 里已经编好的静态库；没有就先构建。
if [ ! -f build-os/libqzos_uvrpc.a ] || [ ! -f build-os/libqzos_flatcc.a ]; then
  echo "missing build-os/libqzos_{uvrpc,flatcc}.a — run scripts/build-os.sh" >&2
  exit 1
fi

cc -std=gnu99 -O1 -Wall -Wextra -Werror \
   -I os/src -I third_party/uvrpc/include -I os/third_party/uvrpc-generated \
   -I qzjs/deps/libuv/include -I qzjs/deps/cjson \
   os/src/services.c os/src/appauth.c os/test/test_services.c \
   -o "$BIN/test_services" \
   build-os/libqzos_uvrpc.a build-os/libqzos_flatcc.a \
   build-os/qzjs/libcjson.a build-os/lvgl/liblvgl.a build-os/libqzos_fonts.a \
   build-os/deps/libuv/libuv.a \
   build-os/deps/miniz/libminiz.a \
   -lrt -ldl -lpthread -lm

"$BIN/test_services"
