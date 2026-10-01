# os/test/hostlib.sh — 宿主进程的启停：等判据出现，而不是等超时
#
# 源文件用：  . "$(dirname "$0")/../os/test/hostlib.sh"
#
# ## 为什么有这个文件
#
# 实测：闸门的墙钟时间几乎**精确等于**「host 启动次数 × QZ_AUTOEXIT_S」。
#   test-power-service  3 × 9s = 27s，实测 27.1s
#   test-appauth         2 × 9s = 18s，实测 18.1s
#   test-ipc-surface     1 × 12s = 12s，实测 12.1s
# 真正的工作只有每次 ~2.5s 的开机，其余全是空转等超时。
#
# CPU 侧其实很便宜：空闲 9s 的 host 只用 0.02s CPU（0% 占用）。所以要省的是
# **墙钟**，不是 CPU——而墙钟正是每次改动等全部闸门跑完的时间。
#
# ## 做法与三个约束
#
# 不再靠「固定 sleep + AUTOEXIT 兜底」，而是**等判据自己的那行日志出现**，
# 一出现就干净地停掉宿主。约束：
#
#   1. **必须等到判据行。** 提前停会漏掉后面的输出，于是闸门从「慢」变成「脆」。
#   2. **超时要报错，不能静默继续。** 否则「日志被截断」会被读成「测试通过」——
#      这正是本仓反复栽过的那类：判据读不到东西时，一个宽松的阈值让整体变绿。
#   3. **用 SIGTERM 而不是 SIGKILL。** 宿主有 handler（main.c 的 on_sigint →
#      uv_stop），走正常 exit，stdio 才会 flush。SIGKILL 会丢掉缓冲里的日志行，
#      判据读到的是残缺文件——而残缺文件恰好会让否定判据「通过」。
#
# ## 哨兵必须可证明是最后一条
#
# 早先拿「应用打的第一条结果」当哨兵,结果三个 rpc 都在飞、第二条先到、于是宿主
# 被提前杀掉、第三条的回执永远丢了——**闸门自己引入的竞态**。改成让探针用
# Promise.all 在全部 settle 之后自己打一行 ALLDONE,哨兵等它。这样「等到哨兵」与
# 「所有结果都拿到」是同一件事,不靠投递顺序。
#
# 附带修掉的一个真 bug：多个脚本用 `$$` 给 FIFO 命名，而同一 shell 里 `$$`
# 三次调用相同 → 第二次 mkfifo 撞名失败，且没有 FIFO 的那次 run 静默地什么
# 都不做。hz_start 用自增计数，不会有这个问题。

HZ_PID=""
HZ_LOG=""
HZ_SEQ="${HZ_SEQ:-0}"
# 要跑的命令。默认就是宿主；MIPS 闸门要改成 ("$QEMU" "$BD/qzos-host") 这种
# 带前缀的形式，所以用数组而不是单字符串。
HZ_CMD=()
hz_set_cmd() { HZ_CMD=("$@"); }

# hz_start <log> [KEY=VAL ...]
#   KEY=VAL 追加到宿主环境（QZ_* 之类）。
#   必设环境：QZ_JS_DIR / QZ_APP_DIR / QZ_AUTOEXIT_S（留作超时兜底，不是常态路径）
hz_start() {
  HZ_LOG="$1"; shift
  HZ_SEQ=$((HZ_SEQ + 1))
  HZ_FIFO="${HZ_FIFO_DIR:-/tmp}/hzfifo.$$.$HZ_SEQ"
  mkfifo "$HZ_FIFO"
  : > "$HZ_LOG"
  local cmd=("${HZ_CMD[@]}")
  [ "${#cmd[@]}" -gt 0 ] || cmd=("$HZ_HOST")
  env "$@" QZ_INPUT0="$HZ_FIFO" QZ_INPUT1= "${cmd[@]}" >"$HZ_LOG" 2>&1 &
  HZ_PID=$!
}

# hz_fifo —— 当前这次运行用的输入 FIFO。**回放按键必须用它。**
#
# 调用方若自己另建一个 FIFO 再把路径喂给 replay-keys，按键就写进了没人读的
# 管道：宿主收不到任何输入，看起来像「画面没变化」，而闸门报的是
# 「no post-key commit」——指向完全错误的方向。这个坑我踩过一次。
hz_fifo() { printf '%s' "$HZ_FIFO"; }

# hz_wait <regex> [timeout_s]
#   轮询宿主日志直到 regex 命中。命中返回 0；超时返回 1。
#   超时不自己判 FAIL —— 由调用方决定，因为「没等到」在不同闸门里含义不同。
hz_wait() {
  local re="$1" tmo="${2:-15}"
  local waited=0
  # 每 50ms 一次。日志是 stdio 全缓冲写文件，所以可能出现「内容已产生但尚未
  # flush」的窗口；轮询而非固定 sleep 正是为了跨过这个窗口。
  while [ "$waited" -lt $((tmo * 20)) ]; do
    if grep -qE "$re" "$HZ_LOG" 2>/dev/null; then return 0; fi
    if ! kill -0 "$HZ_PID" 2>/dev/null; then
      # 宿主自己退了（boot 失败等）。最后再查一次日志再报超时，
      # 否则「进程没了」会被读成「日志没写出来」——两者的排查方向不同。
      grep -qE "$re" "$HZ_LOG" 2>/dev/null && return 0
      return 1
    fi
    sleep 0.05
    waited=$((waited + 1))
  done
  grep -qE "$re" "$HZ_LOG" 2>/dev/null
}

# hz_stop —— SIGTERM 让宿主干净退出并 flush 掉 stdio 缓冲，然后回收。
hz_stop() {
  [ -n "$HZ_PID" ] || return 0
  kill -TERM "$HZ_PID" 2>/dev/null || true
  # 给它一点时间走完 uv_stop → exit → flush；超时就 SIGKILL 收尾，
  # 免得一个卡住的进程把整个闸门挂住。
  local i=0
  while kill -0 "$HZ_PID" 2>/dev/null && [ "$i" -lt 40 ]; do sleep 0.05; i=$((i+1)); done
  kill -KILL "$HZ_PID" 2>/dev/null || true
  wait "$HZ_PID" 2>/dev/null || true
  HZ_PID=""
  rm -f "$HZ_FIFO" 2>/dev/null || true
}

# hz_wait_desktop —— 等「桌面首帧已提交」，也就是 shell 画完了。
#
# 哨兵是 qzos-display 的 commit 行，**必须带上时间戳那一段**：
#   qzos-display: [4107910342 ms] commit full wf=full ...
# 而 commit 的格式串里 `%6u ms` 是有的，所以正则要写 `\[.*\] commit`。
# 只写 `commit` 匹配不到——本仓已经三次栽在「grep 模式漏了时间戳」上：
# 数出 0 次，而 `<= N` 类型的判据让 0 永远通过，闸门直接变装饰。
hz_wait_desktop() { hz_wait 'qzos-display: \[.*\] commit' "${1:-15}"; }

# hz_commits <log> —— 数屏幕提交次数。
#
# 正则**必须**容忍时间戳那一段：commit 的格式串里带 `[%6u ms]`，只写
# `commit` 匹配不到。本仓已经三次栽在「grep 模式漏了时间戳」上——数出 0 次，
# 而 `<= N` 类型的判据让 0 永远通过，闸门直接变装饰。
hz_commits() { grep -cE "qzos-display: (\[ *[0-9]+ ms\] )?commit " "$1" 2>/dev/null || true; }

# hz_wait_commits <log> <n> [timeout_s] —— 等到提交次数达到 n
hz_wait_commits() {
  local log="$1" want="$2" tmo="${3:-8}"
  local i=0
  while [ "$i" -lt $((tmo * 20)) ]; do
    [ "$(hz_commits "$log")" -ge "$want" ] && return 0
    sleep 0.05; i=$((i + 1))
  done
  [ "$(hz_commits "$log")" -ge "$want" ]
}

# hz_wait_stable <log> [quiet_ms] [timeout_s]
#   等到「提交次数不再增长」——即画面静止下来。
#
# 为什么不是「按键数 + 1」：一次按键不一定产生视觉变化，而一次按键也可能产生
# **多次**提交。实测 down,down,enter 是 3 次提交：桌面 → 焦点移动 → 应用绘制。
# 按「按键数 + 1」等会在第 2 次（焦点移动）就停机，抓到的是**桌面帧**，
# 于是「标记区墨量 0」——而看起来像产品坏了。
hz_wait_stable() {
  local log="$1" quiet="${2:-600}" tmo="${3:-10}"
  local last=-1 i=0 same=0
  while [ "$i" -lt $((tmo * 20)) ]; do
    local n; n=$(hz_commits "$log")
    if [ "$n" = "$last" ]; then
      same=$((same + 1))
      [ "$same" -ge $((quiet / 50)) ] && return 0
    else
      last="$n"; same=0
    fi
    sleep 0.05; i=$((i + 1))
  done
  return 0   # 超时也算过：画面静止这件事本身不是判据，判据在调用方
}

# hz_keys <script> —— 通过 FIFO 回放按键
hz_keys() {
  python3 "$HZ_REPLAY" --arch "$HZ_ARCH" --script "$1" --out "$HZ_FIFO" >/dev/null 2>&1 || true
}
