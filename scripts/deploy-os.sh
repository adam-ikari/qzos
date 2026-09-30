#!/usr/bin/env bash
# Deploy the MIPS qzos-host build to a C1 Slim / MP-D261 over ADB.
#
#   ADB=adb scripts/deploy-os.sh                      # default adb on PATH
#   ADB="/mnt/c/.../adb.exe -s MagicPen-206892" \
#     scripts/deploy-os.sh                            # Windows adb + device
#
# Pushes qzos-host + qzjs-rt + the JS bundle to /storage/qzos, then runs a
# short smoke test. The smoke test uses QZ_DISPLAY=none so it does not fight
# the running desktop for the panel — take over the screen deliberately with
# QZ_DISPLAY=epaper (see the note printed at the end).
set -euo pipefail
cd "$(dirname "$0")/.."

ADB=${ADB:-adb}
BD=${BUILD_DIR:-build-os-mips}
DEST=${DEST:-/storage/qzos}

read -r -a ADB_CMD <<<"$ADB"
"${ADB_CMD[@]}" get-state >/dev/null 2>&1 || {
  echo "adb cannot reach a device ('$ADB'). Check USB/root-ADB, pass ADB=\"adb -s <serial>\"." >&2
  exit 1
}

for f in "$BD/qzos-host" "$BD/qzjs-rt"; do
  [ -f "$f" ] || { echo "missing $f — run scripts/build-os.sh --mips first" >&2; exit 1; }
done
[ -d "$BD/js" ] || { echo "missing $BD/js — JS bundle not staged" >&2; exit 1; }

echo "==> pushing to $DEST"
"${ADB_CMD[@]}" shell "mkdir -p $DEST/bin $DEST/js"
"${ADB_CMD[@]}" push "$BD/qzos-host" "$BD/qzjs-rt" "$DEST/bin/"
"${ADB_CMD[@]}" shell "chmod +x $DEST/bin/qzos-host $DEST/bin/qzjs-rt"
"${ADB_CMD[@]}" push "$BD/js/." "$DEST/js/"

echo "==> on-device smoke test (QZ_DISPLAY=none, does not touch the panel)"
"${ADB_CMD[@]}" shell "cd $DEST && QZ_DISPLAY=none QZ_RPC_SOCK=none \
  QZ_JS_DIR=$DEST/js QZ_APP_DIR=$DEST/js/apps QZ_AUTOEXIT_S=5 \
  ./bin/qzos-host 2>&1 | tail -20"

cat <<EOF

Deployed to $DEST.

  Smoke test above used QZ_DISPLAY=none, so nothing was drawn. To actually
  drive the e-ink panel:

    "${ADB_CMD[0]}" ${ADB_CMD[*]:1} shell \\
      'cd $DEST && QZ_DISPLAY=epaper QZ_JS_DIR=$DEST/js \\
       QZ_APP_DIR=/storage ./bin/qzos-host'

  Do not run it while C1Terminal holds the panel — two writers on
  /dev/epaper_lcd will fight (see brain: c1-slim-device). Stop the terminal
  first, or take over from the desktop launcher.

  Serial console is not the only screen: /dev/epaper_lcd is write-only, so
  there is no way to capture what is on the panel remotely. Check it by eye,
  or use QZ_DISPLAY=pbm and scripts/frame-preview.sh on the host.
EOF
