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
| `QZ_UI_DEBUG` | — | 打印每条 UI op（`create` / `set`） |
| `QZ_LOOP_DEBUG` | — | 打印 LVGL tick 计数与 loop 状态 |

## 自测

```sh
bash scripts/test-display.sh      # 纯逻辑层单测（133 断言，无需设备/LVGL）
bash tools/rpc-ipc-selftest.sh    # uvrpc 外部 IPC 客户端调 sys.info
bash tools/qemu-selftest.sh       # qemu-mipsel 跑真机产物
```

`test-display.sh` 覆盖条带寻址、掩码移位、边界裁剪、脏区对齐和 e-ink 刷新节奏——这些是最难靠屏上目视发现的部分。

## 真机部署

```sh
scp build-os-mips/qzos-host build-os-mips/qzjs-rt device:/storage/qzos/
scp -r build-os-mips/js            device:/storage/qzos/
# 设备上：QZ_DISPLAY=epaper QZ_JS_DIR=/storage/qzos/js /storage/qzos/qzos-host
```

e-ink 约束（不可违反）：无定时动画/闪烁光标、只提交变化帧、定期全刷清残影、UI 字体只用 Fusion Pixel 位图（1bpp，无抗锯齿）。
