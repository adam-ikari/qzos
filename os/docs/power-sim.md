# 无真机条件下的电源域方案：描述符层 + 场景矩阵 + 假设台账

> 状态：设计稿。目标读者是实现者。
> 背景：[[qzos-as-system]] 的闸门 0（电源归属）需要真机，而当前没有设备。
> 决策记录见 brain `qzos-power-sim`。

## 0. 先说清楚这个方案不能做什么

**模拟器不回答闸门 0 的事实问题。** 「这台设备的电源键现在由谁处理」是设备属性，
模拟器做得再逼真也答不了。任何声称「仿真通过所以电源没问题」的说法都是错的。

它能做、且值得做的是三件事：

1. **把未知收敛到一个薄适配层。** 照 `os/src/panel.c` 已有的描述符模式做，
   未知的部分只剩「碰 sysfs / 发信号 / 调命令」那 20 行 —— 而那恰好是最难
   写对、也最不该在没有真机时凭想象写的一段。
2. **把闸门 0 的三问变成场景矩阵。** A/B/C 三种电源键归属各自要求 qzos 做什么，
   写成可测代码。风险形态从「猜错了」变成「猜错了但会被发现」。
3. **现在就能建 + 测 qzos 侧的全部代码**：按键路由、状态机、UI、超时、e-ink
   纪律、与引擎崩溃恢复的交互。

净效果：**实现风险**关掉大部分，**事实风险**仍然全开 —— 但后者从「一次无底洞
式调查」变成「插上设备时照着清单逐条核对」。

## 1. 现状：电源域是空的

实测与读码确认（brain `qzos-as-system`）：

- `os/src/keymap.c` **没有** `KEY_POWER` 映射；宿主与 JS 里没有任何
  suspend / shutdown / reboot 路径
- `services.c` 只有只读的 `sys.info`
- 设备侧（brain `c1-slim-device`）**没有任何关于电源管理的记录** ——
  这是最大的未知：连「现在是谁在处理」都不知道

所以现在做的事不是「优化电源管理」，是**从零建立电源域**。零基础意味着
「照着 imagine 出来的设备写代码」的风险特别高，所以描述符层 + 场景矩阵不是
过度设计，是这个阶段唯一稳妥的写法。

## 2. 描述符层：`os/src/power.c` + `power.h`

完全照 `panel.c` 的形状 —— 静态描述符表 + 名字查询 + 环境变量覆盖 +
能力位。这是本仓已验证的「换硬件只改一处」模式，电源域照抄即可。

```c
typedef struct {
    const char *name;            /* "mp-d261-sysfs" / "sim-fake" / "none" */

    /* ---- 电源键 ---- */
    const char *key_dev;         /* 承载电源键的 evdev；NULL = 不从这里收 */
    int         key_code;        /* KEY_POWER / KEY_SLEEP，按设备实测填 */
    /* 电源键的**既有归属**。这是闸门 0 的核心未知，所以它是一个可运行时
     * 探测的字段，而不是一个编译期常量——见 §3 的「拒绝在歧义下动作」。*/
    const char *key_owner;       /* "kernel" | "vendor:<path>" | "none" | "unknown" */

    /* ---- 状态读取 ---- */
    const char *state_path;      /* /sys/class/power_supply/BAT0/status */
    const char *capacity_path;   /* .../capacity */

    /* ---- 动作 ---- */
    const char *suspend_path;    /* 写 "mem" 的 sysfs；NULL = 不支持 */
    const char *shutdown_path;   /* 写 1 的 sysfs；NULL = 不支持 */
    const char *reboot_path;

    /* ---- 能力位：决定策略层能做什么（与 panel.c 的 cap_* 同构）---- */
    bool cap_battery;
    bool cap_suspend;
    bool cap_shutdown;
    bool cap_reboot;
} qzos_power_t;
```

API 与 `panel.h` 同构：

```c
const qzos_power_t *qzos_power_default(void);   /* QZ_POWER 可覆盖 */
const qzos_power_t *qzos_power_by_name(const char *name);
const qzos_power_t *qzos_power_active(void);   /* + QZ_POWER_* 环境覆盖 */

/* 状态读；路径不存在时返回 false 而不是编一个值 */
bool qzos_power_read(const qzos_power_t *p, qzos_power_state_t *out);

/* 动作。**返回「我拒绝执行」和「我执行了」是两回事**——
 * key_owner 未知或归别人时必须拒绝并给出原因，见 §3。 */
int qzos_power_request(const qzos_power_t *p, qzos_power_action_t act);
```

**不做的**：不在这一层做策略决策（什么时候该提示关机、省电模式怎么选）。
那是 JS 侧的事（[[qzos-js-first]]：策略与组合写 JS）。这一层只做 I/O 与
「我能不能做」。

## 3. 场景矩阵：闸门 0 的三问 → 可测代码

这是本方案的核心产物。每一行都是一种**可能的**设备实况，以及 qzos 该做什么。

| 场景 | `key_owner` | 电源键实况 | qzos 应该 | qzos 不应该 |
| --- | --- | --- | --- | --- |
| **A** | `kernel` | 内核收了键，自己发 suspend 信号 | 只读状态；把 suspend 能力标为 false | 自己再处理一遍键（双重动作） |
| **B** | `vendor:/usr/bin/pmd` | 厂商守护进程在收 | 状态显示「由 pmd 管理」；关机**转发**给它 | 与它抢键、绕过它直接 `poweroff` |
| **C** | `none` | 没人管 | 全权接管：自己收键、自己 suspend | — |
| **D** | `unknown` | 查不出来 | **拒绝一切写动作**，只读状态，屏上明示 | 猜一个然后执行 |

场景 D 是最要紧的一行，也是 `key_owner` 必须是**运行时可探测**字段的原因：
**在没查清的设备上，唯一安全的动作是拒绝动作。** 一个「大概是 A 吧所以我
也来处理一下」的实现，症状是双重 suspend 或者和 pmd 打架，而这种问题在
真机上极难归因。

### 探测 `key_owner` 的手段（无真机先占位，插上设备逐条跑）

1. `grep -r KEY_POWER /sys/class/input/*/uevent` 与 `/proc/bus/input/devices`
   —— 看内核是否登记了电源键
2. `ps` 全量扫，看有没有名字像电源管理的常驻进程（`pmd`/`powerd`/`pm_manager`）
3. `ls /sys/devices/platform/*/` 找 gpio/power 相关节点
4. `strace`/`/proc/<pid>/fd` 看谁持有相关设备
5. **最直接**：按住电源键，同时 `dmesg | tail` + `ps` 快照对比

前四条写成一个 `scripts/probe-power-owner.sh`，插上设备就能跑，输出直接
喂给 `QZ_POWER_OWNER`。第五条要人。

## 4. 仿真后端：靠「指向假文件树」，不引入新机制

关键设计选择：**不为仿真发明任何机制**。qzos 访问设备本来就是通过路径，
所以仿真 = 把描述符里的路径指向临时目录里的假文件。

```
$TMP/power/BAT0/status      ← 仿真写入
$TMP/power/BAT0/capacity
$TMP/power/suspend
$TMP/power/reject           ← 仿真可写 "1" 让动作「失败」，测错误路径
```

```sh
QZ_POWER=sim-fake \
QZ_POWER_STATE=$TMP/power/BAT0/status \
QZ_POWER_SUSPEND=$TMP/power/suspend \
QZ_POWER_KEY_OWNER=none \      # 场景 C；改成 unknown 即场景 D
QZ_JS_DIR=os/js QZ_APP_DIR=os/js/apps \
  ./build-os/qzos-host
```

**电源键从已有的 FIFO 通道进来** —— `QZ_INPUT0` + `os/test/replay-keys.py`
已经在用（键盘回放就是这么做的）。所以「模拟器」不需要新的输入注入机制，
这是这个方案最省事的地方。

`replay-keys.py` 需要加一个 `power` 键名（映射到 `KEY_POWER`=116），
因为它当前的键表里没有（`os/test/replay-keys.py:27`）。

### 场景矩阵怎么跑成测试

一个 `os/test/verify-power-matrix.sh`：对 A/B/C/D 四种 `key_owner` 各跑一遍，
断言**qzos 的行为符合上表**：

- A/B：按电源键 → 屏上出现「由 kernel/pmd 管理」，且**没有**任何写动作落到
  `suspend` 文件（`test ! -f $TMP/power/suspend.written`）
- C：按电源键 → suspend 文件被写了
- D：按电源键 → 明确拒绝，suspend 文件**没**被写，且屏上明示原因

D 那条是这个矩阵的价值所在：**它测的是「不知道的时候不乱动」**，而这正是
闸门 0 想要的安全性。

## 5. 假设台账：把「未验证」变成可核对的清单

每一个还没在真机上验证的假设，编号 + 写清「怎么验」+「结论影响什么」。
插上设备时照着跑，不再是一次无底洞调查。

| # | 假设 | 影响 | 怎么验 | 状态 |
| --- | --- | --- | --- | --- |
| P1 | 电源键以 `KEY_POWER`(116) 出现在 event0 或 event1 | 键映射 | `cat /proc/bus/input/devices` + 实按 | 未验 |
| P2 | 电源键已被内核或某个进程处理 | 能不能接管 | §3 五步 | 未验 |
| P3 | 存在 `suspend` sysfs 且写 `"mem"` 生效 | 场景 A/C 的动作实现 | `ls /sys/power/`；`echo mem > state` | 未验 |
| P4 | 存在安全的 `poweroff` 路径（不是直接断电） | 关机实现 | `ls /sys/power/`；查厂商文档 | 未验 |
| P5 | 熄屏后能唤醒回 qzos（而不是重启整机） | 恢复流程 | 熄屏后按键实按 | 未验 |
| P6 | 电池/充电状态可从 `power_supply` 读到 | 状态栏 | `ls /sys/class/power_supply/` | 未验 |
| P7 | rootfs 只读时关机不会丢关键状态 | 关机前的持久化 | 需真机 | 未验 |

**P3/P4 是最危险的**：如果设备根本没有可写的 suspend/poweroff sysfs，那
「关机」就只能靠 `echo o > /proc/sysrq-trigger`（需要 sysrq 开启）或
直接断电 —— 后者等于文件系统可能损坏。这条不验清楚就写关机逻辑，是会
**制造数据损坏**的。

## 6. 实施顺序（每步都可独立验证）

1. `power.h` + `power.c` 描述符层 + `sim-fake` / `none` 两个内置后端
   —— 纯逻辑 + 假文件树，可单测，不需要设备
2. `os/test/verify-power-matrix.sh`：四场景行为闸门
3. `services.c` 加 `sys.power.state` / `sys.power.request`（**只读 + 需授权**）
   —— `power` 能力绝不能默认授予（[[qzos-app-package]]）
4. shell 侧：电源键路由 + 关机确认 UI（JS 侧，符合 [[qzos-js-first]]）
5. `scripts/probe-power-owner.sh`（真机时跑，输出喂 `QZ_POWER_OWNER`）
6. 拿到真机后：填 P1–P7，替换 `sim-fake` 为真实描述符

第 3 步的授权设计要特别小心：`sys.power.request` 是**能让设备关机**的调用。
按 `qzos-app-package` 的规则，能力 `power` 永不默认授予，且关机这种动作
最好再加一层「必须由 shell（可信）而非应用触发」的约束。

## 7. 与既有闸门的关系

- 纯逻辑层可以并入 `scripts/test-display.sh` 那种毫秒级单测
- 场景矩阵并入 `os/test/verify-power-matrix.sh`，接进 `verify-all.sh`
- 它**不替代**真机验证：`port-verification` 里「仍缺的真机验证」那节要
  相应更新，明确写「电源域已建模且有闸门，但 P1–P7 仍未验」

## 8. 这个方案明确不做的事

- 不做**整机模拟器**（模拟 MIPS、模拟 epaper 驱动、模拟内核 input 子系统）。
  已有 qemu-user + 假 sysfs 树覆盖了实际需要；再往上堆模拟层，得到的
  是「在一台假设备上通过了」的错觉
- 不猜设备的电源机制然后照着写。**没查清就是场景 D：拒绝动作**
- 不在 C 侧做电源策略（什么时候提示关机），那是 JS 的事
