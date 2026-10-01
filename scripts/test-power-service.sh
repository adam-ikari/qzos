#!/usr/bin/env bash
# scripts/test-power-service.sh — sys.power.* 接进服务面的闸门
#
# 在此之前 qzos_power_may_act 只是个纯函数 + 43 条单测，`power` 能力授权了也
# 无服务可调。现在它是第一个真的能被调用的电源服务，所以要钉住三件事：
#
#   1. 没 power 能力 → 拒（能力边界）
#   2. 有 power 能力但归属未确认 → 拒，**并把中文理由带回 JS**
#      （这是闸门 0 的 UX：用户必须看得见「为什么不能关机」）
#   3. 归属确认 + 假 sysfs → 动作**真的被执行**（正对照）
#
# 第 3 条是前两条的对照：只测否定项的话，一个「把所有 power 调用都拒掉」的
# 实现也能全绿。
#
# **绝不碰真机路径。** 写动作的目标全部是 $OUT/fake 下的普通文件，由
# QZ_POWER_SHUTDOWN / QZ_POWER_SUSPEND / QZ_POWER_REBOOT 指过去。设备上那些
# sysfs 路径一旦被写，最坏情况是直接掉电、损坏文件系统（P3/P4 未验的原因）。
set -euo pipefail
cd "$(dirname "$0")/.."

HZ_HOST=${HOST:-build-os/qzos-host}
[ -x "$HZ_HOST" ] || { echo "missing $HZ_HOST — run scripts/build-os.sh" >&2; exit 1; }

# 启停用共享 helper：等判据那行日志出现就停，而不是等 QZ_AUTOEXIT_S 走完。
# 3 次运行 × 9s = 27s，而实际工作只有每次 ~2.5s 的开机。
. os/test/hostlib.sh
HZ_REPLAY=os/test/replay-keys.py
HZ_ARCH=x86_64

OUT=$(mktemp -d)
# 调试时把临时目录留住：trap 会把日志一起删掉，而「判据匹配不到」这类问题的
# 排查完全依赖日志本身。QZ_KEEP_TMP=1 时不删。
if [ "${QZ_KEEP_TMP:-0}" = "1" ]; then
  trap 'echo "tmp kept: $OUT"' EXIT
else
  trap 'rm -rf "$OUT"' EXIT
fi
JS2="$OUT/js2"
FAKE="$OUT/fake"
pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

mkdir -p "$JS2/apps/pwr" "$OUT/empty" "$FAKE"
for f in ui.js apkg.js sandbox.js shell.js; do ln -s "$PWD/os/js/$f" "$JS2/$f"; done

# 假 sysfs：可写的普通文件。写成功 = 动作真的被执行了。
: > "$FAKE/shutdown"; : > "$FAKE/suspend"; : > "$FAKE/reboot"
printf '42\n' > "$FAKE/capacity"; printf 'Discharging\n' > "$FAKE/state"

# pwr 的 perms 由下面的 PROBE_MODE 决定：noint → 0 能力；power → 有 power。
# 桌面上只有它一个应用，焦点在第 1 行。
mk_probe() {  # mk_probe <perms-json>
  printf '{"schema":1,"id":"pwr","name":"PowerProbe","version":"1.0.0","api":1,"entry":"app.js","perms":%s}\n' "$1" \
    > "$JS2/apps/pwr/app.json"
  cat > "$JS2/apps/pwr/app.js" <<'EOF'
/* 三个调用**同时**发出：宿主只认「此刻的活动应用」，串行等结果会让中途插入的
 * 消息改掉上下文。
 *
 * 哨兵是 ALLDONE，由 Promise.all 在**三个都 settle 之后**打出。不能用「第一条
 * 结果」当哨兵：投递顺序不保证。早先就因为第二条先到而提前停机、把第三条的
 * 回执丢了——闸门自己引入的竞态。 */
console.log('PERMS=' + JSON.stringify(api.perms));

var p1 = ui.rpc('sys.power.state', {}).then(function (r) {
  console.log('STATE owner=' + r.key_owner + ' backend=' + r.backend +
              ' percent=' + r.percent +
              ' shutdown_allowed=' + r.actions.shutdown.allowed +
              ' shutdown_reason=' + r.actions.shutdown.reason);
}, function (e) { console.log('STATE DENIED ' + JSON.stringify(e.message || e)); });

var p2 = ui.rpc('sys.power.request', { action: 'shutdown' }).then(function (r) {
  console.log('REQ ' + JSON.stringify(r));
}, function (e) { console.log('REQ REJECTED ' + JSON.stringify(e.message || e)); });

/* 拼错的动作名：必须与「已知但被拒」区分开 */
var p3 = ui.rpc('sys.power.request', { action: 'halt' }).then(function (r) {
  console.log('ALLDONE' + JSON.stringify(r));
}, function (e) { console.log('TYPO REJECTED ' + JSON.stringify(e.message || e)); });

Promise.all([p1, p2, p3]).then(function () { console.log('ALLDONE'); });
EOF
}

RUN_N=0
HZ_FIFO_DIR="$OUT"

# run_host <log> <等到的判据行> —— 开机 → 回放 enter → 等判据出现 → 干净停机。
#
# AUTOEXIT_S 仍然设，但它是**超时兜底**不是常态路径：hz_wait 先等到判据出现，
# 所以正常情况下每次 3 秒内结束。
run_host() {
  local log="$1" waitfor="$2"
  hz_start "$log" QZ_DISPLAY=pbm QZ_PBM="$OUT/frame.pbm" QZ_RPC_SOCK=none \
           QZ_JS_DIR="$JS2" QZ_APP_DIR="$OUT/empty" QZ_AUTOEXIT_S=20
  if ! hz_wait_desktop 15; then
    bad "宿主没提交桌面首帧就退出了（日志见 $log）"
    hz_stop
    return 1
  fi
  hz_keys enter
  if ! hz_wait "$waitfor" 15; then
    bad "等不到判据行 /$waitfor/ —— 日志被截断或应用没跑到那一步（见 $log）"
    hz_stop
    return 1
  fi
  hz_stop
}

# ================= 场景 1：没有 power 能力 =================
mk_probe '[]'
run_host "$OUT/nocap.log"    'ALLDONE'

if grep -q "STATE owner=" "$OUT/nocap.log"; then
  bad "零能力应用竟调通了 sys.power.state"
else
  ok "零能力应用调 sys.power.state：被拒"
fi
if grep -q "STATE DENIED" "$OUT/nocap.log"; then
  ok "state 有明确拒绝回执（不是静默挂死）"
else
  bad "state 没有任何回执"
fi
if grep -q "REQ REJECTED" "$OUT/nocap.log"; then
  ok "request 有明确拒绝回执"
else
  bad "request 没有任何回执"
fi
# 关键安全断言：被拒 ≠ 只是没回执，而是**没写文件**
if [ -s "$FAKE/shutdown" ]; then
  bad "零能力应用触发了关机写动作 —— 文件里有内容"
else
  ok "零能力应用没有写任何东西（关机文件仍为空）"
fi

# ================= 场景 2：有 power 能力，但归属未确认 =================
mk_probe '["power"]'
# 关键：QZ_POWER_KEY_OWNER 不设 → 描述符默认是 unknown → may_act 全拒。
# 这正是真机现状（闸门 0 未查清），所以这一档必须是最常走到的路径。
QZ_POWER_STATE="$FAKE/state" QZ_POWER_CAPACITY="$FAKE/capacity" \
QZ_POWER_SHUTDOWN="$FAKE/shutdown" QZ_POWER_SUSPEND="$FAKE/suspend" \
QZ_POWER_REBOOT="$FAKE/reboot" \
  run_host "$OUT/unknown.log" 'ALLDONE'

if grep -q "STATE owner=unknown" "$OUT/unknown.log"; then
  ok "归属未确认时 state 如实报 owner=unknown（不编一个值）"
else
  bad "state 没报 owner=unknown：$(grep -o 'STATE owner=[a-z]*' "$OUT/unknown.log" | head -1)"
fi
if grep -q "shutdown_allowed=false" "$OUT/unknown.log"; then
  ok "归属未确认 → shutdown 不被允许"
else
  bad "归属未确认却允许了 shutdown"
fi
# 理由必须**带回 JS**：用户得知道为什么按不动
if grep -q "shutdown_reason=电源键归属未确认" "$OUT/unknown.log"; then
  ok "拒绝理由原样带回 JS（闸门 0 的 UX 靠它）"
else
  bad "拒绝理由没回来：$(grep -o 'shutdown_reason=[^ ]*.*' "$OUT/unknown.log" | head -1)"
fi
# 探针那边做了 JSON.stringify，所以日志里是 {\\"ok\\":false,...} 这种转义形态。
# 早先的判据在原始日志里找 '"ok":false,...' 的字面量 —— 永远匹配不到，于是这条
# 断言**恒定失败**（而我一度以为它揭示了「拒绝回执形状不对」）。判据要跟探针
# 实际打印的形态对齐，或者干脆让探针不打转义形态。
# 用 grep -F 字面量。不要写 'error.....refused' 这种数点的正则：探针那边
# JSON.stringify 并不转义引号（实测输出就是原始 "ok":false），点数猜错就
# 永远匹配不到——而一条永远匹配不到的**否定**判据会恒定失败，读起来却像
# 「产品行为不对」。判据要能被人一眼验证。
if grep -qF '"error":"refused"' "$OUT/unknown.log"; then
  ok "request 返回 refused（而不是 ok:true 假装成功）"
else
  bad "request 的拒绝回执形状不对：$(grep -o 'REQ .*' "$OUT/unknown.log" | head -1)"
fi
# ok 字段必须是 false：refused 若带着 ok:true，调用方按 ok 分支就会以为成功了
if grep -qF 'REQ {"ok":false' "$OUT/unknown.log"; then
  ok "refused 回执带 ok:false（调用方有一个确定的分支依据）"
else
  bad "refused 回执里没有 ok:false：$(grep -o 'REQ .*' "$OUT/unknown.log" | head -1)"
fi
if [ -s "$FAKE/shutdown" ]; then
  bad "归属未确认却执行了关机写动作 —— 这是最危险的一档（P3/P4 未验）"
else
  ok "归属未确认时关机文件仍为空（唯一不可协商的不变量）"
fi

# ================= 场景 3：归属确认 + 假 sysfs =================
# 正对照。缺了它，场景 1/2 的 denied 都可以是「所有 power 调用都被拒」造成的。
mk_probe '["power"]'
QZ_POWER_KEY_OWNER=none \
QZ_POWER_STATE="$FAKE/state" QZ_POWER_CAPACITY="$FAKE/capacity" \
QZ_POWER_SHUTDOWN="$FAKE/shutdown" QZ_POWER_SUSPEND="$FAKE/suspend" \
QZ_POWER_REBOOT="$FAKE/reboot" \
  run_host "$OUT/owned.log"   'ALLDONE'

if grep -q "STATE owner=none" "$OUT/owned.log"; then
  ok "归属确认后 state 报 owner=none"
else
  bad "归属确认后 state 仍报 unknown"
fi
if grep -q "shutdown_allowed=true" "$OUT/owned.log"; then
  ok "归属确认 + 给了 shutdown 路径 → 允许关机"
else
  bad "归属确认后仍不允许关机：$(grep -o 'shutdown_allowed=[a-z]*' "$OUT/owned.log" | head -1)"
fi
# 判据是「读到了假文件里那个 42」，不是「不是 -1」。给个真数字才测得出
# 「它在读」和「它在编默认值」的区别——写 0 不行，0 与「未知」在 -1 的
# 约定下长得太近；而空文件会合法地得到 -1（读不到就说读不到）。
if grep -q "percent=42" "$OUT/owned.log"; then
  ok "容量读到了假文件里的 42（是读出来的，不是编的默认值）"
else
  bad "percent 没读出来：$(grep -o 'percent=[-0-9]*' "$OUT/owned.log" | head -1)"
fi
# 判据落在**效果**上：假 sysfs 文件里真的被写进了内容
if grep -q "1" "$FAKE/shutdown" 2>/dev/null && [ -s "$FAKE/shutdown" ]; then
  ok "关机写动作真的执行了（假 sysfs 文件里有内容）"
else
  bad "归属确认后关机仍未执行 —— 服务面可能根本没接上 power"
fi
# 拼错的动作名必须与「已知但被拒」区分
if grep -q '"error":"unknown action"' "$OUT/owned.log"; then
  ok "拼错动作名 → unknown action（与 refused 区分开）"
else
  bad "拼错动作名的回执不对：$(grep -o 'TYPO .*' "$OUT/owned.log" | head -1)"
fi

printf '  %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
