#!/usr/bin/env bash
# scripts/test-display.sh — 纯逻辑层单测（raster_1bpp + display_policy + panel）
#
# 这三层无 I/O、无全局状态、不依赖 LVGL/uvrpc/qzjs，因此不需要宿主构建，
# 也不需要设备。移植到新 SoC 后先跑这个——条带寻址和刷新节奏的回归
# 在这里挡住，比在屏上目视靠谱得多。
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

gcc -std=c99 -Wall -Wextra -Werror -O1 -g \
  -o "$OUT/test-display" \
  os/test/test_display.c \
  os/src/raster_1bpp.c os/src/display_policy.c os/src/panel.c \
  -I os/src

"$OUT/test-display"
