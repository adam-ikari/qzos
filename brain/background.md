---
slug: background
title: Project background
role: project background
updated: "2026-09-29T00:52:29"
---

# Project background

## Why

把 [qzjs](https://github.com/adam-ikari/qzjs)（libuv 原生、WinterTC 兼容的嵌入式 JS 运行时）移植到
**C1 Slim / MP-D261 墨水屏学习机**（Ingenic XBurst，MIPS32r2 hard-float，~50 MiB RAM），
让这台小设备成为能跑标准 Web API 风格 JavaScript 的便携终端。详见 [[c1-slim-device]]。

## Goals

- 交叉编译出 **静态链接的 `qzjs` CLI**，在设备上运行**交互式 REPL**（逐行求值、会话状态保持）。
- 构建全程可复现：工具链脚本 + CMake toolchain 文件 + qemu-user 无设备回归（[[qemu-isolated-trampoline]]）。
- 经 root ADB 一键部署并做设备端冒烟（[[port-verification]]）。

## Non-goals

- 不做设备原语（GPIO/墨水屏驱动抽象）——qzjs 定位是通用运行时，硬件抽象属宿主层。
- 不重写 qzjs 的 JS 引擎/事件循环；移植=构建+适配+验证。
- 暂不做墨水屏专用 REPL UI（先走 C1Terminal/PTY 文本终端）。

## Target user

设备持有者本人（极客场景）：在 296×152 黑白屏 + 实体键盘上，用 WinterTC JS 做脚本、小工具与联网原型。
