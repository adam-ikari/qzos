---
id: zig-musl-cross-build
title: "交叉编译工具链：Zig 0.14.1 + mipsel-linux-musleabihf 静态"
category: decision
status: active
tags: [toolchain, build]
created: "2026-09-29T00:51:31"
updated: "2026-09-29T00:51:56"
---

<!-- compiled_truth -->
用 **Zig 0.14.1 的 `zig cc -target mipsel-linux-musleabihf`** 作为交叉编译器（与 C1-Slim-Ports 同路线：musl 静态、单下载即用），CMake 3.28 + Ninja 1.12 驱动 qzjs 构建；全部产物静态链接，直接 `adb push` 到 `/storage` 运行。

- toolchain 文件：`cmake/toolchains/mipsel-zig.cmake`（编译器包装脚本在 `.tools/bin/zig-cc-mipsel`，`CMAKE_SYSTEM_PROCESSOR=mips`）
- 档位：`-DQZ_PROFILE=minimal -DCMAKE_BUILD_TYPE=Release`（crypto/compress/textcodec ON；TLS、WAMR OFF），产物 `qzjs`/`qzjs-rt` 各约 2.9 MB

## 关键坑：zig cc 无条件嵌入 DWARF

即使传 `-g0`，`zig cc` 链接出的可执行文件仍带 `.debug_*`（10 MB+）；`zig objcopy --strip-all` 只删 symtab 不删 debug 段，宿主 x86 `strip` 不认 MIPS ELF。
解法：在 toolchain 里 `set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--strip-all -Wl,--build-id=none")`，链接期由 lld 完成剥离（→ 2.9 MB）。


## Timeline

- time: 2026-09-29T00:51:31
  kind: decision
  summary: "Created this page: 交叉编译工具链：Zig 0.14.1 + mipsel-linux-musleabihf 静态"
  source: "移植方案拍板 2026-09-29"
  affects: [zig-musl-cross-build]

- time: 2026-09-29T00:51:56
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: brain update-truth
  affects: [zig-musl-cross-build]
