---
id: qzos-power-sim
title: "电源域：描述符层 + 场景矩阵 + 假设台账（无真机条件下的闸门 0 替代路径）"
category: decision
status: active
tags: [power, suspend, shutdown, simulator, gate0]
created: "2026-09-30T12:27:12"
updated: "2026-09-30T12:27:29"
---

<!-- compiled_truth -->
## 决策：无真机条件下的电源域——描述符层 + 场景矩阵 + 假设台账

设计稿 `os/docs/power-sim.md`。已实现：`os/src/power.{c,h}`（描述符 + 决策）、
`scripts/test-power.sh`（43 断言）、`scripts/probe-power-owner.sh`（真机探测）。

### 诚实的框定：模拟器不回答闸门 0 的事实问题

「这台设备的电源键现在归谁」是设备属性，模拟器做得再逼真也答不了。任何
「仿真通过所以电源没问题」的说法都是错的。

它能做且值得做的三件事：
1. **把未知收敛到一个薄适配层**（照 `panel.c` 的描述符模式），未知的部分只剩
   「碰 sysfs / 发信号 / 调命令」那 20 行
2. **把闸门 0 的三问变成场景矩阵**，写成可测代码：风险形态从「猜错了」变成
   「猜错了但会被发现」
3. **现在就能建 + 测 qzos 侧全部代码**（状态机、决策、拒绝路径）

净效果：实现风险关掉大部分，**事实风险仍全开** —— 但后者从「一次无底洞调查」
变成「插上设备时照清单逐条核对」。

### 描述符层

`qzos_power_t` 与 `qzos_panel_t` 同构：名字查询 + 环境覆盖 + 能力位。三个内置
后端刻意对应「知道多少」的三档而非「设备型号」三档：`none`（缺省=不动）、
`sim-fake`（路径全在 env 里，指向假文件树）、`mp-d261-unverified`（**猜测形状**）。

**仿真不为仿真发明任何机制**：qzos 访问设备本来就走路径，把路径指向临时目录里
的假文件就是仿真。电源键则复用**已有的 FIFO 回放通道**（`QZ_INPUT0` +
`replay-keys.py`，已为此加了 `power`/`sleep` 键名，键值取自 `linux/input.h`
的 KEY_POWER=116 / KEY_SLEEP=142）。

### 场景矩阵（本层真正的价值）

| 场景 | key_owner | qzos 应该 |
| --- | --- | --- |
| A | `kernel` | 只读状态；不重复触发 |
| B | `vendor:<path>` | 转发给守护进程，而非绕过（绕过会打架） |
| C | `none` | 全权接管 |
| D | `unknown` | **拒绝一切写动作**，只读 + 屏上明示 |

**场景 D 是最要紧的**：`key_owner` 因此是**运行时可探测**字段而非编译期常量。
在没查清的设备上，唯一安全的动作是「不动」。

### 已落地的安全关键部分

`qzos_power_may_act()` 是纯函数、无 I/O、设备无关，是这一层唯一能在没有真机时
被真正验证的部分。43 断言覆盖四场景矩阵 + 能力位/路径必须成对 + 缺省描述符
必须拒高危动作。

`mp-d261-unverified` 把 `cap_suspend`/`cap_shutdown` 设成 **false**：P3/P4 未验时
让任何误用都被 `may_act` 挡住。理由是若设备没有可写的 poweroff sysfs，「关机」
只能靠 sysrq 或直接断电，**可能损坏文件系统**，而这台设备熄屏后只能靠 USB ADB
救。宁可不做。

### 假设台账 P1–P7（未验，逐条对应「怎么验」）

P1 电源键码 · P2 键归属 · P3 可写 suspend sysfs · P4 安全 poweroff 路径 ·
P5 熄屏后唤醒方式 · P6 电池节点 · P7 关机时 rootfs 状态持久性。
**P3/P4 最危险**，理由见上。`probe-power-owner.sh` 覆盖 P1–P6 的只读探测，
P5 需人（熄屏后按键实按）。

### 闸门自身的一个教训（重要）

写完 `test_power.c` 后跑变异测试：把「未知归属全拒」那条不变量从 `power.c`
删掉 —— **43 条断言照样全绿**。原因是 `ok()` 只把失败名字记进数组、**漏了
`failed++`**，所以结尾 `if (failed == 0) printf("OK")` 永远走真。

**闸门变成了装饰。** 与 brain 里「回执层面的断言不能替代效果层面的断言」同一个
病：判据必须能发现自己坏了。修好后 4 个 mutant 全部会红（未知归属不拒 4 条、
内核场景 4 条、厂商场景 3 条、关掉 shutdown 能力检查 3 条）。

这条之所以值得记：**它是在「闸门刚写出来、还没被信任」的时候就被抓到的**，
而不是等到某天怀疑它没在测。

### 明确不做

- 不做整机模拟器（模拟 MIPS / epaper 驱动 / 内核 input 子系统）。qemu-user +
  假 sysfs 树已够；再往上堆只会得到「在一台假设备上通过了」的错觉
- 不猜设备机制然后照着写。没查清就是场景 D：拒绝动作
- 不在 C 侧做电源策略（何时提示关机），那是 JS 侧的事（`qzos-js-first`）


## Timeline

- time: 2026-09-30T12:27:12
  kind: decision
  summary: "Created this page: 电源域：描述符层 + 场景矩阵 + 假设台账（无真机条件下的闸门 0 替代路径）"
  source: "用户定方向：跳过真机测试，设计模拟器方案"
  affects: [qzos-power-sim]

- time: 2026-09-30T12:27:12
  kind: decision
  summary: "模拟器不回答闸门 0 的事实问题，只做三件事：把未知收敛到薄适配层（照 panel.c 描述符模式）、把三问变成可测的场景矩阵、现在就能建+测 qzos 侧代码。核心不变量 key_owner==UNKNOWN 则一切写动作被拒（防双重处理）。已实现 power.c 决策层 43 断言 + probe-power-owner.sh 探测脚本；P1–P7 全部未验，mp-d261-unverified 描述符故意关掉高危能力位"
  source: brain update-truth
  affects: [qzos-power-sim]

- time: 2026-09-30T12:27:29
  kind: evidence
  summary: "闸门自身出过一次真事故且被抓到：test_power.c 的 ok() 只把失败名记进数组、漏了 failed++，导致结尾 if (failed==0) printf(\"OK\") 永远走真——把「未知归属全拒」那条核心不变量从 power.c 删掉，43 条断言照样全绿。修好后 4 个 mutant 全红。教训：判据要能发现自己坏了，与「回执层面的断言不能替代效果层面的断言」同病；且这条是在闸门刚写出来、还没被信任时抓到的，不是等到某天怀疑"
  source: "brain append-timeline：变异测试"
  affects: [qzos-power-sim, port-verification]
