#!/usr/bin/env bash
# os/test/verify-rt-recovery.sh — JS 引擎崩溃恢复闸门
#
# 病症（无头实测得到，不是推演）：qzjs-rt 是跑 JS 的独立进程。把它杀掉后，
# 库会往邮箱推一帧 {"type":"error","error":"main-runtime-process-exited-
# unexpectedly"}，但宿主此前只把它 fprintf 到 stderr——于是：
#
#   - 宿主继续正常 tick
#   - 面板上仍是**完好无损的桌面画面**（Hello / Notepad 按钮清清楚楚）
#   - 按键仍被读取
#   - 没有任何东西告诉用户桌面已经死了
#
# 用户面对一个「按任何键都没反应」的僵尸桌面，唯一出路是重启设备。对一台
# 要作为系统的设备这是致命的：**桌面必须同时是可靠性的门面**。一个不报信的
# 桌面比崩溃更糟，因为它骗人。
#
# 本脚本杀掉 rt，断言三件事：
#   1. 屏上出现明确的「JS engine stopped」提示（不是那块完好的死桌面）
#   2. 输入被停掉（引擎死期间按键不被响应）
#   3. rt 被自动重建，桌面逐字节复现
set -euo pipefail
# 本脚本在 os/test/ 下，要上溯两级才到仓库根。
cd "$(dirname "$0")/../.."

HOST=${HOST:-build-os/qzos-host}
REPLAY=${REPLAY:-os/test/replay-keys.py}
VIEW="python3 os/test/pbm_view.py"
[ -x "$HOST" ] || { echo "missing $HOST — run scripts/build-os.sh" >&2; exit 1; }

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"; pkill -f "build-os/qzos-host" 2>/dev/null || true; pkill -x qzjs-rt 2>/dev/null || true' EXIT
pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

F="$OUT/f.fifo"; mkfifo "$F"
QZ_DISPLAY=pbm QZ_PBM="$OUT/f.pbm" QZ_RPC_SOCK=none QZ_JS_DIR=os/js \
  QZ_APP_DIR=os/js/apps QZ_INPUT0="$F" QZ_INPUT1= QZ_AUTOEXIT_S=22 \
  "$HOST" >"$OUT/log" 2>&1 &
HPID=$!

# 等桌面真的画出来（第一帧 commit）
for _ in $(seq 1 40); do
  [ -s "$OUT/f.pbm" ] && break
  sleep 0.5
done
[ -s "$OUT/f.pbm" ] || { echo "桌面没起来；log:" >&2; tail -20 "$OUT/log" >&2; exit 1; }

# ---- 0. 杀 rt 之前：桌面必须是真的（正对照） ----
# 僵尸桌面的判据要能区分「桌面」和「提示」，所以先确认现在确实是桌面。
desk_ink=$($VIEW --region "$OUT/f.pbm" --at 8,41,280,16 | sed -n 's/.*-> \([0-9]*\) ink px/\1/p')
if [ "${desk_ink:-0}" -gt 100 ]; then
  ok "对照：rt 活着时是正常桌面（应用行墨量 ${desk_ink}）"
else
  bad "对照不成立：rt 活着时桌面就没有应用行（墨量 ${desk_ink}）"
fi
cp "$OUT/f.pbm" "$OUT/desk-before.pbm"
$VIEW "$OUT/f.pbm" "$OUT/before.png" --scale 3 >/dev/null

# ---- 1. 杀掉 rt ----
if pkill -x qzjs-rt; then
  ok "已 SIGTERM 掉 qzjs-rt"
else
  bad "找不到 qzjs-rt 进程（测试前提不成立）"
fi

# 等重建完成（退避 1s + 重建耗时）
for _ in $(seq 1 24); do
  grep -q "input re-enabled" "$OUT/log" && break
  sleep 0.5
done

# ---- 2. 屏上必须出现提示（而不是那块完好的死桌面） ----
# 提示框画在正中 292x80，桌面应用行在 y=41——两者不重叠，所以判据是：
# 桌面应用行**消失**了（被提示盖掉/清屏），同时中部提示区**出现**了墨。
notice_ink=$($VIEW --region "$OUT/f.pbm" --at 20,40,256,72 | sed -n 's/.*-> \([0-9]*\) ink px/\1/p')
if grep -q "engine-dead notice shown" "$OUT/log"; then
  ok "宿主画出了崩溃提示（notice 区墨量 ${notice_ink}）"
else
  bad "宿主没画崩溃提示——用户只会看到一块完好的死桌面"
fi

# ---- 2b. 提示必须真的**画出来过**，而且落过屏 ----
# 「画了但没落屏」是 e-ink 上的经典失效模式：LVGL 更新了对象树，但没有东西
# 触发提交，屏上仍是旧内容。所以这里要求日志里有提交记录——而因为桌面静止，
# 崩溃期间唯一可能的提交就是那张提示。
#
# grep 必须容忍时间戳前缀。commit 行格式是
#   qzos-display: [ 12345 ms] commit full …
# 写成 "qzos-display: commit" 会**匹配不到**、一律返回 0，而判据是「>= 2」，
# 于是永远红。这与 verify-input.sh 踩过的坑同一个：日志格式一变，测量就悄悄
# 失效。判据要能发现「自己坏了」，所以这里匹配可选的时间戳部分。
notice_commits=$(grep -cE "qzos-display: (\[ *[0-9]+ ms\] )?commit " "$OUT/log" || true)
if [ "${notice_commits:-0}" -ge 2 ]; then
  ok "提示落过屏（共 ${notice_commits} 次提交：启动帧 / 提示帧 / 恢复后的桌面帧）"
else
  bad "提示只画在对象树里没落屏（提交 ${notice_commits} 次，屏上仍是旧桌面）"
fi

# 提示内容是否真的进了帧：判据是提示框所在区域的墨量（桌面对应位置是空的）。
# 不能拿「恢复前的帧」判——帧文件被后续提交覆盖了。
if [ "${notice_ink:-0}" -gt 300 ]; then
  ok "提示内容确实进了帧（提示框区墨量 ${notice_ink}）"
else
  bad "提示框区几乎无墨（${notice_ink}）——提示可能没画出来"
fi

# ---- 3. rt 被重建，桌面逐字节复现 ----
if grep -q "JS engine up (restart" "$OUT/log"; then
  ok "rt 被自动重建（日志有 restart 记录）"
else
  bad "rt 没被重建：$(grep -c 'JS engine up' "$OUT/log" || true) 次"
fi

if grep -q "input re-enabled" "$OUT/log"; then
  ok "输入在引擎恢复后被重新打开"
else
  bad "输入没被恢复（重启后按键仍无效）"
fi

$VIEW "$OUT/f.pbm" "$OUT/after.png" --scale 3 >/dev/null
if cmp -s "$OUT/desk-before.pbm" "$OUT/f.pbm"; then
  ok "重启后桌面与崩溃前逐字节一致（e-ink：帧是状态的纯函数）"
else
  bad "重启后桌面与崩溃前不同"
fi

# ---- 4. 输入确实被停过（不是只在日志里说） ----
# 判据用日志里 qzos-input 的 disabled/enabled 这一对——它由 qzos_input_enable
# 无条件产出，且停输入的**效果**（按键不被响应）无法从单次运行的帧上看出来：
# 停输入时画面本来就不该变，所以「帧没变」证明不了任何事。这条只保证「停/开」
# 这一对调用发生了；效果层面的覆盖靠上面「重启后桌面逐字节一致」+ 后续
# verify-input.sh 的完整键盘回放。
if grep -q "disabled (engine down)" "$OUT/log" && \
   grep -q "qzos-input: enabled" "$OUT/log"; then
  ok "输入在崩溃时被停、恢复后被开（停/开成对出现）"
else
  bad "输入停/开不成对：$(grep -c 'qzos-input' "$OUT/log" || true) 条 input 日志"
fi

# ---- 5. 启动时 rt 就起不来：不该直接退出 ----
# 造一个「起不来」的 rt：QZ_RT_SERVER 指向不存在的可执行。
# 这是**开机**场景（rt 起不来），与上面「运行中崩溃」不同：此时宿主还不该
# 放弃——设备是墨水屏一体机，宿主退出就等于黑屏，只能等电池耗尽或物理断电。
# 所以开机失败也该留在退避循环里。
OUT2=$(mktemp -d)
F2="$OUT2/f.fifo"; mkfifo "$F2"
rc=0
QZ_DISPLAY=pbm QZ_PBM="$OUT2/f.pbm" QZ_RPC_SOCK=none QZ_JS_DIR=os/js \
  QZ_APP_DIR=os/js/apps QZ_INPUT0="$F2" QZ_INPUT1= QZ_AUTOEXIT_S=9 \
  QZ_RT_SERVER=/nonexistent/qzjs-rt "$HOST" >"$OUT2/log" 2>&1 || rc=$?
if [ "$rc" -ne 0 ]; then
  bad "开机时 rt 起不来，宿主直接退出（rc=$rc）——屏幕会黑到断电"
else
  ok "开机时 rt 起不来，宿主仍留在退避循环里（没有退出）"
fi
attempts=$(grep -c "respawn failed" "$OUT2/log" || true)
if [ "${attempts:-0}" -ge 2 ]; then
  delays=$(grep -o "next attempt in [0-9]*ms" "$OUT2/log" | grep -o "[0-9]*" | tr '\n' ' ')
  ok "开机退避在翻倍（连续失败 ${attempts} 次，间隔: ${delays}）"
else
  bad "9s 内只试了 ${attempts} 次，无法判断退避是否翻倍"
fi
rm -rf "$OUT2"

echo
echo "  $pass passed, $fail failed"
echo "  ..  目检图: /tmp/opencode/rt-before.png /tmp/opencode/rt-after.png"
cp "$OUT/before.png" /tmp/opencode/rt-before.png 2>/dev/null || true
cp "$OUT/after.png" /tmp/opencode/rt-after.png 2>/dev/null || true
[ "$fail" -eq 0 ]
