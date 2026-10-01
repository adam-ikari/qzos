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
trap 'hz_stop; rm -rf "$OUT"' EXIT

HZ_HOST="$BD/qzos-host"
. os/test/hostlib.sh
HZ_FIFO_DIR="$OUT"
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

# qemu-user 下比原生慢一个数量级，所以超时一律放宽到 3 倍。宁可慢，不可脆。
run_mips() {  # run_mips <name> <app_dir> [keys] [settle_ms]
  local name="$1" appdir="$2" keys="${3:-}" settle="${4:-2500}"
  local frame="$OUT/$name.pbm" log="$OUT/$name.log"
  rm -f "$frame" "$log"
  hz_set_cmd "$QEMU" "$PWD/$BD/qzos-host"
  hz_start "$log" QZ_DISPLAY=pbm QZ_PBM="$frame" QZ_RPC_SOCK=none QZ_JS_DIR=os/js \
           QZ_APP_DIR="$appdir" QZ_RT_SERVER="$OUT/qzjs-rt.sh" QZ_AUTOEXIT_S=25
  if ! hz_wait_desktop 40; then
    echo "no desktop commit for $name" >&2; tail -8 "$log" >&2; hz_stop; return 1
  fi
  if [ -n "$keys" ]; then
    python3 os/test/replay-keys.py --arch mips32 --script "$keys" --out "$(hz_fifo)" >/dev/null 2>&1
    hz_wait_stable "$log" "$settle" 25 || true
  else
    # 不按键的场景：等 shell 报出应用数，那行是「发现阶段完成」的标志
    hz_wait 'apps' 40 || true
  fi
  hz_stop
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
hz_set_cmd "$QEMU" "$PWD/$BD/qzos-host"
hz_start "$OUT/bp.log" QZ_DISPLAY=pbm QZ_PBM="$OUT/bp.pbm" QZ_RPC_SOCK=none \
         QZ_JS_DIR="$JS2" QZ_APP_DIR="$OUT/empty" \
         QZ_RT_SERVER="$OUT/qzjs-rt.sh" QZ_AUTOEXIT_S=25
if hz_wait_desktop 40; then
  python3 os/test/replay-keys.py --arch mips32 --script enter --out "$(hz_fifo)" >/dev/null 2>&1
  # 哨兵 = 应用自己打的那行。回执没到就停机的话，判据读的是残缺日志——
  # 而「没读到」很容易被读成「没拿到 perms」。
  hz_wait 'BUILTIN perms=' 40 || true
else
  bad "MIPS 授权探针那次运行没到桌面"
fi
hz_stop
if grep -q 'BUILTIN perms=\["info"\]' "$OUT/bp.log"; then
  ok "MIPS 上内置应用拿到 perms（fail-closed 没有一刀切）"
else
  bad "MIPS 上内置应用没拿到 perms: $(grep -o 'BUILTIN.*' "$OUT/bp.log" | head -1)"
fi

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
