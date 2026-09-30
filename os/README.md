# qzos — C1 Slim / MP-D261 e-ink OS

三层结构：`qzjs-rt`（JS 应用进程）· JSON UI 桥 · `qzos-host`（LVGL + epaper + evdev + uvrpc，单线程单 libuv loop）。

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
| `QZ_JS_DIR` | `js` | shell bundle 目录（ui.js / shell.js / apps） |
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
| `scripts/verify-input.sh` | 键盘端到端（`QZ_INPUT0` 接 FIFO 回放）+ e-ink 刷新预算 | 是 / 否 |
| `scripts/verify-frames.sh` | 原生 vs MIPS 帧逐字节一致（MIPS 经 qemu-user） | 是 / 否 |
| `tools/rpc-ipc-selftest.sh` | uvrpc 外部 IPC 客户端调 `sys.info` | 是 / 否 |

前两项是毫秒级的纯逻辑单测——上层现象不对时先确认它们是绿的，否则容易在上层猜错方向。

**这些通道替代不了真机**：`/dev/epaper_lcd` 只写不可读，落屏波形、残影、真实 evdev 行为、刷新耗时与视觉本身，只能人眼在设备上看（另见 brain `port-verification`）。

断言写成「语义」而不是黄金文件：换字体、改布局不该让测试红，而「方向键没移动焦点」这类真 bug 一定会被抓到。每条新断言都应做**变异测试**（把 bug 塞回去确认它会红），否则无法区分闸门和装饰。

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
