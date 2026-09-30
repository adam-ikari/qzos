#!/usr/bin/env bash
# scripts/test-power.sh — 电源域决策层单测（纯逻辑，无设备、无宿主构建）
#
# 与 test-display.sh / test-keymap.sh 同一层：毫秒级纯逻辑，不依赖 LVGL、
# 不依赖设备、不依赖交叉工具链。上层现象不对时先确认它是绿的。
#
# 覆盖 os/docs/power-sim.md §3 的场景矩阵，重点是场景 D（归属未知 → 全拒）。
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=$(mktemp -d)
trap 'rm -rf "$BIN"' EXIT

cc -std=gnu99 -O1 -Wall -Wextra -Werror \
   -I os/src os/src/power.c os/test/test_power.c -o "$BIN/test_power"

"$BIN/test_power"
