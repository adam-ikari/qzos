#!/usr/bin/env bash
# scripts/probe-power-owner.sh — 真机上探测「电源键归谁管」（闸门 0 的一步）
#
# 用法（插上设备后）：
#   ADB="adb -s <serial>" scripts/probe-power-owner.sh
#
# 输出照抄进 os/src/power.c 的描述符表，逐条对应 brain qzos-power-sim 的
# P1–P7 台账。**不要凭这份输出直接改代码**——先确认它说的是这台设备。
#
# 探测手段全是只读（ps / cat / ls），不会改设备状态。只有最后一步
# 「按住电源键看 dmesg」需要人，且可能真的让设备休眠——所以单列，且默认
# 不执行。
set -euo pipefail
cd "$(dirname "$0")/.."

ADB=${ADB:-adb}
read -r -a ADB_CMD <<<"$ADB"
if ! "${ADB_CMD[@]}" get-state >/dev/null 2>&1; then
  cat >&2 <<EOF
无法连到设备。用法：
  ADB="adb -s <serial>" $0
设备上是 root adb（brain c1-slim-device）。
EOF
  exit 1
fi

sh_() { "${ADB_CMD[@]}" shell "$1" 2>/dev/null | tr -d '\r'; }

echo "=== P6: power_supply 节点（有没有电池/充电状态可读）==="
sh_ 'ls -1 /sys/class/power_supply/ 2>/dev/null || echo "(不存在)"'
for d in /sys/class/power_supply/*/; do
  [ -d "$d" ] || continue
  echo "  $d type=$(sh_ "cat $d/type 2>/dev/null") status=$(sh_ "cat $d/status 2>/dev/null") cap=$(sh_ "cat $d/capacity 2>/dev/null")"
done

echo
echo "=== P3/P4: 有没有可写的 suspend / poweroff 路径（最危险的一条）==="
echo "-- /sys/power/ --"
sh_ 'ls -1 /sys/power/ 2>/dev/null || echo "(不存在)"'
echo "-- /sys/power/state 能不能写（只读试探，不真写 mem）--"
sh_ 'test -w /sys/power/state && echo "   WRITABLE: $(cat /sys/power/state 2>/dev/null)" || echo "   不可写或不存在"'
echo "-- sysrq 是否开启（没有 poweroff sysfs 时的唯一软关机途径）--"
sh_ 'cat /proc/sys/kernel/sysrq 2>/dev/null || echo "(读不到)"'
echo "   ^ 0=完全关闭 1=全部开启；关机需要 1。**这是 P3/P4 缺口的兜底**——"
echo "     若 sysrq=1 且无可写 poweroff，可考虑 sysrq-o（需真机验证安全性）"

echo
echo "=== P1: 电源键在哪个 evdev、是什么键码 ==="
echo "-- /proc/bus/input/devices 里的 KEY_POWER / KEY_SLEEP 登记 --"
sh_ 'grep -B4 -E "KEY_POWER|KEY_SLEEP" /proc/bus/input/devices 2>/dev/null || echo "(内核未登记这两个键)"'
echo "-- event 设备与能力位 --"
for e in /sys/class/input/event*/; do
  n=$(sh_ "cat $e/device/name 2>/dev/null")
  echo "  $e name=$n"
  sh_ "cat $e/device/capabilities 2>/dev/null" | head -1
done

echo
echo "=== P2: 谁可能已经在处理电源键（常驻进程扫描）==="
sh_ 'ps w 2>/dev/null | grep -iE "pmd|power|pm_|suspend|charger|battery|sleep" | grep -v grep || echo "(没看到名字像电源管理的进程)"'
echo "-- 这些进程的 open fd 里有没有 gpio/power 设备 --"
for p in $(sh_ 'ps w 2>/dev/null | grep -iE "pmd|power|pm_" | grep -v grep | awk "{print \$1}"'); do
  echo "  pid=$p"
  sh_ "ls -l /proc/$p/fd 2>/dev/null | grep -iE 'gpio|power|input|event' | head -5"
done

echo
echo "=== 设备上的 init 链（找 S?? 脚本里谁碰电源）==="
sh_ 'ls -1 /etc/init.d/ 2>/dev/null | head -40'
sh_ 'grep -rilE "suspend|poweroff|KEY_POWER|pmd" /etc/init.d/ 2>/dev/null | head -10'

cat <<'EOF'

=== P5: 熄屏后能否唤醒回 qzos（需要人）===
  1. 手动让设备熄屏（短按电源键 / 等超时）
  2. 观察：屏幕是全黑？还是残留画面？
  3. 再按任意键：是回到桌面，还是重启整机（厂商 logo 出现）？
  4. 记下现象 —— 这决定恢复流程要做「resume」还是「relaunch」

=== 附：内核日志基线（按住电源键前后各抓一次）===
  adb shell 'dmesg | tail -50' > /tmp/dmesg-before.txt
  # 现在按住电源键 2 秒
  adb shell 'dmesg | tail -50' > /tmp/dmesg-after.txt
  diff /tmp/dmesg-before.txt /tmp/dmesg-after.txt
  # 有 "Power down"/"suspend entry"/"input: KEY_POWER" 之类 → 说明了处理者
EOF
