---
id: qzos-ui-architecture
title: "QZ OS 的 UI 架构：LVGL 宿主 + qzjs ISOLATED 引擎 + JSON UI 桥"
category: decision
status: active
tags: [ui lvgl architecture bridge]
created: "2026-09-29T21:19:37"
updated: "2026-09-30T02:19:56"
---

<!-- compiled_truth -->
## 决策：三层结构

OS 分三层，LVGL 与 JS 引擎不在同一进程：

```mermaid
graph TB
  subgraph JSProc["qzjs-rt 进程（ISOLATED, 已有）"]
    APP["应用 JS + Shell 逻辑"]
  end
  subgraph Host["qzos-host 进程（新增）"]
    LVGL["LVGL v9 桌面/控件"]
    DISP["显示后端: epaper / pbm"]
    IN["输入后端: evdev 键盘"]
    RT["libuv loop + qzjs 宿主侧"]
  end
  APP -->|"postMessage(JSON UI 指令)"| RT
  RT -->|"qz_post_message(JSON 事件)"| APP
  LVGL --> DISP
  IN --> LVGL
```

- **宿主持 LVGL + 设备句柄**：`/dev/epaper_lcd` 与 `/dev/input/event0/1` 都在宿主进程；LVGL v9 需要单线程 UI 上下文，正好与 ISOLATED 宿主泵 loop 的线程同线程。
- **JS 只说 JSON**：应用不直接调 `lv_*`，而是发 UI 指令（create/set/on/bind 类操作），宿主翻译成 LVGL 调用；控件事件（点击、按键、生命周期）以 JSON 回投到 JS `onmessage`。
- **选 JSON 桥而非 THREAD 直连/同步 RPC 的理由**：沿用 qzjs 现成消息管道（零线程同步）、保留双进程崩溃隔离（应用 JS 崩不掉 UI）、宿主泵 loop 单线程无重入；代价是失去 lv_* 原生 API 手感——用一层薄 JS 封装（`ui.label(...)` 风格）补偿。
- **保留 ISOLATED**：不改 qzjs 编译模型，复用已有 qemu 验证与部署路径。

## 关键落地事实

- 双向通道：宿主 `qz_post_message` → JS `globalThis.onmessage`；JS `postMessage` → 宿主 `cfg.message_cb`（跑在泵宿主 uv loop 的线程）。
- 显示帧格式：296x152 1bpp，8 行条带 `offset=(y/8)*296+x, mask=0x80>>(y%8)`，一帧 5624 字节；全刷写 `/sys/devices/platform/e0266a128/epaper/refresh`。
- 输入：evdev `event0`（matrix）+`event1`（gpio），键码表见 C1Terminal `keyboard.go`（Enter=28, 方向=103/105/106/108, Home=102, Back=158, OK=352）。
- 无触摸 → LVGL 用 keypad indev + group 焦点导航，不做 pointer。
- LVGL 用 v9.6.0（2026-09 最新），显示驱动走自定义 flush 回调，渲染在宿主内存、按需转条带帧。
- 无真机验证：显示后端可插拔，`QZ_DISPLAY=pbm` 把每帧 dump 成 PBM 供目检；qemu-user 只验输入/协议/生命周期。

## 非目标（第一版）

- 不做多窗口/多任务抢占（单全屏应用 + Shell 返回）。
- 不做 JS 直接 `lv_*` 调用（进程内直连）；不改 qzjs 进程模型。
- 不碰厂商签名仓库/c1pkg 信任链（应用以脚本目录形式放 /storage，Shell 自己发现）。


## Timeline

- time: 2026-09-29T21:19:37
  kind: decision
  summary: "Created this page: QZ OS 的 UI 架构：LVGL 宿主 + qzjs ISOLATED 引擎 + JSON UI 桥"
  source: created via brain create-page
  affects: [qzos-ui-architecture]

- time: 2026-09-29T21:20:27
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: brain update-truth
  affects: [qzos-ui-architecture]

- time: 2026-09-30T00:25:47
  kind: decision
  summary: "UI 字体：Fusion Pixel（像素缝合/位图）monospaced，lv_font_conv --bpp 1 --format lvgl --no-compress --autohint-off 生成 fusion_pixel_12（默认）/10(/8)；符号集 os/fonts/symbols.txt（4250 字：GB2312 一级汉字+ASCII+CJK 标点等，gen_symbols.py 产出）。lv_conf 走 LV_FONT_CUSTOM_INCLUDE 定义 LV_FONT_DEFAULT=&fusion_pixel_12，Montserrat 全关。拒绝一切矢量/抗锯齿字体。"
  affects: [qzos-ui-architecture]

- time: 2026-09-30T00:25:47
  kind: decision
  summary: "系统服务通讯面另立 qzos-services-rpc 页：采用 uvrpc（loop 注入 + IPC/INPROC + FlatBuffers）；本页 JSON 桥仅覆盖 JS 应用 UI，二者并存不互替。"
  affects: [qzos-ui-architecture]

- time: 2026-09-30T02:00:50
  kind: decision
  summary: "e-ink 显示管线落地：lv_timer_handler 不自刷新需显式 invalidate、label 高度兜底陷阱、1bpp 主题样式、qemu 冒烟与目检通道"
  source: "qzos-host 首次端到端验证（原生+MIPS 双平台帧一致）"
  affects: [qzos-ui-architecture]

- time: 2026-09-30T02:19:56
  kind: decision
  summary: "显示分层（不进程解耦）：raster_1bpp 纯函数 / display_policy 纯决策 / panel 描述符+设备 I/O / display 编排；局部刷新=提交决策（驱动只收整帧）；换 SoC 只改 panel.c"
  source: "显示层重构 + 133 断言单测 + 原生/MIPS 双平台验证"
  affects: [qzos-ui-architecture]
