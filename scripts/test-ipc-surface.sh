#!/usr/bin/env bash
# scripts/test-ipc-surface.sh — 系统服务面的**外部 IPC 半边**闸门
#
# brain: qzos-service-boundary。这条闸门守的是一个已经真机验证过的洞：
# 授权检查住在 qzos_services_rpc()（INPROC 路径，JS 桥走的那条），而 IPC 监听器
# 曾经把 handler **直接**注册进去，完全绕过授权。实测：宿主里一个应用都没跑，
# 一个外部进程连上 /storage 上的 socket 就调通了 sys.storage.statfs。设备上
# /storage 是 0777，那等于「任何应用都能读你的存储布局」。
#
# 判据读**宿主日志**里的 `Handler not found`，不读客户端回执。原因：uvrpc 的
# client 恒把 status 填成 OK（third_party/uvrpc/src/uvrpc_client.c:158），而
# server 把「Method not found」塞进 result 的头 4 字节（uvrpc_server.c:207），
# 线上**没有标签**能让客户端分辨。曾按「头 4 字节非零即错误」解过，结果
# sys.info 的 `{"se` 被读成错误码 1702044283——猜比不猜更糟。
#
# 「handler 有没有被调用」的权威记录在服务端那一侧，所以判据取那里。
set -euo pipefail
cd "$(dirname "$0")/.."

B=build-os
[ -x "$B/qzos-host" ] || { echo "build first: scripts/build-os.sh" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
SOCK="$TMP/rpc.sock"
LOG="$TMP/host.log"
pass=0; fail=0
ok()   { printf '  PASS  %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  FAIL  %s\n' "$1"; fail=$((fail+1)); }

bash tools/build-rpc-client.sh "$B" >/dev/null 2>&1
CLIENT=/tmp/qzos-rpc-client

( cd "$B" && QZ_DISPLAY=none QZ_RPC_SOCK="$SOCK" \
    QZ_JS_DIR=../os/js QZ_APP_DIR=../os/js/apps QZ_AUTOEXIT_S=12 \
    ./qzos-host >"$LOG" 2>&1 ) &
HOST=$!
trap 'kill $HOST 2>/dev/null || true; rm -rf "$TMP"' EXIT

# 等 socket 出现，而不是 sleep 固定时长
for _ in $(seq 1 60); do [ -S "$SOCK" ] && break; sleep 0.1; done
[ -S "$SOCK" ] || { echo "socket never appeared; host log:" >&2; cat "$LOG" >&2; exit 1; }

# ---- 1. 需能力的方法不上 IPC，且 handler 真的没被调用 ----
"$CLIENT" "$SOCK" sys.storage.statfs >"$TMP/priv.out" 2>&1 || true
if grep -q "withholding 'sys.storage.statfs' from ipc" "$LOG"; then
  ok "需 storage 能力的方法被明确扣在 IPC 之外"
else
  bad "sys.storage.statfs 竟被挂到了 IPC 上（授权可被绕过）"
fi
if grep -q "Handler not found: 'sys.storage.statfs'" "$LOG"; then
  ok "外部进程调需能力的方法：handler 从未被调用（服务端权威记录）"
else
  bad "没看到 Handler not found —— handler 可能真的跑起来了"
  echo "        host log:" >&2; sed 's/^/        /' "$LOG" >&2
fi
# 结果体不应含有挂载信息：即使把错误负载当数据看，也拿不到 statvfs 的输出
if grep -q "mounts" "$TMP/priv.out"; then
  bad "外部进程拿到了 sys.storage.statfs 的数据"
else
  ok "外部进程拿不到 sys.storage.statfs 的数据"
fi

# ---- 2. 正对照：公开方法必须真的能调通 ----
# 没有这一条，上面那些「Handler not found」也可能只是客户端坏了。
"$CLIENT" "$SOCK" sys.info >"$TMP/pub.out" 2>&1 || true
if grep -q '"service":"sys"' "$TMP/pub.out"; then
  ok "正对照：公开方法 sys.info 经 IPC 正常返回（客户端本身是好的）"
else
  bad "正对照失败：sys.info 也调不通，所以上面的拒绝可能只是客户端坏了"
  sed 's/^/        /' "$TMP/pub.out" >&2
fi
if grep -q "Handler not found: 'sys.info'" "$LOG"; then
  bad "sys.info 竟找不到 handler——IPC 公开面是空的"
else
  ok "公开面确实注册了 sys.info"
fi

# ---- 3. socket 权限不能是「谁都能连」 ----
mode=$(stat -c '%a' "$SOCK" 2>/dev/null || echo "?")
case "$mode" in
  600|700) ok "socket 权限 $mode（非 world-connectable）" ;;
  *)      bad "socket 权限 $mode —— 设备上 /storage 是 0777，等于任何应用都能连" ;;
esac

# ---- 4. 扣下的数量必须够：注册表里存在需能力的方法，所以 withheld 不能是 0 ----
# 原来这条写的是 `pub + wh >= 1`，那在变异下照样通过（2 个全公开、0 个扣下，
# 和仍是 1）——「和不为零」证明不了任何事。这里只钉一件可独立判定的事：
# 一定存在需能力的方法（test_services 已从注册表侧证过 sys.storage.statfs
# 需要 storage），所以它必须出现在 withheld 里。
if grep -q "ipc listening on .*([0-9]* public method(s)" "$LOG"; then
  pub=$(grep -o "([0-9]* public method(s)" "$LOG" | grep -o "[0-9]*" | head -1)
  wh=$(grep -o "[0-9]* withheld" "$LOG" | grep -o "[0-9]*" | head -1)
  if [ "${wh:-0}" -ge 1 ]; then
    ok "公开面 $pub 个 / 扣下 $wh 个（注册表里确有需能力的方法，扣下数不可为 0）"
  else
    bad "扣下的方法数为 ${wh:-?} —— 注册表里存在需能力的方法，不该一个都不扣"
  fi
  # 逐条核对：日志里说扣了 N 个，就得能数出 N 行 withholding
  lines=$(grep -c "withholding '.*' from ipc" "$LOG" || true)
  if [ "${lines:-0}" -eq "${wh:-0}" ]; then
    ok "逐条记录与汇总一致（$lines 行 withholding = 汇总 $wh）"
  else
    bad "汇总说扣了 ${wh:-?} 个，日志里却只有 ${lines:-0} 行 withholding"
  fi
else
  bad "启动日志没记录公开面/扣下的划分，无法核对"
fi

wait "$HOST" 2>/dev/null || true
printf '  %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
