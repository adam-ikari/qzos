---
id: c1-slim-device
title: "目标设备 C1 Slim / MP-D261 规格（Ingenic XBurst MIPS32r2）"
category: reference
status: active
tags: [hardware, target, device]
created: "2026-09-29T00:51:31"
updated: "2026-09-29T02:33:59"
---

<!-- compiled_truth -->
移植目标硬件：**C1 Slim / MP-D261**（快易典 MagicPen 系列电子纸学习机），资料与构建方式参考 [Kasiin/C1-Slim-Ports](https://github.com/Kasiin/C1-Slim-Ports)。

## 规格（文档 + 真机 adb 探测一致）

| 项 | 值 |
|---|---|
| SoC | Ingenic XBurst V0.0（FPU V0.0），halley6_v20 |
| CPU/ABI | MIPS32r2 little-endian，Linux hard-float（o32） |
| 内核 | Linux 5.10.186，`PREEMPT` |
| 内存 | ~50 MiB |
| 屏幕 | 296×152 一位黑白墨水屏，`/dev/epaper_lcd`，一帧 5624 字节；帧布局 = 8 行一条带：`offset=(y/8)*296+x`，`mask=0x80>>(y%8)`，置位=黑 |
| 键盘 | `/dev/input/event0`（matrix keypad）+ `/dev/input/event1`（gpio keys）；无 `/dev/uinput` |
| 分区 | `/`(root) ext4 **只读**；`/usr/data` mmcblk0p6、`/usr/resource` mmcblk0p5、`/storage` mmcblk0p8 均 ext4 rw 可执行 |
| init | BusyBox init（`/sbin/init -> ../bin/busybox`），`/etc/inittab` 里 `::sysinit:/etc/init.d/rcS`，rcS 顺序跑 `S??*`；桌面 = `S80app` → `/etc/app_daemon` → `c1updater supervise` |
| Shell | `/bin/bash` 存在；root ADB（`uid=0`）；**没有 `timeout` applet** |

## 本测试机（MagicPen-206892）固件状态

- **桌面不是原厂 `mpenMain`**，而是 **C1ancher**：`/usr/data/c1/core/current/artifacts/`（`C1ancher`、`C1ancher-launcher`、`c1pkg`、`c1updater`），core 目录是 ed25519 签名清单（`manifest.v1` + `.sig`），由 `c1updater supervise` 监督。`pidof mpenMain` 为空。
- rootfs 只读但**可 remount rw 写入**（无 dm-verity，实测 `touch` 成功且掉电保留）；`app_daemon` 自己在装槽位时也走 `mount -o remount,rw /`。
- `/dev/epaper_lcd` 只写不可读（`dd` 返回 Invalid argument）→ 无法远程截屏，屏上效果需人工确认。
- C1ancher 不 grab `/dev/input`，c1term 的 `EVIOCGRAB` 可直接成功。
- 应用页/配网/射频的机制各自成页：[[c1ancher-app-integration]]（launcher 应用契约）、[[c1-wifi-stack]]（atbm603x 按需 insmod、生效配置在 `/usr/resource`）。

## 注意事项

- XBurst 对未对齐访问敏感——qzjs 的 `minimal` 档位（WAMR OFF）避开最大风险面。
- C1Terminal 已由本仓库构建并安装到 `/usr/data/c1term/`（见 [[port-verification]]）。
- **进屏幕有两条路，优先级已变**：首选**桌面 launcher 里选 terminal**（c1pkg 的 external-app 锁交接，桌面自己让屏、自己接回）；`c1term-run.sh` 那套 SIGSTOP 掉 `/usr/data/c1/core/*` 的做法只作 adb 调试用。两个 c1term 会抢 `/dev/input`，不要同时开。


## Timeline

- time: 2026-09-29T00:51:31
  kind: decision
  summary: "Created this page: 目标设备 C1 Slim / MP-D261 规格（Ingenic XBurst MIPS32r2）"
  source: "https://github.com/Kasiin/C1-Slim-Ports + 真机 adb 探测"
  affects: [c1-slim-device]

- time: 2026-09-29T00:51:56
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: brain update-truth
  affects: [c1-slim-device]

- time: 2026-09-29T01:11:19
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: brain update-truth
  affects: [c1-slim-device]

- time: 2026-09-29T01:26:55
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: "真机 adb 反复试验：分离会话的 setsid 启动被 adbd 回收，单会话内启动+等待后成功"
  affects: [c1-slim-device]

- time: 2026-09-29T02:33:51
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: "launcher 集成与配网 CLI 落地 2026-09-29"
  affects: [c1-slim-device]

- time: 2026-09-29T02:33:59
  kind: reversal
  summary: "推翻「C1-Slim-Ports 的 SIGSTOP mpenMain 桥在此机改为挂起 /usr/data/c1/core/* 进程」这一做法作为主路径的必要性：C1ancher 自身提供 external-app 交接（c1pkg 持 /dev/shm/c1ancher-external-app.{lock,runlock} 并写 .mode=direct，桌面全程 S 不被暂停，让出 /dev/epaper_lcd）。屏上终端改走 launcher 应用路径，SIGSTOP 桥降级为 adb 侧调试用法"
  source: "scripts/c1term-launcher-app.sh 真机验证 2026-09-29"
  affects: [c1-slim-device, c1ancher-app-integration]
