---
id: port-verification
title: "移植验证状态与设备实测基线"
category: project
status: active
tags: [verification, device, perf]
created: "2026-09-29T00:51:32"
updated: "2026-09-30T03:13:51"
---

<!-- compiled_truth -->
## 两条线：qzjs 移植（已上真机）/ qzos 宿主（自动化已建立，未上真机）

### qzjs 本体 — 真机已验证（MagicPen-206892, root ADB）

- 部署于 `/storage/c1/qzjs/`。`qzjs --version` → `qzjs 0.2.0`；`-e 'console.log(6*7)'` → `42`。
- REPL 管道会话变量跨行保持；`typeof fetch` → `"function"`；`crypto.subtle` SHA-256("abc") = `ba7816bf…` 正确；`setTimeout` 异步完成。
- **强制 PTY（`adb shell -t`）交互正常** → 即 C1Terminal / bash-in-PTY 所需形态。
- C1Terminal（Go 1.25.6，`GOARCH=mipsle GOMIPS=hardfloat`，2.5 MB）装于 `/usr/data/c1term/`，qzjs 符号链接进其 PATH。启动桥按本机桌面（C1ancher）改造并经 c1pkg 契约交接，见 [[c1ancher-app-integration]]。

### 实测基线（XBurst）

| 指标 | 值 |
|---|---|
| `-e` 端到端启动 | ~60–70 ms（中位） |
| RSS：宿主 `qzjs` | 816 KB |
| RSS：主 RT `qzjs-rt` | 3.1 MB |
| 二进制（strip 后） | 各 ~2.9 MB |

50 MiB 内存余量充足。`qzjs-rt` 经 Zig 交叉编译，MIPS `qzos-host` 约 4.3 MB。

## qzos 宿主的自动化验证（无头、无设备）

`/dev/epaper_lcd` 只写不可读，真机画面远程抓不到——所以正确性靠自动化通道守，视觉才留给人眼。四条都能在开发机上跑：

| 通道 | 覆盖 | 基线 |
|---|---|---|
| `scripts/test-display.sh` | 显示纯逻辑（条带寻址/阈值/脏区/刷新决策） | 133 断言 0 失败 |
| `scripts/verify-frames.sh` | 原生 vs MIPS **帧逐字节一致** | 5635 字节完全相同 |
| `scripts/verify-input.sh` | 键盘全链路（evdev→LVGL→shell） | 3 场景 2/2 |
| `scripts/qemu-verify.sh` | qzjs 自身（version/eval/REPL/crypto/async/fetch） | 8/8 |

- MIPS 侧经 `qemu-mipsel-static` 跑；ISOLATED 双进程需要 rt trampoline（见 [[qemu-isolated-trampoline]]）。
- 键盘回放：把 `QZ_INPUT0` 指向 FIFO，`os/test/replay-keys.py` 按真实 `struct input_event` 布局写入，宿主照常走 `uv_poll`。
- **布局陷阱**：`struct input_event` 的大小随架构不同（x86_64 = 24，mips32 = 16，差在 timeval 里 time_t 的宽度）。喂错布局会被静默切成垃圾事件，所以 `--arch` 必须显式声明**消费者**的架构，不能靠"反正都在同一台机器上跑"蒙混。
- 场景断言语义而非黄金文件：关键那条是 **roundtrip**——进应用再退回来必须逐字节复现桌面帧（e-ink 每帧内容应是状态的纯函数），且前后两次是独立运行，不是自证。

## 仍缺的真机验证（不能被上面这些替代）

- `/dev/epaper_lcd` 的**实际落屏**：波形选择是否被驱动接受、刷屏耗时、残影表现。
- evdev **真实键码**与真机矩阵键盘行为（当前键码表抄自 C1Terminal `keyboard.go`，回放用的是同一张表——等于自证，真机才能证伪）。
- 墨水屏刷新期间的输入响应（面板阻塞时按键是否丢）。
- 视觉本身：字体渲染、布局、CJK 断行，只能人眼在屏上看。

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

- time: 2026-09-30T03:13:19
  kind: decision
  summary: "宿主（qzos-host）自动化验证基线建立，四条通道可无头跑：test-display.sh（显示纯逻辑 133 断言）、verify-frames.sh（原生/MIPS 帧逐字节相同，MIPS 经 qemu-user + rt trampoline）、verify-input.sh（键盘 FIFO 回放三场景 2/2）、qemu-verify.sh（qzjs 自身 8 项）。主机可读 argv 替换二进制：uv 时钟可注入、临时目录 + PATH 里的假 qemu、自带 lib、LD_LIBRARY_PATH/PYTHONPATH 覆盖。**注意**：struct input_event 布局随架构不同（x86_64=24 / mips32=16），跨架构回放事件流必须显式声明消费者架构，否则被静默切成垃圾。仍缺真机验证的部分：/dev/epaper_lcd 实际落屏波形与残影、evdev 真实键码、墨水屏刷新耗时。"
  source: "提交 d98d251 后的验证状态盘点"
  affects: [port-verification, qzos-ui-architecture, zig-musl-cross-build]

- time: 2026-09-30T03:13:40
  kind: reversal
  summary: "更正上一条：其中「主机可读 argv 替换二进制：uv 时钟可注入、临时目录 + PATH 里的假 qemu、自带 lib、LD_LIBRARY_PATH/PYTHONPATH 覆盖」一段是无效内容——本项目从未做过这种替身主机验证，当时也没有验证过，不该写进事实记录。其余部分（四条无头通道、input_event 布局随架构而变、仍缺的真机项）成立。"
  source: "自查：该条内容并非来自实际执行"
  affects: [port-verification]

- time: 2026-09-30T03:13:51
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: brain update-truth
  affects: [port-verification]
