---
id: qemu-isolated-trampoline
title: "qemu-user 下验证 ISOLATED 双进程：QZ_RT_SERVER trampoline"
category: decision
status: active
tags: [test, qemu, build]
created: "2026-09-29T00:51:32"
updated: "2026-09-29T00:51:56"
---

<!-- compiled_truth -->
qzjs 默认 ISOLATED 进程模型：宿主 `qzjs` fork+exec 同目录 `qzjs-rt`（解析链：显式路径 → `QZ_RT_SERVER` 环境变量 → `/proc/self/exe` 同目录 → 编译期 `QZ_RT_PATH`，见 `qzjs/src/ipc_process.c`）。在 qemu-user（`qemu-mipsel-static`）下宿主内核无法直接 exec MIPS 二进制，`-e`/REPL 会报 `qzjs: runtime init failed`。

方案：把 `QZ_RT_SERVER` 指到一个宿主 shell trampoline，由它重新经 qemu 拉起 guest rt，并用 `-0 "$0"` 保留 guest argv[0]：

```sh
#!/bin/sh
exec <repo>/.tools/qemu-mipsel-static -0 "$0" <workdir>/qzjs-rt "$@"
```

已固化在 `scripts/qemu-verify.sh`（8 项冒烟：version/eval/REPL 多行/banner/crypto SHA-256 已知答案/setTimeout/typeof fetch/Response.text）。无设备即可回归。真机不需要 trampoline（原生 exec 正常）。


## Timeline

- time: 2026-09-29T00:51:32
  kind: decision
  summary: "Created this page: qemu-user 下验证 ISOLATED 双进程：QZ_RT_SERVER trampoline"
  source: "移植过程 2026-09-29"
  affects: [qemu-isolated-trampoline]

- time: 2026-09-29T00:51:56
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: brain update-truth
  affects: [qemu-isolated-trampoline]
