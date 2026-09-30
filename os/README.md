# qzos — C1 Slim / MP-D261 e-ink OS

三层结构：`qzjs-rt`（JS 应用进程）· JSON UI 桥 · `qzos-host`（LVGL + epaper + evdev + uvrpc，单线程单 libuv loop）。

## 设计稿

| 文档 | 内容 |
| --- | --- |
| [`docs/js-first.md`](docs/js-first.md) | JS 优先分层：写 C 的两个正当理由、哪些一律不写 C、服务面「JS 优先 C 兜底」 |
| [`docs/app-package.md`](docs/app-package.md) | 应用包结构：manifest 契约、能力式 default-deny 授权、信任与校验规则 |

两条都还没实现。brain 侧对应 [[qzos-js-first]] / [[qzos-app-package]]。

## 构建

```sh
scripts/build-os.sh            # 原生（开发 / pbm 目检）
scripts/build-os.sh --mips     # 交叉：C1 Slim (MIPS32r2 LE, 静态 musl)
```

产物：`build-os*/qzos-host` + `qzjs-rt`（同目录）+ `js/`（POST_BUILD 拷贝）。

## 运行（开发）

```sh
cd build-os
QZ_DISPLAY=pbm QZ_PBM=/tmp/f.pbm QZ_DUMP_RAW=/tmp/f.pgm \
QZ_JS_DIR=js QZ_APP_DIR=js/apps QZ_AUTOEXIT_S=6 ./qzos-host
bash ../scripts/frame-preview.sh /tmp/f.pbm     # ASCII 目检
```

## 显示分层（移植到其他 SoC）

依赖单向向下，换屏/换 SoC 只动最底层：

```
LVGL 驱动    display.c 上半   display device + flush_cb
    ↓
raster_1bpp  纯函数          L8 rect → 1bpp 条带比特、脏区、差分
    ↓
display_policy 纯决策         刷不刷 / 什么波形
    ↓
panel        设备 I/O         唯一知道硬件细节的一层
```

移植时改 `os/src/panel.c` 的 `s_builtin[]` 描述符即可（几何、帧布局、设备路径、刷新机制、能力位），或不动代码用环境变量覆盖：

| 变量 | 说明 |
| --- | --- |
| `QZ_PANEL` | 选内置描述符，如 `mp-d261` |
| `QZ_PANEL_DEV` | 覆盖帧设备路径 |
| `QZ_PANEL_REFRESH` | 覆盖全刷 sysfs（设为空串 = 该屏无全刷机制） |
| `QZ_PANEL_WAVEFORM` | 覆盖波形选择 sysfs |

**局部刷新的实际含义**：MP-D261 驱动只接受整帧 `write(5624)`，没有区域参数。所以局部刷新 = **提交决策**，不是少写字节：无变化完全不刷（省 e-ink 寿命），脏区小时切快速波形。实测改一个字符 → 脏区 10.5%（一条带 + 条带对齐）、9 字节变化。字节级区域写入需要换支持区域参数的控制器，那时在 `panel.c` 加一个 variant 即可，上层不动。

## 键盘

这台设备**没有触摸**，键盘是唯一输入通路，所以键盘契约是硬约束（详见 `os/src/keymap.c` 与 brain `qzos-ui-architecture`）：

- **evdev 字母码不连续**（`KEY_Q=16`、`KEY_A=30`、`KEY_Z=44`，中间跳过修饰键）。
  绝不能用 `'a' + (code - KEY_A)` 这类算术推字符——那会让几乎每个字母静默打错
  （`KEY_H`→`'f'`、`KEY_0`→`':'`），屏幕上只表现为"键盘不好使"。映射逐项写死。
- **方向键 → 焦点移动由宿主自己做**（`input.c` 的 `adapt_arrows`）。LVGL v9 的
  keypad 只把 `LV_KEY_NEXT`/`LV_KEY_PREV` 当作"移动 group 焦点"，方向键要走
  gridnav，而本项目 `LV_USE_GRIDNAV=0`；设备键盘又没有 Tab / PageUp / PageDown，
  所以直接把方向键交给 LVGL 的后果是桌面应用列表**在真机上根本走不动**。
- **编辑态归 focus 决定**：bridge 的 `op:focus` 依目标控件是否可编辑设置
  `lv_group_set_editing()`；编辑态下方向键归光标，非编辑态归焦点移动。

## 环境变量

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `QZ_DISPLAY` | `panel` | 显示后端：`panel`（真屏）/ `pbm`（导出帧）/ `none`（空跑） |
| `QZ_PBM` | `/tmp/qzos-frame.pbm` | `pbm` 后端的帧输出路径（P4，1bpp） |
| `QZ_DUMP_RAW` | — | 额外 dump L8 灰度中间帧（P5）。排查「帧全白」时必看：能区分「LVGL 没画」与「阈值化画错」 |
| `QZ_FULL_EVERY` | `20` | 每 N 次提交做一次全刷清残影（`0` = 只在显式请求时全刷） |
| `QZ_FAST_THRESHOLD` | `0` | 脏区占比 ≤ N% 时显式要求快速波形（`0` = 禁用，交给驱动选） |
| `QZ_AUTO_COMMIT` | `1` | `0` = 只在显式 `refresh` op 时提交 |
| `QZ_DISPLAY_DEBUG` | — | 每次提交多打脏区矩形 + 变化 ASCII 图（`#` 本次变黑 / `o` 变白 / `.` 没变）。查「无缘无故多刷屏」用：只看 `changed` 字节数猜不出来源，看得见形状才知道是哪个控件在动 |

## JS 引擎崩溃恢复

`qzjs-rt`（跑 JS 的独立进程）挂掉时，qzjs 会往邮箱推一帧
`{"type":"error","error":"main-runtime-process-exited-unexpectedly"}`。
`bridge.c` 认这帧 → `qzos_host_on_rt_death()` 排一次带退避的 rt 重建：

1. **告知** —— 屏上画 `JS engine stopped` + 倒计时（正中 292×80 框，全刷）
2. **停输入** —— `qzos_input_enable(0)` 停 poll 并清空事件队列。引擎死后按键
   无人消费而画面停在旧桌面上，继续响应只会骗人
3. **重建** —— `qz_destroy` + `qz_create` + 重挂 `qz_message_fd` + 重跑 boot。
   M-P7 契约下宿主不持有 rt 内部状态，销毁即干净
4. **恢复** —— 开输入、撤提示、**退避复位**

退避 1s→2s→4s…封顶 30s。用退避而非固定间隔，是因为最危险的场景正是「一起
就崩」——固定 1s 会变成无限重启循环，把墨水屏刷满、电池耗光，而这些刷新一点
用都没有。

**开机时 rt 起不来也不退出**：设备是墨水屏一体机，宿主一退就是黑屏，用户只能
等电池耗尽或物理断电。留在退避循环里，qzjs-rt 一旦就位就自动起来。

## 电源域

`os/src/power.c` 是描述符层，形状照 `panel.c`（换设备只改一处）。设计稿
[`docs/power-sim.md`](docs/power-sim.md)。

**核心不变量：`key_owner == UNKNOWN ⇒ 一切写动作被拒。** 挡的是「双重处理」
—— 若内核或厂商 pmd 已在收电源键而 qzos 也去 suspend 一次，症状是设备在两层
逻辑间来回跳，真机上极难归因，因为两边都「看起来在工作」。

当前所有设备事实**均未在真机验证**（台账 P1–P7，见 brain `qzos-power-sim`）。
所以 `mp-d261-unverified` 描述符把 `cap_suspend`/`cap_shutdown` 都设成 false：
若设备没有可写的 poweroff sysfs，「关机」只能靠 sysrq 或直接断电，可能损坏
文件系统 —— 而这台设备熄屏后只能靠 USB ADB 救。宁可不做。

插上设备后先跑探测（只读，不改设备）：

```sh
ADB="adb -s <serial>" scripts/probe-power-owner.sh
```

## 系统服务面：JS 碰 C 的唯一通道

`os/src/services.c` 的注册表 `s_registered[]` 就是 **C 能力的完整清单**。
JS（shell 与应用）碰到宿主 C 代码只有这一条路；UI 桥（`bridge.c`）是纯渲染命令
通道，不承载能力。授权检查落在服务面侧，不在渲染桥——挂在渲染通道上只挡住了
那一条，而 JS 还能走别的路。

```c
{ "sys.info",              NULL,      sys_info_handler },
{ "sys.storage.statfs",    "storage", sys_storage_statfs_handler },
```

两条规则：

- **方法必须落在自己能力的命名空间下**（`sys.storage.*` 需要 `storage`）。启动时
  `verify_registry()` 查一次，测试里再查一次——cap 配错方法名会让授权形同虚设。
- **能力是注册表显式声明的**，不靠「以 `sys.` 开头就算」的前缀匹配。前缀式匹配
  等于「`sys.` 下任何方法名都可达」，而实际 handler 只有一个——那是给未来留了
  一扇没锁的门。

### 两条路径，不是一条

服务面有**两个入口**，这一点必须显式面对——早期只守了一个，于是 IPC 成了后门：

| 路径 | 谁能到 | 暴露哪些方法 | 授权 |
| --- | --- | --- | --- |
| INPROC（`inproc://qzos`） | JS（shell + 应用） | 全部 | 应用 `perms`，default-deny |
| IPC（`ipc://$QZ_RPC_SOCK`） | 设备上任意进程 | **只有 `cap == NULL` 的** | 无从授权 |

IPC 那边**补不了授权**：unix socket 没有可用的调用方身份，而设备是单用户 root 盒
（uid 区分不出谁是谁）。所以诚实的做法是划清暴露面——**需能力的方法一律不上 IPC**。
曾经把 handler 直接注册进 IPC，实测：宿主里一个应用都没跑，一个外部进程连上
`/storage` 上的 socket 就调通了 `sys.storage.statfs`。设备上 `/storage` 是 0777，
socket 又被 uvrpc 建成 0755，等于「任何应用都能读你的存储布局」。现在扣下需能力的
方法，并把 socket 收紧到 0600。

### uvrpc 的错误回执在客户端不可辨（第三方限制）

`third_party/uvrpc` 的 client **恒**填 `status = UVRPC_OK`、`error_code = 0`
（`uvrpc_client.c:158`），而 server 对「handler 不存在」是把 `int32` 错误码塞进
`result` 的头 4 字节 + 消息串（`uvrpc_server.c:207`）。线上**没有标签**能让客户端
分辨成功结果和错误负载。

后果：任何 uvrpc 客户端都会把失败看成成功。曾按「头 4 字节非零即错误」在
`tools/qzos-rpc-client.c` 里解过，结果 `sys.info` 的 `{"se` 被读成错误码
1702044283——**猜比不猜更糟**，已回退。判据改取**宿主日志**里的
`Handler not found`（服务端权威记录，见 `scripts/test-ipc-surface.sh`）。

本仓的免疫方式：派发前自己 `svc_find()`，且注册与派发同源于 `s_registered[]`，
所以 handler 缺失不可达。注册失败仍可能发生（`uvrpc_server_register` 返回非 OK），
此时由 `s_bound[]` 把静默的假成功换成明确的 `service not bound`——不能派发进空洞。

## 授权面（app package）

`os/js/apkg.js` 做 manifest 校验与信任判定，`os/js/sandbox.js` 在**加载任何应用
之前**装好遮蔽面。设计见 [`docs/app-package.md`](docs/app-package.md)。

- `perms` 填**能力**不是方法名；能力 `X` 授予 `sys.X` 与 `sys.X.*`，缺省为空
  （default-deny）。能力表：`info` / `storage` / `settings` / `net` / `power`
  （后两个等闸门 0，`power` 绝不默认授予）
- 遮蔽必须**连 `globalThis.__native__` 一起做**——它实测暴露 57 个原生，含
  `fsWrite`、`processSpawn`、`tcpConnect`、`contextSpawn`。只换 `qzjs.fs`
  等于门遮了后门没遮
- 面**永不还原**，只换指向哪个应用；否则 `back()` 还原真身这个动作本身会开洞
- 目录信任目前**一律 fail-closed**：`statMode` 原语还没实现（JS 侧没有 `stat`），
  所以用户应用的 `perms` 恒被清空

## 帧的目检通道

`/dev/epaper_lcd` 只写不可读，画面只能靠帧文件判断。`os/test/pbm_view.py` 把 1bpp
帧渲染成 PNG（最近邻放大——1bpp 上任何插值都会把 1px 笔画糊成灰边，字就认不出了）：

```sh
python3 os/test/pbm_view.py /tmp/qzos-input/boot.pbm /tmp/boot.png --scale 3
# 区域墨量：判「某块地方没有东西」（--max 缺省 -1 = 只测量不判定）
python3 os/test/pbm_view.py --region f.pbm --at 4,126,288,24 --max 100
```

`os/js/apkg.js` 做 manifest 校验与信任判定，`os/js/sandbox.js` 在**加载任何应用
之前**装好遮蔽面。设计见 [`docs/app-package.md`](docs/app-package.md)。

- `perms` 填**能力**不是方法名；能力 `X` 授予 `sys.X` 与 `sys.X.*`，缺省为空
  （default-deny）。表：`info` / `storage` / `settings` / `net` / `power`
  （后两个等闸门 0，`power` 绝不默认授予）
- 遮蔽必须**连 `globalThis.__native__` 一起做**——它实测暴露 57 个原生，含
  `fsWrite`、`processSpawn`、`tcpConnect`、`contextSpawn`。只换 `qzjs.fs` 是
  门遮了后门没遮
- 面**永不还原**，只换指向哪个应用；否则 `back()` 还原真身这个动作本身会开洞
- 目录信任目前**一律 fail-closed**：`statMode` 原语还没实现（JS 侧没有 `stat`），
  所以用户应用的 `perms` 恒被清空

## 自测

### 环境变量

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `QZ_JS_DIR` | `js` | shell bundle 目录（ui.js / shell.js / apkg.js / sandbox.js / apps） |
| `QZ_APP_DIR` | `/storage` | 用户应用目录（与 `QZ_JS_DIR/apps` 合并扫描，`app.json` 为 manifest） |
| `QZ_RPC_SOCK` | `/storage/qzos/rpc.sock` | uvrpc 外部 IPC 监听路径（`none` 关闭） |
| `QZ_RT_SERVER` | 同目录 `qzjs-rt` | JS 运行时可执行文件（qemu 下需指向包装脚本） |
| `QZ_INPUT0` / `QZ_INPUT1` | `/dev/input/event0` / `event1` | 键盘 evdev（打不开则跳过） |
| `QZ_AUTOEXIT_S` | — | 跑 N 秒后自行退出。自动化冒烟用，**不要**改用外部 `timeout`（会给进程组发 SIGTERM，误报成 JS 崩溃） |
| `QZ_INPUT_DEBUG` | — | 逐跳打印 evdev 进队 / 出队到 LVGL 的键。键是唯一没有硬件反馈的输入，链路上任一环静默丢键都表现为「按了没反应」，没有这行只能靠猜 |
| `QZ_UI_DEBUG` | — | 打印每条 UI op（`create` / `set`） |
| `QZ_LOOP_DEBUG` | — | 打印 LVGL tick 计数与 loop 状态 |

## 验证

统一入口（从纯逻辑到端到端分层跑）：

```sh
bash scripts/verify-all.sh
```

单条通道：

| 脚本 | 覆盖 | 需要宿主构建 / 设备 |
| --- | --- | --- |
| `scripts/test-display.sh` | 显示纯逻辑：条带寻址、掩码移位、边界裁剪、脏区对齐、刷新决策（133 断言） | 否 / 否 |
| `scripts/test-keymap.sh` | evdev 键码映射（82 断言，键码常量取自 `<linux/input.h>`） | 否 / 否 |
| `scripts/test-power.sh` | 电源域决策层（43 断言）。场景矩阵，重点是**归属未知时全拒** | 否 / 否 |
| `scripts/test-services.sh` | 系统服务注册表（38 断言）。**JS→C 唯一边界的可验证形态**：注册表性质、default-deny、IPC 公开面规则、未绑定时报错 | 部分 |
| `scripts/test-ipc-surface.sh` | 服务面**外部 IPC 半边**（8 断言）：外部进程调需能力的方法时 handler 从未被调用 + **正对照**（`sys.info` 必须调得通，否则「拒绝」可能只是客户端坏了）+ socket 权限 | 是 / 否 |
| `scripts/test-apkg.sh` | 应用包校验 + 授权遮蔽（82 断言）。**跑在真实 qzjs 上**：核心断言是「`__native__` 那 57 个原生真被遮住了」，在 node 上跑等于什么都没测 | 原生 qzjs / 否 |
| `os/test/test_shell_apps.sh` | 应用模型端到端：坏 manifest 被列出来但拒绝启动，**判据是画面像素** | 是 / 否 |
| `os/test/verify-rt-recovery.sh` | JS 引擎崩溃恢复：杀 `qzjs-rt`，断言屏上出现提示、rt 被重建、桌面逐字节复现、开机失败也留在退避循环 | 是 / 否 |
| `scripts/verify-input.sh` | 键盘端到端（`QZ_INPUT0` 接 FIFO 回放）+ e-ink 刷新预算 | 是 / 否 |
| `scripts/verify-frames.sh` | 原生 vs MIPS 帧逐字节一致（MIPS 经 qemu-user） | 是 / 否 |
| `scripts/verify-mips-e2e.sh` | MIPS 端到端：桌面/键盘/应用发现/授权遮蔽在 qemu-user 下逐条验 | 是 / 否 |
| `tools/rpc-ipc-selftest.sh` | 手工探测：外部 IPC 客户端调 `sys.info`。**闸门是上面的 `test-ipc-surface.sh`** | 是 / 否 |

前三项是毫秒级的纯逻辑单测——上层现象不对时先确认它们是绿的，否则容易在上层猜错方向。

**跑闸门要看退出码，不要只看断言行。** 曾经 `verify-frames.sh` 里
`frame-preview.sh | head -3` 让 preview 收到 SIGPIPE 返回 141，`set -euo pipefail`
让整个脚本以 141 退出——**每一条断言都打印了 PASS，但 `verify-all.sh` 的
「ALL PASS」从来没出现过**，而且它后面挂的步骤根本不会跑。只 grep 断言行会把
这种失败看成全绿。已改用 `sed -n '1,3p'`（读完输入、不产生 SIGPIPE）。

**这些通道替代不了真机**：`/dev/epaper_lcd` 只写不可读，落屏波形、残影、真实 evdev 行为、刷新耗时与视觉本身，只能人眼在设备上看（另见 brain `port-verification`）。

断言写成「语义」而不是黄金文件：换字体、改布局不该让测试红，而「方向键没移动焦点」这类真 bug 一定会被抓到。每条新断言都应做**变异测试**（把 bug 塞回去确认它会红），否则无法区分闸门和装饰。

**判据要落在效果上，不是回执上。** 「日志里没有 launch 失败」是回执层断言——回执由 dispatch 路径无条件产出，与引擎有没有真拒绝无关（qzjs 的 interrupt 测试就是这么全绿的）。所以「坏包没执行」判的是**画面那一行有没有墨**（`pbm_view.py --region`）。

**否定项必须配正对照。** 只测「坏包没出现」的话，一个「把所有包都拒掉」的实现也能全绿。所以 `test_shell_apps.sh` 里同一个包造两份、**只改 manifest**：合法那份必须出现标记（证明区域判据测得动），非法那份必须没有。阈值取正对照的一半而不是 0，否则一颗散点就让断言飘红而那种红不指向任何真问题。

## 真机部署

```sh
scp build-os-mips/qzos-host build-os-mips/qzjs-rt device:/storage/qzos/
scp -r build-os-mips/js            device:/storage/qzos/
# 设备上：QZ_DISPLAY=epaper QZ_JS_DIR=/storage/qzos/js /storage/qzos/qzos-host
```

e-ink 约束（不可违反，违反任一条都是产品缺陷而非优化项）：

- **无定时动画、无闪烁光标**——动画 = 每帧一次波形刷新。已踩：textarea 光标
  400ms 闪烁（每敲一键刷 4~5 次）、默认主题按钮 120ms style transition
  （每次点击多刷 3~4 次）。因此 `op_create` 对每个控件 `lv_obj_remove_style_all()`
  后自己定死视觉，不继承为背光屏设计的主题样式。
- **一次按键最多刷一次屏**——`verify-input.sh` 的刷新预算按 `1 + 按键数` 断言，
  上下限都卡：上限挡动画回归，下限挡「测量失效」（例如日志格式一改导致 grep
  数出 0 次刷新，上限断言会让 0 永远通过）。
- **只提交变化帧**——LVGL 报脏不等于像素真的变了，`changed == 0` 必须一路传到
  策略层，否则每次「重绘了但内容一样」都白刷一次。
- **定期全刷清残影**（`QZ_FULL_EVERY`）。
- **UI 字体只用 Fusion Pixel 位图**（1bpp，无抗锯齿）。
