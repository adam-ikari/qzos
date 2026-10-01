#!/usr/bin/env bash
# scripts/test-appauth.sh — 应用授权的来源闸门（brain: qzos-service-boundary）
#
# 守的是一个实测过的提权漏洞。曾经的形状是：op:app 带着 JS 侧填的 perms 数组
# 进来，host 直接收下当授权用。而应用与 shell 共享同一个 QuickJS 全局、`ui` 是
# 全局对象，于是任何应用都能自己调 ui.setApp('self', ['storage']) 给自己授权：
#
#     DECLARED=[]                      ← manifest 只声明零能力
#     SETAPP ACCEPTED
#     qzos-services: app 'escaper' authorized (1 caps)
#     STORAGE *** ALLOWED ***
#
# 不需要外部进程、不需要 socket。修法：**能力只从磁盘 manifest 推导**（appauth.c），
# 消息里的 perms 一律不看。JS 可以点名一个应用，不能决定它能做什么。
#
# 每条否定项都配正对照。只测否定项的话，一个「把所有调用都拒掉」的实现也能全绿。
#
# 判据走 console.log 而不是画面：这里要判的是「哪个方法被放行」，pbm_view 不做
# OCR。画面层面由 test_shell_apps.sh 覆盖。
set -euo pipefail
cd "$(dirname "$0")/.."

HOST=${HOST:-build-os/qzos-host}
[ -x "$HOST" ] || { echo "missing $HOST — run scripts/build-os.sh" >&2; exit 1; }

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
JS2="$OUT/js2"
pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

mkdir -p "$JS2/apps/escaper" "$JS2/apps/liar" "$JS2/apps/imposter" "$OUT/empty"
for f in ui.js apkg.js sandbox.js shell.js; do ln -s "$PWD/os/js/$f" "$JS2/$f"; done

# escaper：perms []，零能力。桌面上第 1 行。
printf '%s\n' '{"schema":1,"id":"escaper","name":"Escaper","version":"1.0.0","api":1,"entry":"app.js","perms":[]}' \
  > "$JS2/apps/escaper/app.json"
# liar：磁盘上真有 storage，桌面上第 2 行。它会**反过来**声明零能力 ——
# 若仍被放行，说明 host 认磁盘不认消息。缺了它，那些 denied 只是「全都拒了」。
printf '%s\n' '{"schema":1,"id":"liar","name":"Underclaim","version":"1.0.0","api":1,"entry":"app.js","perms":["storage"]}' \
  > "$JS2/apps/liar/app.json"
# imposter：目录名 imposter，manifest 里写 id=victim，声明 storage+power。
# 它**不会**出现在桌面上（apkg.js 因 id 与目录名不符就拒了），所以测不到
# 「它自己起来会怎样」——能测的是零能力的 escaper 去**点名**它。
printf '%s\n' '{"schema":1,"id":"victim","name":"Imposter","version":"1.0.0","api":1,"entry":"app.js","perms":["storage","power"]}' \
  > "$JS2/apps/imposter/app.json"
echo '/* never launched */' > "$JS2/apps/imposter/app.js"

cat > "$JS2/apps/escaper/app.js" <<'EOF'
/* 零能力应用，逐个试探抬手姿势。串行：每次 rpc 的回执到达后再做下一次，
 * 否则后发的 setApp 会在前一个结果解析前改掉上下文，判据就不确定了。 */
console.log('DECLARED=' + JSON.stringify(api.perms));

function probe(tag, then) {
  return ui.rpc('sys.storage.statfs', {}).then(
    function (r) { console.log(tag + ' ALLOWED' + (r && r.mounts ? ' mounts=' + r.mounts.length : '')); },
    function () { console.log(tag + ' DENIED'); if (then) then(); });
}

ui.setApp('escaper', ['storage']);           /* 给自己加能力 */
probe('P1 ESCALATE-SELF', function () {
  ui.setApp('imposter', ['storage']);        /* 点名一个 id 与目录名不符的目录 */
  probe('P2 IMPOSTER-NAMED');
});
EOF

cat > "$JS2/apps/liar/app.js" <<'EOF'
console.log('LIAR-DECLARED=' + JSON.stringify(api.perms));
ui.setApp('liar', []);                        /* 磁盘有 storage，消息说零个 */
ui.rpc('sys.storage.statfs', {}).then(
  function (r) { console.log('P3 DISK-AUTHORITY ALLOWED mounts=' +
                              (r && r.mounts ? r.mounts.length : '?')); },
  function () { console.log('P3 DISK-AUTHORITY DENIED'); });
EOF

# run_app <行号>：桌面按目录名排序，焦点从第 1 行起（escaper=1 · liar=2；
# imposter 不可列，所以不占行）。
#
# 曾经写成「一律按 1 次 enter」——测 liar 时其实 launch 的是 escaper，正对照假失败。
# 行号是显式参数，不靠想当然。另外 replay-keys 的脚本分隔符是**逗号**
# （--help: "down,down,enter"），写成空格会变成一个没人认识的名字、一个键都不发，
# 而这跟「应用没启动」在日志里长得一样。
run_app() {
  local row="$1" log="$2" tag="$3"
  local F="$OUT/fifo.$tag"; mkfifo "$F"
  ( QZ_DISPLAY=pbm QZ_PBM="$OUT/$tag.pbm" QZ_RPC_SOCK=none \
    QZ_JS_DIR="$JS2" QZ_APP_DIR="$OUT/empty" QZ_AUTOEXIT_S=9 \
    QZ_INPUT0="$F" QZ_INPUT1= "$HOST" >"$log" 2>&1 ) &
  local pid=$!
  sleep 2.5
  local script=""
  if [ "$row" -gt 1 ]; then
    for _ in $(seq 2 "$row"); do script="${script}down,"; done
  fi
  script="${script}enter"
  python3 os/test/replay-keys.py --arch x86_64 --script "$script" --out "$F" >/dev/null 2>&1
  wait "$pid" 2>/dev/null || true
}

run_app 1 "$OUT/escaper.log" escaper
run_app 2 "$OUT/liar.log"    liar

# ---- 桌面本身：id 不符的包不该被列出来（JS 侧；C 侧另有 P2）----
if grep -q "app 'imposter' authorized" "$OUT/liar.log"; then
  bad "imposter 竟被授权了"
fi
if grep -qiE "imposter" "$OUT/liar.log" && grep -qiE "launch.*imposter" "$OUT/liar.log"; then
  bad "imposter（id 与目录名不符）被 launch 了"
else
  ok "id 与目录名不符的包不进桌面（apkg.js 侧）"
fi

# ---- 1. 给自己加能力：必须被拒 ----
#
# 判据是「**看到了 DENIED 回执**」，不是「没看到 ALLOWED」。后者会把一次静默
# 挂死读成通过——本仓已经三次栽在这类回执判据上（grep 匹配不到带时间戳的行、
# test_power 漏 failed++、verify-frames 的 SIGPIPE）。否定项必须配正证据。
if grep -q "P1 ESCALATE-SELF DENIED" "$OUT/escaper.log"; then
  ok "零能力应用靠 ui.setApp 给自己加能力：被拒（有 DENIED 回执）"
else
  bad "没有 P1 的 DENIED 回执 —— 分不清「被正确拒绝」和「rpc 静默挂死」"
fi
if grep -q "P1 ESCALATE-SELF ALLOWED" "$OUT/escaper.log"; then
  bad "零能力应用靠 ui.setApp 给自己加了 storage（提权成立）"
else
  ok "零能力应用确实没拿到 storage"
fi

# ---- 2. 点名一个 id 不符的目录：必须被拒 ----
# 链式：P2 只在 P1 的拒绝回调里才发，所以 P2 出现即证明第一段的回执到了。
if grep -q "P2 IMPOSTER-NAMED DENIED" "$OUT/escaper.log"; then
  ok "点名 id 与目录名不符的目录：被拒（有 DENIED 回执）"
else
  bad "没有 P2 的 DENIED 回执 —— 链式探测没走完，或第二段静默挂死"
fi
if grep -q "P2 IMPOSTER-NAMED ALLOWED" "$OUT/escaper.log"; then
  bad "点名 id 与目录名不符的目录竟拿到了授权（把 victim 的 manifest 拷进自己目录就能继承权限）"
else
  ok "冒名目录确实没拿到授权"
fi
if grep -q "id mismatch" "$OUT/escaper.log"; then
  ok "C 侧记下了 id mismatch（不是静默忽略）"
else
  bad "没有 id mismatch 记录 —— 分不清「被正确拒绝」和「目录没找到」"
fi

# ---- 3. 提权尝试要留痕 ----
# 静默忽略会让「为什么我的能力没生效」变成查不到的事。
if grep -q "claim ignored" "$OUT/escaper.log"; then
  ok "提权尝试被记账（claimed vs derived 出现在日志里）"
else
  bad "提权尝试没有留痕"
fi

# ---- 4. 正对照：磁盘 manifest 是权威 ----
if grep -q "P3 DISK-AUTHORITY ALLOWED" "$OUT/liar.log"; then
  ok "正对照：磁盘上声明 storage 的应用被放行（哪怕它自己声明零能力）"
else
  bad "正对照失败：磁盘上真有 storage 也没放行 —— host 根本不读 manifest，"
  bad "那么上面那些 denied 只是「全都拒了」，不是「按磁盘授权」"
fi
if grep -q "claimed 0 cap(s), host derived 1" "$OUT/liar.log"; then
  ok "声称与推导不一致时两侧数字都被记录（claimed 0 / derived 1）"
else
  bad "没有记录 claimed vs derived"
fi

printf '  %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
