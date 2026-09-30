#!/usr/bin/env bash
# tools/build-rpc-client.sh — 编译外部 uvrpc IPC 客户端
# 用法: bash tools/build-rpc-client.sh [build-dir]  →  /tmp/qzos-rpc-client
set -e
cd "$(dirname "$0")/.."
B="${1:-build-os}"
UV=third_party/uvrpc

gcc -std=gnu99 -o /tmp/qzos-rpc-client tools/qzos-rpc-client.c \
  -I "$UV/include" -I os/third_party/uvrpc-generated \
  -I qzjs/deps/libuv/include -I qzjs/deps/libuv/src \
  -DUVRPC_DEFAULT_ALLOCATOR=0 \
  "$B/libqzos_uvrpc.a" "$B/libqzos_flatcc.a" "$B/deps/libuv/libuv.a" \
  -lm -lpthread
echo "built /tmp/qzos-rpc-client"
