---
id: port-verification
title: "移植验证状态与设备实测基线"
category: project
status: active
tags: [verification, device, perf]
created: "2026-09-29T00:51:32"
updated: "2026-09-29T01:27:28"
---

<!-- compiled_truth -->
## 验证状态（2026-09-29）

### M1 — qzjs 本体跑通

- **qemu-user**：8/8 冒烟通过（`scripts/qemu-verify.sh`）。
- **真机 MagicPen-206892（root ADB）**：部署于 `/storage/c1/qzjs/`。
  - `qzjs --version` → `qzjs 0.2.0`；`-e 'console.log(6*7)'` → `42`
  - REPL 管道会话：变量跨行保持；`typeof fetch` → `"function"`
  - `crypto.subtle` SHA-256("abc") = `ba7816bf…` 正确；`setTimeout` 异步完成
  - **强制 PTY（`adb shell -t`）交互**正常 → 即 C1Terminal/bash-in-PTY 所需形态

### M2 — 屏上 REPL（C1Terminal）

- C1Terminal（Go 1.25.6，`GOARCH=mipsle GOMIPS=hardfloat CGO_ENABLED=0`，2.5 MB）装于 `/usr/data/c1term/`，qzjs 符号链接进其 `PATH`。
- 启动桥按本机桌面（C1ancher）改造：SIGSTOP `/usr/data/c1/core/*`，c1term 退出后 CONT；反悔用 `c1term-stop.sh`。
- 一键脚本 `scripts/on-device-repl.sh`（`--stop` 关闭）当前**稳定成功**：c1term 进程存活、C1ancher 处于 `T`、`/tmp/c1term.log` 为空。
- **仍缺人工目视确认**：`/dev/epaper_lcd` 只写不可读，远程无法截屏。屏上应见 `c1slim#` 提示符，输入 `qzjs` 进入 REPL。

## 实测基线（XBurst）

| 指标 | 值 |
|---|---|
| `-e` 端到端启动 | ~60–70 ms（中位） |
| RSS：宿主 `qzjs` | 816 KB |
| RSS：主RT `qzjs-rt` | 3.1 MB |
| 二进制（strip 后） | 各 ~2.9 MB |

50 MiB 内存余量充足。

## 上游行为备忘（非移植缺陷）

- `-e`/REPL 的 eval 通道按 classic script 求值，**不支持顶层 `await`**（连脚本文件模式也不支持）；交互用 `.then()` / async IIFE。
- `fs` 未作为全局暴露；`new Blob().stream` 相关 `CompressionStream` 链路报 `not a function`。


## Timeline

- time: 2026-09-29T00:51:32
  kind: decision
  summary: "Created this page: 移植验证状态与设备实测基线"
  source: "真机 adb 验证 2026-09-29"
  affects: [port-verification]

- time: 2026-09-29T00:51:57
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: brain update-truth
  affects: [port-verification]

- time: 2026-09-29T01:11:19
  kind: evidence
  summary: "M2 进行中：C1Terminal(go1.25.6 mipsle hardfloat, 2.5MB) 装至 /usr/data/c1term/，qzjs 桥入其 PATH；发现本机桌面为 C1ancher，启动桥改为挂起 /usr/data/c1/core/* 进程；setsid 分离后跨 adb 会话存活，日志无错"
  source: "2026-09-29 真机 adb 安装验证"
  affects: [port-verification]

- time: 2026-09-29T01:27:05
  kind: reversal
  summary: "推翻上一条M2"
  source: "scripts/on-device-repl.sh 修复后真机复跑 2026-09-29"
  affects: [port-verification, c1-slim-device]

- time: 2026-09-29T01:27:16
  kind: reversal
  summary: "推翻上一条 M2 进行中的存活结论：setsid 分离启动并不能跨 adb 会话存活（adbd 会回收立即退出会话的子进程）。改为把启动与 sleep 4 观察放在同一个 adb 会话内后稳定成功：c1term pid 存活、C1ancher 912/913 State=T、/tmp/c1term.log 为空。M2 至此在进程层面确认；墨水屏画面仍需人工目视（/dev/epaper_lcd 只写不可读）。注：本条替代上一条被引号截断的 reversal（时间线只追加，故保留）"
  source: "scripts/on-device-repl.sh 修复后真机复跑 2026-09-29"
  affects: [port-verification, c1-slim-device]

- time: 2026-09-29T01:27:28
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: "M2 真机复跑确认（scripts/on-device-repl.sh）"
  affects: [port-verification]
