#!/usr/bin/env bash
# scripts/verify-frames.sh — 原生 / MIPS 双平台帧一致性回归
#
# 为什么需要这一条：显示逻辑（raster_1bpp 阈值 / display_policy 刷新决策 /
# panel 条带寻址）全是整数与位运算，但真机上 /dev/epaper_lcd 是只写设备，
# 抓不到画面——"编译过"和"屏上画对"之间隔着一条无人值守的鸿沟。能自动化的
# 唯一替代是：把同一段 JS 喂给两个平台的宿主，比较它们吐出的 1bpp 帧是否
# 逐字节相同。相同即等价性回归（MIPS 上没有悄悄改掉的整数/对齐/端序问题），
# 不同即定位到平台。视觉本身仍然只能人眼在屏上看。
#
# 用法:
#   scripts/verify-frames.sh                 # 自动构建缺失的产物
#   KEEP=1 scripts/verify-frames.sh          # 保留导出的帧到 /tmp/qzos-frames/
set -euo pipefail
cd "$(dirname "$0")/.."

Q=".tools/qemu-mipsel-static"
NATIVE_DIR="${BUILD_DIR:-build-os}"
MIPS_DIR="${BUILD_DIR_MIPS:-build-os-mips}"
AUTOEXIT="${AUTOEXIT_S:-4}"
OUT=/tmp/qzos-frames
mkdir -p "$OUT"

die() { echo "FAIL: $*" >&2; exit 1; }

# 1) 产物齐不齐 —— 缺就构建（qemu 侧产物是 MIPS 交叉编译的硬前提）
[ -x .tools/zig/zig ] || bash scripts/fetch-tools.sh
[ -x "$Q" ] || bash scripts/fetch-tools.sh

if [ ! -x "$NATIVE_DIR/qzos-host" ]; then
  echo "==> native build missing, building"
  bash scripts/build-os.sh
fi
if [ ! -x "$MIPS_DIR/qzos-host" ]; then
  echo "==> mips build missing, building (交叉编译，首次较慢)"
  bash scripts/build-os.sh --mips
fi

# 2) 纯逻辑层先过：条带寻址 / 刷新决策 / panel 参数
echo "==> display unit tests (133 assertions)"
bash scripts/test-display.sh

# 3) qemu trampoline：宿主内核无法直接 exec MIPS 二进制，QZ_RT_SERVER 指向
#    一个宿主 shell，由它重新经 qemu 拉起 guest rt 并用 -0 保留 guest argv[0]
#    （见 brain: qemu-isolated-trampoline）
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cat > "$WORK/rt-qemu" <<EOF
#!/bin/sh
exec "$PWD/$Q" -0 "\$0" "$PWD/$MIPS_DIR/qzjs-rt" "\$@"
EOF
chmod +x "$WORK/rt-qemu"

run_frame() { # <out.pbm> <logfile> <label> <command...>
  local out=$1 log=$2 label=$3; shift 3
  echo "==> $label"
  if ! timeout 180 "$@" >"$log" 2>&1; then
    echo "--- $label log ---"; cat "$log"; die "$label exited non-zero"
  fi
  [ -s "$out" ] || { echo "--- $label log ---"; cat "$log"
                     die "$label produced no frame at $out"; }
}

COMMON=(
  QZ_DISPLAY=pbm
  QZ_RPC_SOCK=none
  QZ_AUTOEXIT_S="$AUTOEXIT"
  QZ_JS_DIR=os/js
  QZ_APP_DIR=os/js/apps
)

# native：/proc/self/exe 同目录即可找到 qzjs-rt，无需 trampoline
env "${COMMON[@]}" QZ_PBM="$OUT/native.pbm" \
    "$PWD/$NATIVE_DIR/qzos-host" >"$OUT/native.log" 2>&1 || {
  cat "$OUT/native.log"; die "native host exited non-zero"; }
[ -s "$OUT/native.pbm" ] || { cat "$OUT/native.log"
                              die "native produced no frame"; }

# mips：经 qemu-user 跑宿主，QZ_RT_SERVER 指向 trampoline
run_frame "$OUT/mips.pbm" "$OUT/mips.log" "mips host (qemu-user)" \
  env "${COMMON[@]}" QZ_PBM="$OUT/mips.pbm" QZ_RT_SERVER="$WORK/rt-qemu" \
    "$Q" "$PWD/$MIPS_DIR/qzos-host"

# 4) 逐字节比较
echo
echo "==> frames"
for f in native mips; do
  printf '  %-7s %s bytes  magic=%s\n' "$f" \
    "$(wc -c <"$OUT/$f.pbm")" \
    "$(head -c 2 "$OUT/$f.pbm")"
done
if cmp -s "$OUT/native.pbm" "$OUT/mips.pbm"; then
  echo
  echo "PASS: native == mips ($(wc -c <"$OUT/native.pbm") bytes identical)"
  # 用 sed -n '1,3p' 而不是 | head -3。
  #
  # head 读够 3 行就退出并关掉管道，frame-preview.sh 往已关闭的管道写 → SIGPIPE
  # → 返回 141 → 本脚本 set -euo pipefail 直接以 141 退出。
  # 后果很阴：**每一条断言都打印了 PASS，但 verify-all.sh 的「ALL PASS」从来没
  # 出现过**，而且 verify-all.sh 后面挂的步骤（现在是 MIPS 端到端）根本不会跑。
  # 我连着几轮只看 grep 出来的断言行，全绿就当通过了——「部件都绿但整体失败」
  # 只有看退出码才看得见。
  # sed -n '1,3p' 会把输入读完，不产生 SIGPIPE。
  bash scripts/frame-preview.sh "$OUT/native.pbm" | sed -n '1,3p'
  [ "${KEEP:-0}" = 1 ] || true
else
  echo
  echo "FAIL: frames differ — platform-dependent display logic"
  cmp "$OUT/native.pbm" "$OUT/mips.pbm" | head -3
  echo "native=$OUT/native.pbm mips=$OUT/mips.pbm"
  exit 1
fi
