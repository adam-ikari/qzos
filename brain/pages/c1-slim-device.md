---
id: c1-slim-device
title: "目标设备 C1 Slim / MP-D261 规格（Ingenic XBurst MIPS32r2）"
category: reference
status: active
tags: [hardware, target, device]
created: "2026-09-29T00:51:31"
updated: "2026-09-29T01:26:55"
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
| 分区 | `/`(root) ext4 **只读**；`/storage` mmcblk0p8 ext4 rw 可执行；`/usr/data` mmcblk0p6 ext4 rw 可执行（91MB） |
| Shell | `/bin/bash` 存在；root ADB（`uid=0`） |

## 本测试机（MagicPen-206892）固件状态

- **桌面不是原厂 `mpenMain`**，而是 **C1ancher**：`/usr/data/c1/core/current/artifacts/C1ancher-launcher`（argv[0] 伪名为 `{app_daemon}`）+ `C1ancher app` + `c1pkg`，由 `/etc/c1updater/.../c1updater supervise` 监督。`pidof mpenMain` 为空。
- 因此 C1-Slim-Ports 的启动桥（SIGSTOP mpenMain）在此机需改为：**挂起所有 exe 位于 `/usr/data/c1/core/*` 的进程**，c1term 退出后再 CONT。
- `/dev/epaper_lcd` 只写不可读（`dd` 返回 Invalid argument）→ 无法远程截屏，屏上效果需人工确认。
- C1ancher 不 grab `/dev/input`，c1term 的 `EVIOCGRAB` 可直接成功。

## 后台启动常驻进程的正确姿势（adb）

**adbd 会回收立即退出的 adb 会话下的子进程**：`adb shell "setsid foo & "`（会话立刻结束）里的作业还没来得及 detach 就被杀掉，表现为进程"起来了又消失"，且日志文件根本没生成。`setsid` / `nohup` 单独都不够。

可用写法：**启动与观察放在同一个 adb 会话内**，让会话多活几秒给作业完成脱离：

```
adb shell "setsid /usr/data/c1term/c1term-run.sh </dev/null >/tmp/c1term.log 2>&1 & sleep 4; echo PID=\$(pidof c1term)"
```

注意远程命令串用**单引号**包起来，`$(...)` 才会在设备上展开；写成双引号会被本地 shell 抢先展开。

## 注意事项

- XBurst 对未对齐访问敏感——qzjs 的 `minimal` 档位（WAMR OFF）避开最大风险面。
- C1Terminal 已经由本仓库构建并安装到 `/usr/data/c1term/`（见 [[port-verification]]）。


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
