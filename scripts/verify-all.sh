#!/usr/bin/env bash
# scripts/verify-all.sh — qzos 宿主全部自动化验证（无头、无设备）
#
# 从外到内分层跑，任何一层失败就停：
#   1. 纯逻辑单测（不需要宿主构建，最快，回归定位最快）
#   2. 端到端场景：键盘回放、帧一致性（需要宿主构建）
#
# 为什么按这个顺序：纯逻辑层（133+82 断言）毫秒级，条带寻址、脏区、键码
# 映射这类问题在这里就能定位；等到帧不对再去猜是哪一层算错了，方向就反了。
#
# 真机验证不在这里 —— /dev/epaper_lcd 只写不可读，落屏波形、残影、真实
# evdev 行为只能人眼在设备上看（见 brain: port-verification）。
set -euo pipefail
cd "$(dirname "$0")/.."

step() { echo; echo "=== $* ==="; }

step "display logic unit tests (pure, no host build)"
bash scripts/test-display.sh

step "keymap unit tests (pure, no host build)"
bash scripts/test-keymap.sh

step "power domain decision layer (pure, no device)"
bash scripts/test-power.sh

step "system-service registry (the only JS->C boundary)"
bash scripts/test-services.sh

step "app package + sandbox unit tests (real qzjs, no device)"
bash scripts/test-apkg.sh

step "shell app model end-to-end (manifest gate, pixel assertions)"
bash os/test/test_shell_apps.sh

step "JS engine crash recovery (kill rt, expect notice + respawn)"
bash os/test/verify-rt-recovery.sh

step "keyboard end-to-end (FIFO replay, semantic assertions)"
bash scripts/verify-input.sh

step "frame consistency: native == MIPS (qemu-user)"
bash scripts/verify-frames.sh

step "MIPS end-to-end (qemu-user: app model, auth facade, keyboard)"
bash scripts/verify-mips-e2e.sh

echo
echo "ALL PASS"
