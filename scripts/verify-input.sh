#!/usr/bin/env bash
# scripts/verify-input.sh — 键盘通路回归（无头、无设备）
#
# 补的是什么洞：这台设备**没有触摸**，键盘是唯一输入通路，而它是整条链上
# 最没有自动化覆盖的一段——evdev -> uv_poll -> 事件队列 -> LVGL keypad ->
# group 焦点 -> JSON click/value 事件 -> JS shell 生命周期。任何一环静默断掉，
# 现象都是同一个：「按了没反应」。没有闸门就只能靠人肉在屏上试。
#
# 怎么无头驱动：QZ_INPUT0 指向一个 FIFO，os/test/replay-keys.py 按真实
# struct input_event 布局写进去，宿主照常走 uv_poll 读——和真机同一条路，
# 只是事件源从内核换成脚本。
#
# 场景断言语义，不存黄金文件——所以"改了 UI 样式导致帧变化"不会被误报成
# bug，而"方向键不动焦点"这种真 bug 一定会被抓到（见 nav 断言）。
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${HOST:-$PWD/build-os/qzos-host}"
REPLAY="python3 os/test/replay-keys.py"
AUTOEXIT_S="${AUTOEXIT_S:-8}"
KEY_DELAY_S="${KEY_DELAY_S:-2.5}"
OUT=/tmp/qzos-input
mkdir -p "$OUT"

[ -x "$HOST" ] || { echo "missing $HOST — run scripts/build-os.sh" >&2; exit 1; }

pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

# run <name> <key-script> [key-script ...]  多段脚本会依次投放（等上一段跑完）
run() {
  local name=$1; shift
  local fifo="$OUT/$name.fifo" frame="$OUT/$name.pbm" log="$OUT/$name.log"
  rm -f "$fifo" "$frame" "$log"
  mkfifo "$fifo"

  ( QZ_DISPLAY=pbm QZ_PBM="$frame" QZ_RPC_SOCK=none \
    QZ_AUTOEXIT_S="$AUTOEXIT_S" QZ_JS_DIR=os/js QZ_APP_DIR=os/js/apps \
    QZ_INPUT0="$fifo" QZ_INPUT1= \
    "$HOST" >"$log" 2>&1 ) &
  local pid=$!

  local script
  for script in "$@"; do
    if [ -n "$script" ]; then
      sleep "$KEY_DELAY_S"
      # --arch 是必须的：struct input_event 的大小随架构不同（x86_64=24、
      # mips32=16，差在 timeval 里的 time_t 宽度），喂错布局会被静默切成
      # 垃圾事件——测试"通过"而什么也没测。
      $REPLAY --arch x86_64 --script "$script" --out "$fifo"
    fi
  done

  wait "$pid" || true
  rm -f "$fifo"
  [ -s "$frame" ] || { echo "  (no frame; tail of $log)"; tail -5 "$log"; return 1; }
}

# 该场景一共向屏提交了几次
commits() { grep -c "qzos-display: commit" "$OUT/$1.log" || true; }

same()     { cmp -s "$OUT/$1.pbm" "$OUT/$2.pbm"; }
differs()  { ! same "$1" "$2"; }

echo "==> input regression (display=pbm, keyboard via FIFO replay)"
echo

run boot        ""                      || { echo "FAIL: boot" >&2; exit 1; }
run app1        "enter"                 || { echo "FAIL: app1" >&2; exit 1; }
run app2        "down,enter"            || { echo "FAIL: app2" >&2; exit 1; }
run back        "down,enter,back"       || { echo "FAIL: back" >&2; exit 1; }
run type-abc    "down,enter" "a,b,c"    || { echo "FAIL: type-abc" >&2; exit 1; }
run type-xy     "down,enter" "x,y"      || { echo "FAIL: type-xy" >&2; exit 1; }

# ---- 1. 导航：方向键必须真的移动焦点 ----
# 这条曾经"通过"但什么都没测：早期版本里方向键被原样交给 LVGL，而 v9 的
# keypad 只认 NEXT/PREV（Tab/PageUp）移动焦点，设备键盘又没有这两个键——
# 于是 down 是个空操作，enter 启动的始终是第一个应用，而"帧变了"照样成立。
if differs app1 app2; then
  ok "nav: down reached a different app than enter alone"
else
  bad "nav: down,enter and enter produced the same frame — arrow key is a no-op"
fi

# ---- 2. 生命周期：进应用再退回来必须复现同一帧 ----
# e-ink 每帧提交的内容应是状态的纯函数；A 与 C 是两次独立运行，不是自证。
if same boot back; then
  ok "lifecycle: launch then back reproduced the desktop frame byte-for-byte"
else
  bad "lifecycle: back did not restore the desktop frame"
  cmp "$OUT/boot.pbm" "$OUT/back.pbm" 2>&1 | head -2
fi

# ---- 3. 文本输入：字符必须真的进到控件里 ----
# 不比对字形（换字体就会误报），而是问"文本内容影响画面"：不同字符串必须
# 给出不同帧，且都不能等同于空文本的帧。
if differs type-abc type-xy; then
  ok "typing: different strings produce different frames"
else
  bad "typing: 'abc' and 'xy' rendered identically — characters are not landing"
fi
if differs type-abc app2 && differs type-xy app2; then
  ok "typing: typed text changes the frame vs the empty app"
else
  bad "typing: typed text did not change the frame at all"
fi

# ---- 4. e-ink 刷新预算：打字的代价必须是「每个字符一次」 ----
# 这条挡的是"光标闪烁"这类回归：LVGL 的 textarea 光标是无限循环动画，闪一下
# 就刷一次屏，在 1bpp 墨水屏上等于每半秒一次全屏波形。实测曾出现每敲一键刷
# 4~5 次。这里给的是硬上限，不是黄金值。
budget_check() { # <name> <允许的最大提交数>
  local got; got=$(commits "$1")
  if [ "$got" -le "$2" ]; then
    ok "refresh budget: $1 committed $got times (max $2)"
  else
    bad "refresh budget: $1 committed $got times, max $2 — something is animating"
  fi
}
# boot=1，enter 进应用=1；type-abc 再加 3 个字符 + 少量余量
budget_check boot     1
budget_check app1     2
budget_check type-abc 7

echo
echo "  frames: $OUT/*.pbm   logs: $OUT/*.log"
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
