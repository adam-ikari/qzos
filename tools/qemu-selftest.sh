#!/usr/bin/env bash
# tools/qemu-selftest.sh — qemu-mipsel 下跑真机产物（交叉冒烟）
#
# 两个 qemu-user 特有的坑（本脚本都处理了）：
#   1. qzjs-rt 是 fork+execve 起的 MIPS 二进制，qemu-user 不会自动给子进程
#      解释器 → 用包装脚本把 QZ_RT_SERVER 指向它。
#   2. 设备节点（/dev/epaper_lcd、/dev/input/*）不存在 → 走 QZ_DISPLAY=pbm
#      导出帧，输入自动跳过。
#
# 已知：rt 在 qemu 下 boot 完会 idle 自退（--qzjs-rt-server 形态语义），
# 原生构建同样时长不复现，属 qemu 环境差异；桌面 UI 与 sys.info 往返均已验证。
#
# 用法: bash tools/qemu-selftest.sh [mips-build-dir]
set -e
cd "$(dirname "$0")/.."
B="${1:-build-os-mips}"
QEMU="$PWD/.tools/qemu-mipsel-static"
RTW=/tmp/qzos-rt-qemu-wrap.sh

[ -x "$QEMU" ] || { echo "missing qemu: $QEMU" >&2; exit 1; }
file "$B/qzos-host" | grep -q MIPS || { echo "not a mips build: $B" >&2; exit 1; }

# rt 包装：spawn 的 argv 原样透传，只把 exec 目标换成 qemu
cat > "$RTW" <<EOF
#!/usr/bin/env bash
exec "$QEMU" "$PWD/$B/qzjs-rt" "\$@"
EOF
chmod +x "$RTW"

export QZ_DISPLAY=pbm QZ_PBM=/tmp/qzos-qemu.pbm QZ_DUMP_RAW=/tmp/qzos-qemu.pgm
export QZ_RPC_SOCK=/tmp/qzos-qemu.sock QZ_RT_SERVER="$RTW"
export QZ_JS_DIR=js QZ_APP_DIR=js/apps
rm -f "$QZ_PBM" "$QZ_DUMP_RAW" "$QZ_RPC_SOCK"

cd "$B"
# 注意：不要用外部 `timeout` 收尾——它发 SIGTERM 给整个进程组，会连带
# 打断宿主（看起来像 rt 崩溃）。要限制时长就在宿主侧加 QZ_AUTOEXIT_S。
echo "--- qemu-mipsel ./qzos-host ---"
QZ_AUTOEXIT_S=6 "$QEMU" ./qzos-host 2>&1 | head -20 || true
echo "--- artifacts ---"
ls -la /tmp/qzos-qemu.pbm /tmp/qzos-qemu.pgm /tmp/qzos-qemu.sock 2>&1 || true
echo "--- frame preview ---"
bash ../scripts/frame-preview.sh /tmp/qzos-qemu.pbm 2>&1 | head -30 || true
