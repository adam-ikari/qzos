#!/usr/bin/env bash
# scripts/test-keymap.sh — 键码映射单测（纯逻辑，无 LVGL、无宿主构建、无设备）
#
# 为什么单独一个：字母段的 evdev 码**不连续**（KEY_Q=16、KEY_A=30、KEY_Z=44），
# 而这台设备没有触摸、键盘是唯一输入。一次"用算术从码推字符"的写法就能让
# 每个字母都静默打错（KEY_H -> 'f'、KEY_0 -> ':'），屏幕上完全看不出是映射
# 错了，只会觉得"键盘不好使"。这类东西必须钉死在单测里。
#
# 测试的键码常量直接取自内核头 <linux/input.h>，所以测试自己不会抄错码；
# 期望的字符才是字面量。移植到新键盘布局时改 keymap.c 即可。
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

gcc -std=c99 -Wall -Wextra -Werror -O1 -g \
  -o "$OUT/test-keymap" \
  os/test/test_keymap.c os/src/keymap.c \
  -I os/src

"$OUT/test-keymap"
