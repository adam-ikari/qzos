#!/usr/bin/env bash
# scripts/verify-input.sh — 键盘通路回归（无头、无设备）
#
# 补的是什么洞：这台设备**没有触摸**，键盘是唯一输入通路，而它是整条链上
# 最没有自动化覆盖的一段——evdev -> uv_poll -> 事件队列 -> LVGL keypad ->
# group 焦点 -> JSON click 事件 -> JS shell 生命周期。任何一环静默断掉，
# 现象都是同一个：「按了没反应」。没有闸门就只能靠人肉在屏上试。
#
# 怎么无头驱动：QZ_INPUT0 指向一个 FIFO，os/test/replay-keys.py 按真实
# struct input_event 布局写进去，宿主照常走 uv_poll 读——和真机同一条路，
# 只是事件源从内核换成脚本。
#
# 三个场景，断言的是**语义**而不是黄金文件：
#   boot       不按键            -> 桌面帧 A
#   launch     down,enter        -> 帧 B，必须 != A（应用确实启动了）
#   roundtrip  down,enter,back   -> 帧 C，必须 == A（回桌面必须复现同一帧）
#
# roundtrip 这条是关键：e-ink 每帧提交的内容应当是状态的纯函数，所以
# 「进应用再退回来」必须逐字节回到原帧。它同时覆盖了退格路径和重渲染，
# 而且不是黄金文件那种自证——A 和 C 是两次独立运行产生的。
#
# 只跑原生：输入的风险是语义（路由/焦点/生命周期），不是平台差异；
# 平台等价性由 scripts/verify-frames.sh 的帧逐字节比较负责。
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${HOST:-$PWD/build-os/qzos-host}"
REPLAY="python3 os/test/replay-keys.py"
AUTOEXIT_S="${AUTOEXIT_S:-6}"
KEY_DELAY_S="${KEY_DELAY_S:-2.5}"
OUT=/tmp/qzos-input
mkdir -p "$OUT"

[ -x "$HOST" ] || { echo "missing $HOST — run scripts/build-os.sh" >&2; exit 1; }

pass=0; fail=0
ok()   { echo "  PASS  $*"; pass=$((pass+1)); }
bad()  { echo "  FAIL  $*"; fail=$((fail+1)); }

# run <name> <key-script>   -> 场景帧落在 $OUT/<name>.pbm
run() {
  local name=$1 script=$2
  local fifo="$OUT/$name.fifo" frame="$OUT/$name.pbm" log="$OUT/$name.log"
  rm -f "$fifo" "$frame" "$log"
  mkfifo "$fifo"

  # 后台起宿主；输入脚本等宿主 open() 之后再写（FIFO 语义本身就有这个保证）
  ( QZ_DISPLAY=pbm QZ_PBM="$frame" QZ_RPC_SOCK=none \
    QZ_AUTOEXIT_S="$AUTOEXIT_S" QZ_JS_DIR=os/js QZ_APP_DIR=os/js/apps \
    QZ_INPUT0="$fifo" QZ_INPUT1= \
    "$HOST" >"$log" 2>&1 ) &
  local pid=$!

  if [ -n "$script" ]; then
    sleep "$KEY_DELAY_S"
    # 注意 --arch：struct input_event 的字节大小随架构不同（x86_64=24、
    # mips32=16，差在 timeval 里的 time_t 宽度），喂错布局会被静默切成垃圾。
    $REPLAY --arch x86_64 --script "$script" --out "$fifo"
  fi

  wait "$pid" || true
  rm -f "$fifo"
  [ -s "$frame" ] || { echo "  (no frame; host log: $log)"; tail -5 "$log"; return 1; }
}

echo "==> input regression (display=pbm, keyboard via FIFO replay)"
echo

run boot ""              || { echo "FAIL: boot scenario produced no frame" >&2; exit 1; }
run launch "down,enter"   || { echo "FAIL: launch scenario produced no frame" >&2; exit 1; }
run roundtrip "down,enter,back" \
                           || { echo "FAIL: roundtrip scenario produced no frame" >&2; exit 1; }

if cmp -s "$OUT/boot.pbm" "$OUT/launch.pbm"; then
  bad "launch: frame identical to desktop — key never reached the app"
else
  ok "launch: down+enter changed the screen (app launched)"
fi

if cmp -s "$OUT/boot.pbm" "$OUT/roundtrip.pbm"; then
  ok "roundtrip: launch then back reproduced the desktop frame byte-for-byte"
else
  bad "roundtrip: back did not restore the desktop frame"
  cmp "$OUT/boot.pbm" "$OUT/roundtrip.pbm" | head -2
fi

echo
echo "  frames: $OUT/{boot,launch,roundtrip}.pbm"
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
