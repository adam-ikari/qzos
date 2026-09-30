#!/usr/bin/env bash
# Install C1Terminal (built from Kasiin/C1-Slim-Ports) to /usr/data/c1term on
# the device, wire qzjs into its PATH, and provide a standalone launcher that
# suspends the stock mpenMain UI while the terminal runs (no rootfs patching).
#
#   ADB="adb.exe -s MagicPen-206892" scripts/c1term-install.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ADB=${ADB:-adb}
BD=${BUILD_DIR:-build-mips}
read -r -a ADB_CMD <<<"$ADB"
"${ADB_CMD[@]}" get-state >/dev/null

[ -f "$BD/c1term" ] || { echo "missing $BD/c1term — build C1Terminal first (see this script's header note in README)" >&2; exit 1; }
[ -f "$BD/qzjs" ] && [ -f "$BD/qzjs-rt" ] || { echo "missing qzjs artifacts — run scripts/build-mips.sh" >&2; exit 1; }

D=/usr/data/c1term
echo "==> pushing c1term + qzjs"
"${ADB_CMD[@]}" shell "mkdir -p $D/bin /storage/c1/qzjs"
"${ADB_CMD[@]}" push "$BD/c1term" "$D/c1term" >/dev/null
"${ADB_CMD[@]}" push "$BD/qzjs" "$BD/qzjs-rt" /storage/c1/qzjs/ >/dev/null
"${ADB_CMD[@]}" shell "chmod +x $D/c1term /storage/c1/qzjs/qzjs /storage/c1/qzjs/qzjs-rt; ln -sf /storage/c1/qzjs/qzjs $D/bin/qzjs; ln -sf /storage/c1/qzjs/qzjs-rt $D/bin/qzjs-rt"

# Standalone launcher: this test device runs the C1ancher desktop (not the
# stock mpenMain), so suspend every process whose exe lives under
# /usr/data/c1/core while the terminal owns screen + keyboard.
cat > /tmp/c1term-run.sh <<'EOF'
#!/bin/sh
D=/usr/data/c1term
UI=""
for p in $(ls /proc | grep -E '^[0-9]+$'); do
  e=$(readlink /proc/$p/exe 2>/dev/null)
  case "$e" in /usr/data/c1/core/*) UI="$UI $p";; esac
done
[ -n "$UI" ] && kill -STOP $UI
trap '[ -n "$UI" ] && kill -CONT $UI' EXIT INT TERM
$D/c1term
EOF
cat > /tmp/c1term-stop.sh <<'EOF'
#!/bin/sh
kill $(pidof c1term) 2>/dev/null
for p in $(ls /proc | grep -E '^[0-9]+$'); do
  e=$(readlink /proc/$p/exe 2>/dev/null)
  case "$e" in /usr/data/c1/core/*) kill -CONT $p 2>/dev/null;; esac
done
EOF
chmod +x /tmp/c1term-run.sh /tmp/c1term-stop.sh
"${ADB_CMD[@]}" push /tmp/c1term-run.sh /tmp/c1term-stop.sh $D/ >/dev/null
"${ADB_CMD[@]}" shell "chmod +x $D/c1term-run.sh $D/c1term-stop.sh"

# Wi-Fi CLI, on the terminal's PATH
"${ADB_CMD[@]}" push "$(dirname "$0")/c1wifi" "$D/bin/c1wifi" >/dev/null
"${ADB_CMD[@]}" shell "chmod 755 $D/bin/c1wifi"

cat <<EOF

Installed. Usage:
  Interactive REPL on the e-ink screen — one shot, from the project root:
    ADB="${ADB[0]} ${ADB_CMD[*]:1}" scripts/on-device-repl.sh
  or from the desktop: pick the 'terminal' app (scripts/c1term-launcher-app.sh --install).
  Inside the terminal:
    qzjs        JS REPL (PATH includes $D/bin)
    c1wifi      Wi-Fi: scan / join <ssid> <psk> / status / off
    HOME        exits and restores the stock desktop
  If anything gets stuck:
    "${ADB_CMD[0]}" ${ADB_CMD[*]:1} shell /usr/data/c1term/c1term-stop.sh
EOF
