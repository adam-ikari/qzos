---
id: qzos-js-first
title: "qzos 分层：JS 优先，C99 只在必要时"
category: decision
status: active
tags: [layering, js, c99, architecture, services]
created: "2026-09-30T04:31:11"
updated: "2026-09-30T14:55:11"
---

<!-- compiled_truth -->
## 决策：JS 优先分层，C99 只在必要时

用户定的约束：**系统尽量用 JS 开发，只在必要时用 C99。** 设计稿 `os/docs/js-first.md`。
这是 [[qzos-as-system]] 的实现形态约束。

### 写 C 只剩两个正当理由，且必须能说清是哪一个

1. **JS 够不到的 syscall**：没有 `statvfs`、`poll/select`、`ioctl`、signal、
   进程创建。JS 的定时器与事件循环是宿主给的，不是内核接口。（渲染同理。）
2. **应用能自己撤销的那种检查**：具体指**方法名边界**（`op_rpc`）——应用能改写
   shell 自己的 `ui.rpc` 实现，所以「在 JS 里比对一次方法名」拦不住它，必须落 C。

**第 2 条的范围要说准**：它只适用于应用**持有引用**的检查。**闭包里的检查是有效
的**——JS 枚举不到闭包变量，所以「launch 前把 `qzjs.fs` 换成闭包捕获真身的过滤面」
应用绕不过去。正因如此，授权的**主要执行点在 JS**，C 那道 `op_rpc` 是补方法名边界、
不是主体。

### 别再写 C：qzjs 已经把这些给了 JS

`qzjs.fs`（readFile / readFileBinary / writeFile / exists / readdir / unlink，
**全是 async**，`readFileSync` 直接抛异常）、`qzjs.storage`、`qzjs.http2` 客户端
与服务端、`pal.timeNow/hrtime/log/randomBytes`、ports、`postMessage`。所以设置服务、
应用包读写、HTTP 同步、日志一行 C 都不需要。

### 分层

| 层 | 语言 |
| --- | --- |
| 桌面 / 窗口 / 应用生命周期 / 启动顺序 | **JS** |
| 应用模型：发现、manifest 校验、授权策略、包管理 | **JS** |
| **应用授权执行点**（fs / native 面遮蔽） | **JS** |
| 系统服务：策略与组合（阈值、格式、告警、状态机） | **JS** |
| 方法名边界（`op_rpc`） | **C** |
| 系统服务：硬件原语（statvfs / 波形 / 电源 ioctl） | **C** |
| 输入（evdev poll+read）、渲染（LVGL）、传输（qzjs 邮箱 + uvrpc） | **C** |

### 推论：服务面应当「JS 优先、C 兜底」

`op:rpc` 现在的终点是 C 的 uvrpc server。JS-first 下应反过来：**先查 JS 服务表，
命中就 JS 处理；未命中才落到 uvrpc。** C 只提供窄到无法滥用的原语。两个实利：
改服务**不碰交叉工具链、不用重推设备**（MIPS 交叉构建慢且挑 clang 版本）；
纯 JS 服务能**脱离宿主构建单测**，复用 `test-display.sh` / `test-keymap.sh`
已验证的「毫秒级纯逻辑层」红利（133+82 断言）。

### 已知代价（不是 bug，是取舍）

- C 变少**不等于**验证变少：JS 服务照样要效果层面断言（`sys.storage` 报的自由
  字节数要对上 `statvfs` 真值，不是「RPC 回了东西」）。
- 遮蔽类断言**必须连 `__native__` 一起做**。实测它暴露 57 个原生，含
  `fsWrite`/`fsRemove`/`fsWriteSync`、**`processSpawn`/`processTerminate`**、
  `tcpConnect`/`tcpListen`、`contextSpawn`/`contextDestroy`、`nativeEvalScript`、
  `selfPath`——只测 `qzjs.fs` 被遮，「遮了门面没遮后门」会全绿。细节见
  [[qzos-app-package]]。
- 剩下唯一的结构性洞是「同上下文互相读写」，而 `contextSpawn`/`contextDestroy`
  表明**一应用一 `qzContext` 可由 shell 在 JS 层编排、不必改引擎**。这是
  JS-first 少写 C 反而更干净的一个例子。
- `shell.js` 会变大（现 171 行，加应用模型/授权策略/服务注册后 400+）。届时按
  职责拆模块，沿用 `api.require` 加载约定，别长成泥球。


## Timeline

- time: 2026-09-30T04:31:11
  kind: decision
  summary: "Created this page: qzos 分层：JS 优先，C99 只在必要时"
  source: "用户定约束「系统尽可能是使用 js 开发，只在必要时使用 c99」"
  affects: [qzos-js-first]

- time: 2026-09-30T04:31:11
  kind: decision
  summary: "写 C 只有两个理由：JS 够不到的 syscall（statvfs/poll/ioctl/signal/进程）、必须待在应用上下文之外的东西（授权强制点，因 shell 与应用共享同一 QuickJS 上下文）；qzjs.fs/storage/http2 已足够覆盖文件、设置、网络、日志，一律不写 C；服务面改为 JS 优先 C 兜底（op:rpc 先查 JS 服务表，未命中才落 uvrpc）；已知 qzjs.fs 对所有应用无路径限制，perms 只管服务面不是沙箱，结构性解法是一应用一 qzContext"
  source: brain update-truth
  affects: [qzos-js-first]

- time: 2026-09-30T04:51:17
  kind: decision
  summary: "写 C 只剩两个理由：JS 够不到的 syscall、应用能自己撤销的检查（仅指方法名边界，闭包检查有效）；因此授权主要执行点在 JS，C 的 op_rpc 只是补边界；qzjs.fs/storage/http2/log 已足够，一律不写 C；服务面 JS 优先 C 兜底"
  source: brain update-truth
  affects: [qzos-js-first]

- time: 2026-09-30T04:51:17
  kind: reversal
  summary: "收窄「JS 侧检查等于没放」这条过强判断：只对应用持有引用的检查成立；闭包捕获真身的遮蔽是有效的，所以授权主体从 C 移到 JS。同时删掉「qzjs.fs 无路径限制=结构性无解」的成本条目——实为可注入遮蔽的缺口"
  source: "brain update-truth + 探针实测"
  affects: [qzos-js-first, qzos-app-package]

- time: 2026-09-30T14:55:11
  kind: reversal
  summary: "翻正「服务面 JS 优先、C 兜底」这条措辞：它暗示 C 原语层可被任意 JS 服务触达，等于在授权检查之外开后门。正确表述是——JS 优先说的是**语言选择**（策略与组合写 JS，syscall/必须待在应用上下文之外的东西写 C），而**边界形状**由 qzos-service-boundary 定：JS 到 C 只有 uvrpc 服务面一条路，注册表即 C 能力完整清单"
  source: brain append-timeline
  affects: [qzos-js-first, qzos-service-boundary]
