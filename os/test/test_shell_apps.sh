#!/usr/bin/env bash
# os/test/test_shell_apps.sh — shell 应用模型的端到端闸门
#
# 覆盖 os/js/apkg.js 接入 shell 之后那些「纯逻辑单测测不到」的部分：
#   1. 坏包（id 与目录名不符 / 表外能力）被**列出来但拒绝启动**
#   2. 坏包的代码**没有**执行（画面判据，不是日志判据）
#   3. 桌面帧逐字节可复现（e-ink：帧是状态的纯函数）
#   4. 装面之后 shell 自己没被误伤
#
# 判据为什么落在画面上：`/dev/epaper_lcd` 只写不可读，屏上一切只能靠帧判断。
# 而「看日志里有没有 launch 失败」是**回执层面**的断言——回执是 dispatch 路径
# 无条件产出的，与引擎有没有真拒绝毫无关系（qzjs brain 里的老教训：interrupt
# 那条测试就是这么全绿的）。所以这里判画面。
#
# 为什么不用全局墨量阈值：某个控件重排或多一个正常应用，都能让全局墨量涨
# 同样的量级，数值上区分不开——那样的判据是装饰。所以：
#   - fixture 应用把标记画在**桌面上本来空白**的一行（y=120），判据就干净了；
#   - 配一条**正对照**（同一个包，manifest 修对），先证明「这块地方真能出现
#     墨」，再要求坏包场景这块**没有**墨。只测否定项的话，一个把所有包都
#     拒掉的实现也能全绿。
set -euo pipefail
# 本脚本在 os/test/ 下，要上溯两级才到仓库根（build-os/、scripts/ 都在那）。
cd "$(dirname "$0")/../.."

HOST=${HOST:-build-os/qzos-host}
[ -x "$HOST" ] || { echo "missing $HOST — run scripts/build-os.sh" >&2; exit 1; }
VIEW="python3 os/test/pbm_view.py"

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

# 标记区 = fixture 应用自己画字的那一行。
#
# 选 y=126 是因为这是**唯一**在「桌面 / 详情页 / 正常应用」三种画面上都不会
# 被别的东西占用的行：桌面只有标题(y≈4..25) + 3 行按钮(y=30..103)；详情页是
# 标题 + 原因行(y≈34..) + back 按钮(y=100..122)。y≥126 是空白带。
# 之前取 y=120 撞上 back 按钮下沿，正对照只有 170 墨点而负判据 581——
# 阈值不是猜出来的，是被这一条实测顶回去的。
MARK_Y=126
MARK="4,$MARK_Y,288,24"
ink_at() { $VIEW --region "$1" --at "$2" | sed -n 's/.*-> \([0-9]*\) ink px/\1/p'; }
region_fail() {  # region_fail <pbm> <rect> <max> —— 供变异测试用
  $VIEW --region "$1" --at "$2" --max "$3" >/dev/null 2>&1
}

# ---- fixture：坏包 ----
# 三重违规：id 与目录名不符、声明表外能力 sudo、api 正常。
# 任何一条校验被漏掉，它都会被装进列表。
mkdir -p "$OUT/apps/badpkg"
cat > "$OUT/apps/badpkg/app.json" <<'EOF'
{ "schema": 1, "id": "WRONG-NAME", "name": "Should Not Launch",
  "version": "1.0.0", "api": 1, "entry": "app.js", "perms": ["sudo"] }
EOF
cat > "$OUT/apps/badpkg/app.js" <<EOF
ui.create('label', { id: 'boom', text: 'BADPKG-RAN', x: 4, y: $MARK_Y, w: 288, font: 'md' });
ui.refresh(true);
EOF

# ---- 正对照：同一个包，**只**把 manifest 修对（id 对齐 + 能力合法） ----
# 关键：app.js 一字不改。这样两条路径的**唯一**差异就是 manifest 合法性，
# 判据隔离的就是「校验有没有生效」。如果正对照另造一个包、画不同的字，
# 那测出来的是两个不同的东西，不是同一个开关的正反两面。
mkdir -p "$OUT/ctl/badpkg"
sed 's/"WRONG-NAME"/"badpkg"/; s/"perms": \["sudo"\]/"perms": ["info"]/' \
  "$OUT/apps/badpkg/app.json" > "$OUT/ctl/badpkg/app.json"
cp "$OUT/apps/badpkg/app.js" "$OUT/ctl/badpkg/app.js"
# 顺带验证 sed 真的改对了（否则正对照会因同样的原因被拦，判据全失效）
grep -q '"id": "badpkg"' "$OUT/ctl/badpkg/app.json" || {
  echo "正对照 manifest 生成失败" >&2; exit 1; }

REPLAY=${REPLAY:-os/test/replay-keys.py}

# run_desk <app_dir> <name> [keys]
# 桌面只**列**应用，应用自己的画面要 launch 之后才出现。所以要判「坏包有没有
# 被执行」，必须真的把焦点移到它并回车——只截桌面帧是测不到执行与否的。
# 按键走 FIFO + replay-keys.py（struct input_event 的布局随架构不同，--arch
# 必须显式声明消费者架构，见 verify-input.sh 的注释）。
run_desk() {
  local appdir="$1" name="$2" keys="${3:-}"
  local fifo="$OUT/$name.fifo" frame="$OUT/$name.pbm" log="$OUT/$name.log"
  rm -f "$fifo" "$frame" "$log"
  mkfifo "$fifo"
  ( QZ_DISPLAY=pbm QZ_PBM="$frame" QZ_RPC_SOCK=none QZ_JS_DIR=os/js \
    QZ_APP_DIR="$appdir" QZ_AUTOEXIT_S=9 QZ_INPUT0="$fifo" QZ_INPUT1= \
    "$HOST" >"$log" 2>&1 ) &
  local pid=$!
  if [ -n "$keys" ]; then
    # 延时要够桌面把应用扫完并渲染出来（与 verify-input.sh 同量级）
    sleep 2.5
    python3 "$REPLAY" --arch x86_64 --script "$keys" --out "$fifo"
  fi
  wait "$pid" || true
  [ -s "$frame" ] || { echo "no frame for $name; log:" >&2; tail -20 "$log" >&2; exit 1; }
}

# 第 3 行（goodpkg / badpkg）需要 down,down,enter
# 正对照：同一个包、只把 manifest 修对 → 它**应该**能启动，标记出现在标记区。
run_desk "$OUT/ctl"  ctl  "down,down,enter"
# 负判据：坏 manifest → 焦点移到它并回车。若校验生效，进的是**详情页**
# （显示 id-mismatch,perm-unknown），BADPKG-RAN 永不出现。
run_desk "$OUT/apps" desk "down,down,enter"

desk_ink=$(ink_at "$OUT/desk.pbm" "$MARK")
ctl_ink=$(ink_at "$OUT/ctl.pbm" "$MARK")

# ---- 1. 坏包被 shell **看见**了（否则下面全是在测「它压根没被发现」） ----
if grep -q "3 apps" "$OUT/desk.log"; then
  ok "坏包被统计进应用列表（3 apps）——它是被校验拦下的，不是没被发现"
else
  bad "坏包没进列表（$(grep -o '[0-9]* apps' "$OUT/desk.log" | head -1)）——闸门没在测该测的东西"
fi

# ---- 2. 正对照：只修 manifest（app.js 不变），标记**必须**出现 ----
# 这是整条闸门的支点。只测「坏包没出现」的话，一个「把所有包都拒掉」的实现
# 也能全绿——所以先证明同一个包在 manifest 合法时确实能跑起来。
if [ "${ctl_ink:-0}" -gt 100 ]; then
  ok "正对照成立：manifest 合法时同一应用真的执行了（标记区墨量 ${ctl_ink}）"
else
  bad "正对照不成立（标记区墨量 ${ctl_ink}）——区域判据测不动，下一条断言无意义"
fi

# ---- 3. 负判据：坏 manifest 时，同一应用的代码**没有**执行 ----
# 阈值取正对照的一半（不是 0）：正对照实测 170 墨点，那是 10 个 12px 字符。
# 用 0 的话，任何一丁点残留——包括 label 的边框、anti-alias 都不存在的
# 1bpp 上的一颗散点——都会让这条飘红，而那种红不指向任何真问题。
if [ "${desk_ink:-999}" -lt $(( ${ctl_ink:-170} / 2 )) ]; then
  ok "坏包未执行（标记区墨量 ${desk_ink} < 正对照的一半，BADPKG-RAN 不在画面上）"
else
  bad "坏包执行了：标记区墨量 ${desk_ink}（正对照 ${ctl_ink}，BADPKG-RAN 出现在画面上）"
fi

# ---- 4. 桌面帧可复现（e-ink：帧是状态的纯函数） ----
run_desk "$OUT/apps" desk2 "down,down,enter"
if cmp -s "$OUT/desk.pbm" "$OUT/desk2.pbm"; then
  ok "两次独立运行的桌面帧逐字节一致"
else
  bad "桌面帧不可复现（同一状态两次跑出不同画面）"
fi

# ---- 5. 装面之后系统自己没被误伤：正常应用仍列在桌面上 ----
# 判据是应用行区域有墨（Hello / Notepad 两个按钮），不是全局墨量：桌面空屏和
# 「有按钮但没字」都能骗过全局阈值。
btn_ink=$($VIEW --region "$OUT/desk.pbm" --at 8,41,280,16 --at 8,66,280,12 \
          | sed -n 's/.*-> \([0-9]*\) ink px/\1/p' | sort -rn | head -1)
if [ "${btn_ink:-0}" -gt 100 ]; then
  ok "授权面装上后 shell 仍能列出正常应用（应用行墨量 ${btn_ink}）"
else
  bad "装面后桌面异常空（应用行墨量 ${btn_ink}）——面把系统自己砍了"
fi

# ---- 6. 给人留一张能直接看的图 ----
$VIEW "$OUT/desk.pbm" "$OUT/desk.png" --scale 3 >/dev/null
$VIEW "$OUT/ctl.pbm"  "$OUT/ctl.png"  --scale 3 >/dev/null
cp "$OUT/desk.png" "$OUT/ctl.png" /tmp/opencode/ 2>/dev/null || true
echo "  ..  目检图: /tmp/opencode/desk.png（坏包场景） /tmp/opencode/ctl.png（正对照）"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
