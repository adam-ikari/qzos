#!/usr/bin/env bash
# Deploy the MIPS build to a C1 Slim / MP-D261 over ADB and smoke-test it.
#
#   ADB=adb scripts/deploy-device.sh              # default adb on PATH
#   ADB=/mnt/c/.../platform-tools/adb.exe scripts/deploy-device.sh   # via Windows adb
set -euo pipefail
cd "$(dirname "$0")/.."
ADB=${ADB:-adb}
BD=${BUILD_DIR:-build-mips}
DEST=/storage/c1/qzjs

# ADB may carry args, e.g. ADB="adb.exe -s MagicPen-206892"
read -r -a ADB_CMD <<<"$ADB"
"${ADB_CMD[@]}" get-state >/dev/null 2>&1 || {
  echo "adb cannot reach a device ('$ADB'). Check USB/root-ADB, pass ADB=\"adb -s <serial>\"." >&2
  exit 1
}
for f in "$BD/qzjs" "$BD/qzjs-rt"; do
  [ -f "$f" ] || { echo "missing $f — run scripts/build-mips.sh first" >&2; exit 1; }
done

echo "==> pushing to $DEST"
"${ADB_CMD[@]}" shell "mkdir -p $DEST"
"${ADB_CMD[@]}" push "$BD/qzjs" "$BD/qzjs-rt" "$DEST/"
"${ADB_CMD[@]}" shell "chmod +x $DEST/qzjs $DEST/qzjs-rt"

echo "==> on-device smoke test"
"${ADB_CMD[@]}" shell "$DEST/qzjs --version"
"${ADB_CMD[@]}" shell "$DEST/qzjs -e 'console.log(6*7)'"
# REPL over a pipe exercises the same eval channel the interactive session uses
"${ADB_CMD[@]}" shell "printf '1+1\nvar x=40\nx+2\n' | $DEST/qzjs"

cat <<'EOF'

Deployed. To use the REPL on the device:
  1. Press T to open C1Terminal (stock desktop slot 1).
  2. At the bash prompt run:  /storage/c1/qzjs/qzjs
  3. Type JS, press Enter; Ctrl-D exits.  (49x18 screen — keep lines short.)
EOF
