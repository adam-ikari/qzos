#!/usr/bin/env bash
# scripts/check-mips-binary.sh — MIPS 闸门的前置条件：二进制确实是从当前依赖构建的
#
# 源文件用：  . "$(dirname "$0")/check-mips-binary.sh"
# 调它：      hz_require_mips_binary
#
# ## 为什么必须有这个检查
#
# MIPS 闸门（`verify-mips-e2e.sh` / `verify-frames.sh`）跑的是 `build-os-mips/qzos-host`，
# 而那个二进制**可能是上一次构建的**。实测踩过：qzjs 升到 960f24d5 之后 MIPS 构建
# 其实失败了 —— polyfill 的字节码要用**目标架构的** qjsc 生成，x86 宿主上
# `Exec format error` —— 而 `verify-mips-e2e` 拿 15:04 的旧二进制跑出 **10/10 全绿**。
#
# 那种绿比红危险：它让人以为 MIPS 那侧是验过的。而「子模块 gitlink 变了、二进制没跟上」
# 这个状态，只看「闸门全绿」是**发现不了**的 —— 闸门测的是二进制，不是依赖一致性。
#
# 判据落在 **SHA** 上而不是时间戳：时间戳只能证明「比某个文件新」，而真正要回答的
# 问题是「这个二进制是从哪份依赖构建出来的」。
#
# 戳由 `scripts/build-os.sh` 在**构建成功之后**写入 —— 所以「没有戳」本身就说明
# 那次构建没成功过。
hz_require_mips_binary() {
  local bin="${1:-build-os-mips/qzos-host}"
  local stamp="$(dirname "$bin")/.build-stamp"

  if [ ! -x "$bin" ]; then
    echo "  (no MIPS binary $bin — run scripts/build-os.sh --mips)" >&2
    return 1
  fi
  if [ ! -f "$stamp" ]; then
    echo "  (no build stamp $stamp — that build never succeeded, or predates it)" >&2
    echo "   run scripts/build-os.sh --mips" >&2
    return 1
  fi

  local want got bad=0
  local m
  for m in qzjs lvgl uvrpc; do
    case "$m" in
      qzjs)  want=$(git -C qzjs rev-parse HEAD 2>/dev/null || echo none) ;;
      lvgl)  want=$(git -C third_party/lvgl rev-parse HEAD 2>/dev/null || echo none) ;;
      uvrpc) want=$(git -C third_party/uvrpc rev-parse HEAD 2>/dev/null || echo none) ;;
    esac
    got=$(sed -n "s/^$m=//p" "$stamp" 2>/dev/null | head -1)
    if [ "$got" != "$want" ]; then
      echo "  (stale MIPS binary: $m is at ${want:0:8} but the binary was built from ${got:0:8})" >&2
      bad=1
    fi
  done
  [ "$bad" -eq 0 ] || { echo "   run scripts/build-os.sh --mips" >&2; return 1; }
  return 0
}
