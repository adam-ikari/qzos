#!/usr/bin/env bash
# Download cross-build tools into .tools/ (zig, ninja, qemu-mipsel). Idempotent.
set -euo pipefail
cd "$(dirname "$0")/.."
TOOLS="$PWD/.tools"
mkdir -p "$TOOLS"

ZIG_VER=0.14.1
NINJA_VER=1.12.1

if [ ! -x "$TOOLS/zig/zig" ]; then
  echo "==> zig $ZIG_VER"
  curl -fL --retry 3 -o "$TOOLS/zig.tar.xz" \
    "https://ziglang.org/download/$ZIG_VER/zig-x86_64-linux-$ZIG_VER.tar.xz"
  tar xf "$TOOLS/zig.tar.xz" -C "$TOOLS"
  mv "$TOOLS/zig-x86_64-linux-$ZIG_VER" "$TOOLS/zig"
  rm "$TOOLS/zig.tar.xz"
fi

if [ ! -x "$TOOLS/ninja" ]; then
  echo "==> ninja $NINJA_VER"
  curl -fL --retry 3 -o "$TOOLS/ninja.zip" \
    "https://github.com/ninja-build/ninja/releases/download/v$NINJA_VER/ninja-linux.zip"
  python3 -m zipfile -e "$TOOLS/ninja.zip" "$TOOLS"
  rm "$TOOLS/ninja.zip"
  chmod +x "$TOOLS/ninja"
fi

if [ ! -x "$TOOLS/qemu-mipsel-static" ]; then
  echo "==> qemu-user-static (mipsel)"
  tmp=$(mktemp -d)
  if (cd "$tmp" && apt-get download qemu-user-static >/dev/null 2>&1); then
    (cd "$tmp" && dpkg -x qemu-user-static_*.deb x)
    cp "$tmp/x/usr/bin/qemu-mipsel-static" "$TOOLS/qemu-mipsel-static"
    chmod +x "$TOOLS/qemu-mipsel-static"
    rm -rf "$tmp"
  else
    echo "WARN: could not fetch qemu-user-static via apt; install qemu-user-static manually." >&2
  fi
fi

echo "==> tools ready:"
"$TOOLS/zig/zig" version && "$TOOLS/ninja" --version
