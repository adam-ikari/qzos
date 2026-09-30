#!/usr/bin/env bash
# scripts/verify-mips-e2e.sh — MIPS 产物在 qemu-user 下跑端到端
#
# 补 verify-frames.sh 的缺口：那条只比**启动那一帧**的字节一致，而新加的
# 路径（rt 崩溃恢复、授权边界、坏包详情页）全都在启动之后 —— 也就是说
# 「原生验过、MIPS 没验」的正好是这几条最复杂的。
#
# 需要：build-os-mips/（scripts/build-os.sh --mips）与 .tools/qemu-mipsel-static
set -euo pipefail
cd "$(dirname "$0")/.."

QEMU=.tools/qemu-mipsel-static
BD=${BD:-build-os-mips}
[ -x "$BD/qzos-host" ] || { echo "missing $BD/qzos-host — run scripts/build-os.sh --mips" >&2; exit 1; }
[ -x "$QEMU" ] || { echo "missing $QEMU — run scripts/fetch-tools.sh" >&2; exit 1; }

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

# qemu-user 下 ISOLATED 双进程模型需要 trampoline：主RT 是 fork 出来的子进程，
# qemu-user-static 不会自动带上解释器（见 brain qemu-isolated-trampoline）。
cat > "$OUT/qzjs-rt.sh" <<EOF
#!/bin/sh
exec $PWD/$QEMU -0 "\$0" "$PWD/$BD/qzjs-rt" "\$@"
EOF
chmod +x "$OUT/qzjs-rt.sh"

run_mips() {  # run_mips <name> <app_dir> [keys]
  local name="$1" appdir="$2" keys="${3:-}"
  local fifo="$OUT/$name.fifo" frame="$OUT/$name.pbm" log="$OUT/$name.log"
  rm -f "$fifo" "$frame" "$log"; mkfifo "$fifo"
  ( QZ_DISPLAY=pbm QZ_PBM="$frame" QZ_RPC_SOCK=none QZ_JS_DIR=os/js \
    QZ_APP_DIR="$appdir" QZ_RT_SERVER="$OUT/qzjs-rt.sh" \
    QZ_AUTOEXIT_S=9 QZ_INPUT0="$fifo" QZ_INPUT1= \
    "$QEMU" "$PWD/$BD/qzos-host" >"$log" 2>&1 ) &
  local pid=$!
  if [ -n "$keys" ]; then
    sleep 3
    python3 os/test/replay-keys.py --arch mips32 --script "$keys" --out "$fifo"
  fi
  wait "$pid" 2>/dev/null || true
  [ -s "$frame" ] || { echo "no frame for $name" >&2; tail -8 "$log" >&2; return 1; }
}

echo "==> MIPS: 桌面起来了"
mkdir -p "$OUT/empty"
if run_mips desk "$OUT/empty" ""; then
  ink=$(python3 os/test/pbm_view.py --region "$OUT/desk.pbm" --at 8,41,280,16 \
        | sed -n 's/.*-> \([0-9]*\) ink px/\1/p')
  if [ "${ink:-0}" -gt 100 ]; then
    ok "MIPS 桌面渲染正常（应用行墨量 ${ink}）"
  else
    bad "MIPS 桌面空（墨量 ${ink}）"
  fi
  if grep -q "\[shell\] up, 2 apps" "$OUT/desk.log"; then
    ok "MIPS 上应用发现正常（2 apps）"
  else
    bad "MIPS 上应用发现异常: $(grep -o 'up, .* apps' "$OUT/desk.log" | head -1)"
  fi
else
  bad "MIPS 桌面没起来"
fi

echo "==> MIPS: 键盘 + 应用启动（授权面在 MIPS 上也要能装）"
if run_mips nav "$OUT/empty" "down,enter"; then
  if grep -qE "qzos-display: (\[ *[0-9]+ ms\] )?commit " "$OUT/nav.log"; then
    n=$(grep -cE "qzos-display: (\[ *[0-9]+ ms\] )?commit " "$OUT/nav.log")
    ok "MIPS 上按键→启动应用产生 ${n} 次提交（授权面没把系统自己砍掉）"
  else
    bad "MIPS 上按 enter 后一次提交都没有——键盘链路死了"
  fi
else
  bad "MIPS 键盘场景失败"
fi

echo "==> MIPS: 授权面确实装上了（__native__ 后门被遮）"
# 在 JS 侧自报：应用若能看到 __native__.fsWrite 就说明遮蔽没生效。
#
# 两个易错点（都踩过）：
#  - QZ_APP_DIR 指向**父目录**，包是其下的子目录。指到包自己会让 readdir
#    返回 ["app.json","app.js"]（文件而非目录），scanDir 一个都认不出来。
#  - probe 包按 name 排在 "Hello"/"Notepad" 之后，所以要 down,down 才到得了。
mkdir -p "$OUT/probe"
cat > "$OUT/probe/app.json" <<'EOF'
{ "schema":1,"id":"probe","name":"Probe","version":"1.0.0","api":1,
  "entry":"app.js","perms":["info"] }
EOF
cat > "$OUT/probe/app.js" <<'EOF'
console.log('SANDBOX n=' + typeof globalThis.__native__ +
            ' fw=' + typeof (globalThis.__native__ || {}).fsWrite +
            ' ps=' + typeof (globalThis.__native__ || {}).processSpawn +
            ' perms=' + JSON.stringify(api.perms));
EOF
run_mips probe "$OUT" "down,down,enter" || true
probe_line=$(grep -o 'SANDBOX.*' "$OUT/probe.log" | head -1)
# 两半都要验：遮蔽生效，且**用户目录的应用 perms 为空**（fail-closed 在 MIPS 上
# 同样成立）。只验前半会漏掉「授权在 MIPS 上被静默放开」；只验后半则完全测
# 不到遮蔽——两个是不同的失效面。
if echo "$probe_line" | grep -q "n=object fw=undefined ps=undefined"; then
  ok "MIPS 上遮蔽生效（__native__ 在、fsWrite/processSpawn 不可见）"
else
  bad "MIPS 上遮蔽异常: ${probe_line:-（应用没跑起来）}"
fi
if echo "$probe_line" | grep -q "perms=\[\]"; then
  ok "MIPS 上用户目录应用 fail-closed（perms=[]，statMode 原语未实现）"
else
  bad "MIPS 上用户目录应用拿到了授权（应为空）: ${probe_line:-（无输出）}"
fi

# 内置目录的应用**应该**拿到 perms——反过来验 fail-closed 没有一刀切到
# 内置包上（那条一刀切曾让 hello 的 sys.info 静默失效）。
JS2="$OUT/js2"; mkdir -p "$JS2/apps/bp"
for f in ui.js apkg.js sandbox.js shell.js; do ln -s "$PWD/os/js/$f" "$JS2/$f"; done
cat > "$JS2/apps/bp/app.json" <<'EOF'
{ "schema":1,"id":"bp","name":"Builtin Probe","version":"1.0.0","api":1,
  "entry":"app.js","perms":["info"] }
EOF
cat > "$JS2/apps/bp/app.js" <<'EOF'
console.log('BUILTIN perms=' + JSON.stringify(api.perms));
EOF
run_mips builtin "$OUT/empty" "" 2>/dev/null || true
FIFO="$OUT/builtin.fifo"; rm -f "$FIFO"; mkfifo "$FIFO"
( QZ_DISPLAY=pbm QZ_PBM="$OUT/bp.pbm" QZ_RPC_SOCK=none QZ_JS_DIR="$JS2" \
  QZ_APP_DIR="$OUT/empty" QZ_RT_SERVER="$OUT/qzjs-rt.sh" \
  QZ_AUTOEXIT_S=9 QZ_INPUT0="$FIFO" QZ_INPUT1= \
  "$QEMU" "$PWD/$BD/qzos-host" >"$OUT/bp.log" 2>&1 ) &
BP=$!
sleep 4
python3 os/test/replay-keys.py --arch mips32 --script enter --out "$FIFO"
wait "$BP" 2>/dev/null || true
if grep -q 'BUILTIN perms=\["info"\]' "$OUT/bp.log"; then
  ok "MIPS 上内置应用拿到 perms（fail-closed 没有一刀切）"
else
  bad "MIPS 上内置应用没拿到 perms: $(grep -o 'BUILTIN.*' "$OUT/bp.log" | head -1)"
fi

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
