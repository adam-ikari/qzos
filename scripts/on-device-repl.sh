#!/usr/bin/env bash
# One-shot: launch the on-screen REPL session on the C1 Slim.
#   ADB="adb.exe -s MagicPen-206892" scripts/on-device-repl.sh
# Verifies liveness, then prints keys.
set -uo pipefail
cd "$(dirname "$0")/.."
ADB=${ADB:-adb}
read -r -a ADB_CMD <<<"$ADB"

if ! "${ADB_CMD[@]}" get-state >/dev/null 2>&1; then
  echo "device unreachable via '$ADB'. Reconnect the C1 Slim (power/USB), check:" >&2
  echo "  ${ADB_CMD[0]} ${ADB_CMD[*]:1} devices" >&2
  exit 1
fi

D=/usr/data/c1term
if [ "${1:-}" = "--stop" ]; then
  "${ADB_CMD[@]}" shell "$D/c1term-stop.sh" && echo "terminal stopped, desktop resumed"
  exit 0
fi
if ! "${ADB_CMD[@]}" shell "test -x $D/c1term -a -x $D/c1term-run.sh" 2>/dev/null; then
  echo "C1Terminal not installed — run:  ADB=\"$ADB\" scripts/c1term-install.sh" >&2
  exit 1
fi
if ! "${ADB_CMD[@]}" shell "test -x /storage/c1/qzjs/qzjs" 2>/dev/null; then
  echo "qzjs not deployed — run:  ADB=\"$ADB\" scripts/deploy-device.sh" >&2
  exit 1
fi

# replace a previous session if any
"${ADB_CMD[@]}" shell "$D/c1term-stop.sh" >/dev/null 2>&1
# launch AND settle inside one adb session: adbd reaps children of a session
# that exits immediately, which kills the setsid'd job before it can detach.
OUT=$("${ADB_CMD[@]}" shell "setsid $D/c1term-run.sh </dev/null >/tmp/c1term.log 2>&1 & sleep 4; echo PID=\$(pidof c1term)" 2>&1 | tr -d '\r')
PID=$(printf '%s\n' "$OUT" | sed -n 's/^PID=\([0-9 ]*\)$/\1/p')

if [ -n "$PID" ]; then
  cat <<'EOF'
Screen terminal is up (c1slim# prompt).

  qzjs<Enter>      start the JS REPL
  <Ctrl-D>         exit the REPL
  <HOME>           exit the terminal, desktop returns
  SHIFT            key layer (see on-screen hint)

Stuck?  ADB="..." scripts/on-device-repl.sh --stop
EOF
else
  echo "c1term did not stay up; log:" >&2
  "${ADB_CMD[@]}" shell cat /tmp/c1term.log 2>&1 | tr -d '\r' >&2
  exit 1
fi
