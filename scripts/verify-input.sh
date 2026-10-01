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

HZ_HOST="${HOST:-$PWD/build-os/qzos-host}"
HZ_REPLAY=os/test/replay-keys.py
HZ_ARCH=x86_64
REPLAY="python3 os/test/replay-keys.py"
# AUTOEXIT_S 只是**超时兜底**。早先每次运行都老老实实等它走完（8s），而实测
# 每个场景只有 4 段按键 × 2.5s 沉降 = 10s 里的前几秒在做事，其余空转。
# 现在改成：等画面静止就停（hz_wait_stable），AUTOEXIT 退成看门狗。
AUTOEXIT_S="${AUTOEXIT_S:-20}"
# 段与段之间的沉降：原来固定 2.5s。这里改成「等画面静止」（下限 0.4s），
# 段与段之间必须留时间是因为同一段按键可能产生多次提交。
HZ_FIFO_DIR=""
. os/test/hostlib.sh
OUT=/tmp/qzos-input
mkdir -p "$OUT"
HZ_FIFO_DIR="$OUT"

[ -x "$HZ_HOST" ] || { echo "missing $HZ_HOST — run scripts/build-os.sh" >&2; exit 1; }

pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

# run <name> <key-script> [key-script ...]  多段脚本会依次投放（等上一段跑完）
run() {
  local name=$1; shift
  local frame="$OUT/$name.pbm" log="$OUT/$name.log"
  rm -f "$frame" "$log"
  hz_start "$log" QZ_DISPLAY=pbm QZ_PBM="$frame" QZ_RPC_SOCK=none \
           QZ_AUTOEXIT_S="$AUTOEXIT_S" QZ_JS_DIR=os/js QZ_APP_DIR=os/js/apps
  if ! hz_wait_desktop 15; then
    echo "  (no desktop commit; tail of $log)"; tail -5 "$log"; hz_stop; return 1
  fi

  local script
  for script in "$@"; do
    if [ -n "$script" ]; then
      # --arch 是必须的：struct input_event 的大小随架构不同（x86_64=24、
      # mips32=16，差在 timeval 里的 time_t 宽度），喂错布局会被静默切成
      # 垃圾事件——测试"通过"而什么也没测。
      $REPLAY --arch x86_64 --script "$script" --out "$(hz_fifo)"
      # 段与段之间必须等画面静止：同一段按键可能产生多次提交
      # （实测 down,down,enter = 桌面 / 焦点移动 / 应用绘制）。
      hz_wait_stable "$log" 500 10 || true
    fi
  done

  hz_stop
  [ -s "$frame" ] || { echo "  (no frame; tail of $log)"; tail -5 "$log"; return 1; }
}

# 该场景一共向屏提交了几次。
# 注意：这是**测量**，所以它本身必须能发现"测量坏了"。曾经把日志格式从
# "commit ..." 改成 "[123 ms] commit ..." 之后，这个 grep 悄悄匹配不到、
# 一律返回 0，而断言是"提交次数 <= 上限"，于是 0 永远通过——闸门变成装饰。
# 所以下面用下限断言兜住：一次启动必须恰好刷 1 次，0 说明测量失效。
commits() { grep -cE "qzos-display: (\[ *[0-9]+ ms\] )?commit " "$OUT/$1.log" || true; }

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
# 按一个**留在屏上**的按钮：这是唯一会渲染出按下/抬起两态的路径，主题动画
# 的额外刷新全都藏在这里。（桌面上的 enter 立刻 ui.clear() 把按钮销毁，
# 按下态根本没被画出来，所以 app1/app2 抓不到主题动画的回归。）
run tap         "enter" "enter"         || { echo "FAIL: tap" >&2; exit 1; }

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

# ---- 3b. 系统服务面（uvrpc）：JS 调 sys.info 必须真的回来 ----
# 覆盖 JSON UI 桥之外的那条腿：postMessage -> bridge op:rpc -> uvrpc INPROC
# server -> 响应回投 JS -> 界面更新。tap 场景的第二次 enter 就是按 hello 的
# "sys.info (rpc)" 按钮。
if differs tap app1; then
  ok "services: sys.info round trip changed the screen (result rendered)"
else
  bad "services: pressing the rpc button changed nothing — the call never came back"
fi

# ---- 4. e-ink 刷新预算：一次按键最多刷一次屏 ----
# 这条挡的是"主题动画"与"光标闪烁"两类回归：LVGL 默认主题给按钮挂了 120ms 的
# style transition，textarea 光标是 400ms 无限循环动画——在 1bpp 墨水屏上，
# 动画 = 每帧一次波形刷新。实测曾出现每敲一键刷 4~5 次、每点一下按钮多刷 4 次
# （都是 50ms 一帧地来回跳，在两种渲染之间反复）。
#
# 期望值写成"1 + 按键数"而不是魔数：首帧刷 1 次，之后**每个按键至多贡献 1 次**
# （方向键移焦点 1 次、回车进应用 1 次、每个字符 1 次）。这样写的好处是它直接
# 表达了 e-ink 的真实约束，而不是把当前 UI 的巧合数字钉死；同时下限检查顺带
# 兜住"测量失效"——曾经日志格式一改，grep 匹配不到、一律返回 0，而上限断言
# 让 0 永远通过，闸门直接变成装饰。
budget() { # <name> <该场景按键数>
  local keys="$2" want got
  want=$((keys + 1))
  got=$(commits "$1")
  if [ "$got" -eq "$want" ]; then
    ok "refresh budget: $1 = $got refreshes for $keys keypress(es) (1 + n)"
  elif [ "$got" -lt "$want" ]; then
    bad "refresh budget: $1 = $got, want $want — measurement or rendering is broken"
  else
    bad "refresh budget: $1 = $got refreshes for $keys keypress(es), want $want — something is animating"
  fi
}
budget boot     0   # 只有首帧
budget app1     1   # enter
budget app2     2   # down, enter
budget back     3   # down, enter, back
budget type-abc 5   # down, enter, a, b, c
budget tap      2   # enter（进应用）, enter（按留在屏上的按钮）

echo
echo "  frames: $OUT/*.pbm   logs: $OUT/*.log"
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
