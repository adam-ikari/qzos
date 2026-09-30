#!/usr/bin/env bash
# tools/rpc-ipc-selftest.sh — 验证 uvrpc 服务拓扑的 IPC 半边
#
# 注意：这是**手工探测**，不是闸门。真正的闸门是 scripts/test-ipc-surface.sh，
# 它额外钉住「外部进程调需能力的方法时 handler 从未被调用」并带正对照。
#
# 宿主是 uvrpc hub：inproc://qzos（JS 桥）+ ipc://<sock>（外部服务进程）。
# 本脚本起宿主，再用独立进程客户端连 unix socket 调 sys.info。
#
# 用法: bash tools/rpc-ipc-selftest.sh [sock-path]
set -e
cd "$(dirname "$0")/.."
SOCK="${1:-/tmp/qzos-rpc-selftest.sock}"
B=build-os
[ -x "$B/qzos-host" ] || { echo "build first: scripts/build-os.sh" >&2; exit 1; }

bash tools/build-rpc-client.sh "$B"
rm -f "$SOCK"

cd "$B"
QZ_DISPLAY=none QZ_RPC_SOCK="$SOCK" QZ_JS_DIR=js QZ_APP_DIR=js/apps \
  ./qzos-host > /tmp/qzos-host-ipc.log 2>&1 &
HOST=$!
trap 'kill $HOST 2>/dev/null || true; rm -f "$SOCK"' EXIT
sleep 1.5

echo "--- host ---"
cat /tmp/qzos-host-ipc.log
echo "--- external IPC client -> $SOCK ---"
/tmp/qzos-rpc-client "$SOCK" sys.info
