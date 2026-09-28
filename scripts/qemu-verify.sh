#!/usr/bin/env bash
# Verify the MIPS build under qemu-user (no device needed).
# ISOLATED model: the host qzjs forks/execs qzjs-rt; under qemu-user the kernel
# cannot exec a MIPS binary directly, so QZ_RT_SERVER points at a trampoline
# that re-invokes the guest rt through qemu while preserving argv[0].
set -uo pipefail
cd "$(dirname "$0")/.."
[ -x .tools/qemu-mipsel-static ] || scripts/fetch-tools.sh

Q=".tools/qemu-mipsel-static"
BD="${BUILD_DIR:-build-mips}"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp "$BD/qzjs" "$BD/qzjs-rt" "$WORK/"
cat > "$WORK/rt-qemu" <<EOF
#!/bin/sh
exec "$PWD/$Q" -0 "\$0" "$WORK/qzjs-rt" "\$@"
EOF
chmod +x "$WORK/rt-qemu"
export QZ_RT_SERVER="$WORK/rt-qemu"

pass=0; fail=0
check() { # name expected-substring command...
  local name=$1 want=$2; shift 2
  local out; out=$("$@" 2>&1)
  if grep -qF -- "$want" <<<"$out"; then
    echo "  PASS  $name"; pass=$((pass+1))
  else
    echo "  FAIL  $name (wanted '$want')"; sed 's/^/        /' <<<"$out"; fail=$((fail+1))
  fi
}

runq() { (cd "$WORK" && "$OLDPWD/$Q" ./qzjs "$@"); }
echo "==> qemu-user verification ($BD)"

check "version"        "qzjs 0.2.0"                      runq --version
check "eval"           "42"                              runq -e 'console.log(6*7)'
check "repl-session"   "42"                              bash -c \
  "cd '$WORK'; printf '1+1\nvar x=40\nx+2\n' | '$OLDPWD/$Q' ./qzjs"
check "repl-banner"    "WinterTC runtime"                bash -c \
  "cd '$WORK'; printf '1\n' | '$OLDPWD/$Q' ./qzjs"
check "crypto-subtle"  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" \
  runq -e '(async()=>{const d=await crypto.subtle.digest("SHA-256",new TextEncoder().encode("abc"));console.log(Array.from(new Uint8Array(d)).map(b=>("0"+b.toString(16)).slice(-2)).join(""))})()'
check "timers-await"   "timer ok"                        runq -e '(async()=>{await new Promise(r=>setTimeout(r,5));console.log("timer ok")})()'
check "fetch-global"   "function"                        runq -e 'console.log(typeof fetch)'
check "streams"        "stream"                          runq -e '(async()=>{console.log(await new Response("stream").text())})()'

echo "==> $pass passed, $fail failed"
[ "$fail" -eq 0 ]
