---
id: qzos-as-system
title: "qzos 作为系统接管设备：替代范围、开机持锁机制与电源归属闸门"
category: decision
status: active
tags: [system, boot, powerservice, takeover]
created: "2026-09-30T04:19:05"
updated: "2026-09-30T10:03:30"
---

<!-- compiled_truth -->
## 决策：qzos 作为「系统」接管设备，逐项替换原有程序

目标从「在 C1 Slim 上跑起一个 qzos 应用」抬升为「**qzos 就是这台设备的系统**」：
开机即接管、桌面是 qzos shell、原厂程序逐项被 qzos 自己的服务替代。

### 接管机制：沿用 C1ancher 已验证的交接契约，而不是不可逆替换

C1ancher 与外部程序之间本来就有一套验证过的交接（见 [[c1ancher-app-integration]]）：
`.c1pkg-mode=direct` + exec 前 `flock` `.lock`/`.runlock`，fd 随 exec 继承；
拿不到锁的 C1ancher 停在 `do_sys_poll` 里不碰 `/dev/epaper_lcd`。开机时在 rcS 的
`S80app` 之前插一个脚本、让 qzos-host 持锁常驻，就等于用厂商自己的契约完成接管。

**关键性质：可退出。** 杀掉 qzos-host → 锁释放 → C1ancher 立刻接回屏幕。
这条「一个进程就能退回原厂系统」是整个计划的安全底座：任何时刻都能把设备退回
已知可用状态，不依赖救砖手段。**因此不做不可逆替换**——不改 rootfs 启动项、
不替换 c1pkg 信任锚、不删厂商守护进程。接管以「随时能退回」为前提。

### 闸门 0（阻塞项，必须先有真机）：电源归属尚未查清

**qzos 目前完全没有电源管理**：`keymap.c` 没有映射 `KEY_POWER`，宿主与 JS 里
没有任何 suspend / shutdown / reboot 路径。而这台设备熄屏后唯一恢复手段是
USB ADB（rootfs 只读、无 dm-verity 但可 remount）。所以「qzos 独占屏幕」必须和
「谁负责关机」一起回答，否则会做出**只能拔电的设备**。

上真机第一件事，查清三问：(a) 电源键现在由谁处理（内核 gpio / 厂商守护进程 /
C1ancher？）；(b) 安全的关机与休眠路径；(c) 熄屏后如何回到 qzos。
**这三条落定前不写任何接管代码。**

### 逐项替代清单

| 原程序 | 位置 | qzos 替代 | 状态 |
| --- | --- | --- | --- |
| C1ancher 桌面 / launcher | `S80app` → `app_daemon` → `c1updater supervise` | qzos shell + 开机持锁 | 闸门 0 后开工 |
| 电源管理 | **未记录** | `sys.power.*` | 闸门 0 |
| 厂商配网脚本 | atbm603x 按需 insmod + `/usr/resource` 生效配置 | `sys.net.*` | 待做（[[c1-wifi-stack]]） |
| 原厂应用体系 / c1pkg 仓库 | `/usr/data/c1/pkg/`（ed25519 信任锚） | qzos 应用目录（`/storage`） | **明确不做**：换信任锚会废掉厂商商店验签（[[c1ancher-app-integration]]） |
| C1Terminal（自家移植） | `/usr/data/c1term/` | 被 qzos 取代 | 已被取代 |

### 「替换所有程序」的实现载体是服务面，不是界面

把厂商守护进程的能力收进 qzos 自己的 uvrpc 服务面（[[qzos-services-rpc]]），再由
shell 与应用经 `op:rpc` 消费——所以推进单位是**服务**，不是界面。第一批服务只做
**只读、可用真机之外手段证伪**的那些（`sys.storage` / `sys.settings` /
`sys.power.capabilities`），写动作（关机、配网、重启）全部等闸门 0。

### 验证纪律沿用既有结论

`/dev/epaper_lcd` 只写不可读，屏上一切只能靠自动化通道守（见 [[port-verification]]）。
新增服务一律要求**效果层面**断言：例如 `sys.storage` 报的自由字节数要与 `statvfs`
真值相符，而不只是「RPC 回了东西」。


## Timeline

- time: 2026-09-30T04:19:05
  kind: decision
  summary: "Created this page: qzos 作为系统接管设备：替代范围、开机持锁机制与电源归属闸门"
  source: "用户定方向「qzos作为系统替换原来的所有程序」"
  affects: [qzos-as-system]

- time: 2026-09-30T04:19:18
  kind: decision
  summary: "qzos 抬升为设备系统：沿用 C1ancher 交接契约持锁接管（可退出为安全底座），逐项替代桌面/电源/配网，明确不碰 c1pkg 信任链；电源归属为闸门 0，未查清前不写接管代码"
  source: brain update-truth
  affects: [qzos-as-system]

- time: 2026-09-30T04:19:42
  kind: decision
  summary: "用户定方向：qzos 不再是设备上的一个应用，而是要作为系统替换原有全部程序。据此记录：接管走 C1ancher 已验证的 flock+direct 契约（可退出=安全底座，拒绝不可逆替换）；发现阻塞项——qzos 完全没有电源管理（keymap 无 KEY_POWER、无 suspend/shutdown），而设备熄屏后只能靠 ADB 救，故「独占屏幕」必须与「谁负责关机」一起解决，立为闸门 0"
  source: brain append-timeline
  affects: [qzos-as-system, qzos-services-rpc, qzos-ui-architecture]

- time: 2026-09-30T09:11:58
  kind: evidence
  summary: "实测出「系统化」方向最硬的缺口：qzjs-rt（JS 引擎进程）被杀后，qzos-host 继续正常运行、LVGL 继续 tick、面板上仍是完好的桌面画面、按键仍被读取，但**没有任何东西告诉用户桌面已经死了**——错误帧 main-runtime-process-exited-unexpectedly 被 bridge.c 当「非 op 消息」打到 stderr 就丢弃，shell 完全不知道。用户面对一个按任何键都没反应的僵尸桌面，唯一出路是重启设备。对「qzos 作为系统」这是致命的：桌面必须是可靠性的门面。恢复路径已确认可行（qz_destroy + qz_create + 重新 poll qz_message_fd，M-P7 契约下宿主不持有 rt 内部状态）"
  source: "brain append-timeline：/tmp/opencode/rtkill.sh 实测（pkill qzjs-rt 后观察 20s）"
  affects: [qzos-as-system, qzos-ui-architecture]

- time: 2026-09-30T10:03:30
  kind: decision
  summary: "JS 引擎崩溃恢复落地：bridge.c 认 qzjs 的 {type:error} 帧 → qzos_host_on_rt_death() 走「告知(屏上画 JS engine stopped + 倒计时) → 停输入(停 poll + 清队列) → 重建(qz_destroy+qz_create+重挂 fd+重跑 boot) → 恢复(开输入/撤提示/退避复位)」，退避 1s→30s 封顶；**开机时 rt 起不来也不退出**（墨水屏一体机宿主一退就是黑屏，只能等电池耗尽）。闸门 verify-rt-recovery.sh 11 项（杀 rt → 提示落屏 / rt 重建 / 桌面逐字节复现 / 开机失败留在退避循环且退避翻倍）。期间修掉一个必崩 bug：s_ev[] 静态零初始化使 fd==0 而非 -1，qzos_input_enable 用 fd<0 当守卫，对从未 uv_poll_init 的 handle 调 uv_poll_stop 直接段错误（gdb 定位）。另删掉 shell.js 里那个 setInterval 心跳——实测 25s 无按键主 RT 并不自退，前提不成立"
  source: "brain append-timeline：gdb 定位段错误 + 2 个变异测试确认闸门有效"
  affects: [qzos-as-system, qzos-ui-architecture]
