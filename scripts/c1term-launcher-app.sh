#!/usr/bin/env bash
# Register / remove the C1ancher launcher app for the on-screen terminal.
#   ADB="adb.exe -s MagicPen-206892" scripts/c1term-launcher-app.sh --install
#   ... --remove | --status | --launch
#
# C1ancher's app page is `c1pkg gui`, which scans /storage/c1/apps/*/current and
# runs `c1pkg launch <id>`. Two device-side rules, found by experiment:
#   * every directory from the app id down must not be writable by group/other
#     (0555 dirs, 0444 metadata). A world-writable dir makes c1pkg refuse with
#     "installed entry metadata is unavailable" — a trust guard, not a ledger;
#   * .c1pkg-mode "direct" makes c1pkg take the external-app locks
#     (/dev/shm/c1ancher-external-app.{lock,runlock}) and set .mode=direct, so
#     C1ancher stops drawing on /dev/epaper_lcd while we hold them. The desktop
#     yields on its own — no SIGSTOP bridge is needed on this path.
# Only the label is at the mercy of the trust model: apps absent from the
# vendor-signed index are labelled with their id.
set -uo pipefail
cd "$(dirname "$0")/.."
ADB=${ADB:-adb}
read -r -a ADB_CMD <<<"$ADB"

ID=${C1TERM_APP_ID:-terminal}
VER=${C1TERM_APP_VER:-1.0.0}
APPS=/storage/c1/apps
D=/usr/data/c1term
V=$APPS/$ID/versions/$VER

adb_sh() { "${ADB_CMD[@]}" shell "$@" 2>&1 | tr -d '\r'; }

if ! "${ADB_CMD[@]}" get-state >/dev/null 2>&1; then
  echo "device unreachable via '$ADB'" >&2
  exit 1
fi

case "${1:---status}" in
  --status)
    echo "app dir:   $(adb_sh "test -d $APPS/$ID && echo installed || echo absent")"
    echo "c1term:    $(adb_sh "test -x $D/c1term && echo present || echo MISSING")"
    echo "in index:  $(adb_sh "grep -c -w $ID /usr/data/c1/pkg/cache/verified.v1 2>/dev/null") row(s) (a row here would give a vendor-signed title)"
    echo "pkg list:  $(adb_sh "/usr/data/c1/bin/c1pkg list" | grep -F "$ID" || echo "-")"
    exit 0
    ;;

  --launch)
    # same-session launch rule as scripts/on-device-repl.sh (adbd reaps the
    # children of a session that exits immediately)
    adb_sh "setsid /usr/data/c1/bin/c1pkg launch $ID </dev/null >/tmp/c1term.log 2>&1 & sleep 6; echo c1term=\$(pidof c1term); cat /tmp/c1term.log"
    exit 0
    ;;

  --remove)
    adb_sh "chmod -R u+w $APPS/$ID 2>/dev/null; rm -rf $APPS/$ID"
    echo "removed $APPS/$ID from the launcher"
    exit 0
    ;;

  --install)
    if [ -n "$(adb_sh "test -x $D/c1term -a -x $D/bin/qzjs || echo MISSING")" ]; then
      echo "C1Terminal/qzjs not installed — run:  ADB=\"$ADB\" scripts/c1term-install.sh" >&2
      exit 1
    fi
    ;;

  *)
    echo "usage: ADB=\"...\" scripts/c1term-launcher-app.sh --install|--remove|--status|--launch" >&2
    exit 2
    ;;
esac

# The installed package dir is read-only by design, so it holds only a thin
# wrapper; the real payload stays under /usr/data/c1term and can be upgraded
# without touching the package again.
WRAP=$(mktemp)
cat > "$WRAP" <<EOF
#!/bin/sh
# scripts/on-device-repl.sh can also start the terminal over adb; two instances
# would fight over /dev/input.
if pidof c1term >/dev/null 2>&1; then
    echo 'c1term is already running (HOME exits it)' >&2
    sleep 2
else
    $D/c1term
fi
# C1ancher branches on the mode string ("terminal"/"direct"), so clear it back
# to the default while we still hold the lock, before the desktop resumes.
: > /dev/shm/c1ancher-external-app.mode
exit 0
EOF

adb_sh "chmod -R u+w $APPS/$ID 2>/dev/null; rm -rf $APPS/$ID; mkdir -p $V" >/dev/null
"${ADB_CMD[@]}" push "$WRAP" "$V/terminal" >/dev/null 2>&1 || { echo "push failed" >&2; rm -f "$WRAP"; exit 1; }
rm -f "$WRAP"

OUT=$(adb_sh "
printf 'terminal' > $V/.c1pkg-entry
printf 'direct'   > $V/.c1pkg-mode
ln -sfn versions/$VER $APPS/$ID/current
chmod 0444 $V/.c1pkg-entry $V/.c1pkg-mode
chmod 0555 $V/terminal
chmod 0555 $APPS/$ID $APPS/$ID/versions $V
ls -la $V $APPS/$ID
")
printf '%s\n' "$OUT"

if [ -n "$(adb_sh "test -f $V/.c1pkg-entry && test -x $V/terminal || echo BROKEN")" ]; then
  echo "install failed" >&2
  exit 1
fi

echo
echo "c1pkg list:"
adb_sh "/usr/data/c1/bin/c1pkg list"
echo
echo "On the device: from the desktop open the app page (c1pkg gui / 我的应用) and"
echo "pick '$ID' — the terminal takes the screen, HOME hands it back to the desktop."
echo "Remove it again with:  ADB=\"$ADB\" scripts/c1term-launcher-app.sh --remove"
