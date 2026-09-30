#!/usr/bin/env bash
# Install / remove the boot-time autostart of the on-screen terminal.
#   ADB="adb.exe -s MagicPen-206892" scripts/c1term-autostart.sh --enable
#   ... scripts/c1term-autostart.sh --disable | --status | --remove
#
# BusyBox init runs /etc/init.d/S??* from rcS. The rootfs is read-only, so we
# remount rw only long enough to write a 4-line stub; all real logic stays in
# the writable /usr/data/c1term/c1term-boot.sh and is upgradeable without
# touching the system partition again.
set -uo pipefail
cd "$(dirname "$0")/.."
ADB=${ADB:-adb}
read -r -a ADB_CMD <<<"$ADB"

D=/usr/data/c1term
STUB=/etc/init.d/S99c1term

adb_sh() { "${ADB_CMD[@]}" shell "$@" 2>&1 | tr -d '\r'; }

if ! "${ADB_CMD[@]}" get-state >/dev/null 2>&1; then
  echo "device unreachable via '$ADB'" >&2
  exit 1
fi

case "${1:-}" in
  --status)
    printf 'rootfs stub:  %s\n' "$(adb_sh "test -f $STUB && echo installed || echo absent")"
    printf 'disable flag: %s\n' "$(adb_sh "test -e $D/disable && echo present || echo absent")"
    printf 'boot logic:   %s\n' "$(adb_sh "test -x $D/c1term-boot.sh && echo present || echo absent")"
    printf 'rootfs mount: %s\n' "$(adb_sh "grep ' / ' /proc/mounts")"
    exit 0
    ;;

  --disable)
    adb_sh "touch $D/disable && chmod 666 $D/disable"
    echo "disable flag set — the terminal will not auto-start on the next boot"
    echo "re-enable with:  ADB=\"$ADB\" scripts/c1term-autostart.sh --enable"
    exit 0
    ;;

  --remove)
    adb_sh "mount -o remount,rw / && rm -f $STUB && sync; mount -o remount,ro /"
    echo "removed $STUB (boot logic under $D left in place)"
    exit 0
    ;;

  --enable)
    if [ -n "$(adb_sh "test -x $D/c1term -a -x $D/c1term-run.sh || echo MISSING")" ]; then
      echo "C1Terminal not installed — run:  ADB=\"$ADB\" scripts/c1term-install.sh" >&2
      exit 1
    fi
    ;;

  *)
    echo "usage: ADB=\"...\" scripts/c1term-autostart.sh --enable|--disable|--status|--remove" >&2
    exit 2
    ;;
esac

# ── device-side boot logic (writable partition) ──────────────────────────────
BOOT=$(mktemp)
cat > "$BOOT" <<'BOOT_EOF'
#!/bin/sh
# Started by /etc/init.d/S99c1term at boot. /usr/data is mounted by S21data in
# the background and the desktop is launched by c1updater, so both may lag.
# Wait for the desktop, then hand the screen to c1term.
D=/usr/data/c1term
LOG=/tmp/c1term-boot.log

[ -e "$D/disable" ] && exit 0

i=0
while [ $i -lt 90 ]; do
  [ -x "$D/c1term-run.sh" ] || exit 0
  desktop=""
  for p in $(ls /proc | grep -E '^[0-9]+$'); do
    e=$(readlink /proc/$p/exe 2>/dev/null)
    case "$e" in /usr/data/c1/core/*) desktop=yes ;; esac
  done
  [ -n "$desktop" ] && break
  i=$((i + 1))
  sleep 1
done

echo "$(cut -d. -f1 /proc/uptime)s: starting c1term (desktop=${desktop:-none})" >>$LOG
setsid $D/c1term-run.sh </dev/null >>$LOG 2>&1
echo "$(cut -d. -f1 /proc/uptime)s: c1term exited" >>$LOG
BOOT_EOF

"${ADB_CMD[@]}" push "$BOOT" $D/c1term-boot.sh >/dev/null 2>&1 || {
  echo "push failed" >&2; rm -f "$BOOT"; exit 1; }
rm -f "$BOOT"
adb_sh "chmod 755 $D/c1term-boot.sh && rm -f $D/disable" >/dev/null

# ── the read-only rootfs stub ────────────────────────────────────────────────
OUT=$(adb_sh "
mount -o remount,rw / || { echo 'REMOUNT_RW_FAILED'; exit 1; }
printf '#!/bin/sh\n[ \"\$1\" = start ] || exit 0\nsetsid $D/c1term-boot.sh &\nexit 0\n' > $STUB
chmod 755 $STUB
sync
mount -o remount,ro / || echo 'REMOUNT_RO_FAILED'
test -f $STUB && echo STUB_OK
")
printf '%s\n' "$OUT" | grep -v '^$'
if ! printf '%s\n' "$OUT" | grep -q STUB_OK; then
  echo "could not write $STUB" >&2
  exit 1
fi

echo
echo "installed. On the next boot the terminal takes the screen by itself;"
echo "press HOME in the terminal to hand it back to the desktop."
echo "turn it off later with:  ADB=\"$ADB\" scripts/c1term-autostart.sh --disable"
