#!/usr/bin/env bash
# scripts/test-apkg.sh — apkg.js / sandbox.js 单测（纯 JS 层）
#
# 跑在**真实 qzjs** 上而不是 node：本测试的核心断言是「globalThis.__native__
# 上那批原生真的被遮住了」，而 __native__ 是 qzjs 宿主注入的——在 node 上跑
# 等于什么都没测。这与 test-display.sh / test-keymap.sh 是同一条纪律（纯逻辑
# 层毫秒级、不依赖设备），只是这里的「纯逻辑」是 JS。
#
# 依赖原生构建的 qzjs（build-os/qzjs/qzjs）；没构建过就先跑 scripts/build-os.sh。
set -euo pipefail
cd "$(dirname "$0")/.."

QZJS=${QZJS:-build-os/qzjs/qzjs}
if [ ! -x "$QZJS" ]; then
  echo "找不到 $QZJS —— 先跑 scripts/build-os.sh（原生构建）" >&2
  exit 1
fi

# 真实文件系统 fixture：读写断言必须落在真文件/真目录上，否则测的是
# 「目录不存在」而不是「越权被拒」。app/ 与 sibling/ 平级，sibling 里放一个
# 诱饵文件用来验证「包外写读都被拒，且拒绝真的没落地」。
ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT
mkdir -p "$ROOT/app/lib" "$ROOT/sibling"
echo 'TOP-SECRET' > "$ROOT/sibling/secret.txt"

# 引导：apkg.js / sandbox.js / 测试脚本都塞进同一个 runtime，且走和 shell 一样的
# 加载路径（qzjs.fs.readFile + eval），不给被测代码任何捷径。
BOOT="$ROOT/boot.js"
cat > "$BOOT" <<EOF
(async function () {
  'use strict';
  var dir = '$PWD/os/js';
  globalThis.__QZOS_TEST_ROOT = '$ROOT';
  globalThis.arguments = ['$ROOT'];
  var load = async function (rel) { (0, eval)(await qzjs.fs.readFile(dir + '/' + rel)); };
  await load('apkg.js');
  await load('sandbox.js');
  (0, eval)(await qzjs.fs.readFile('$PWD/os/test/test_apkg.js'));
})();
EOF

# qzjs 的 eval 通道是 classic script（不支持顶层 await），所以引导里用 async IIFE。
"$QZJS" -e "$(cat "$BOOT")"
