---
id: qzos-js-first
title: "qzos 分层：JS 优先，C99 只在必要时"
category: decision
status: active
tags: [layering, js, c99, architecture, services]
created: "2026-09-30T04:31:11"
updated: "2026-09-30T04:31:11"
---

<!-- compiled_truth -->
## 决策：JS 优先分层，C99 只在必要时

用户定的约束：**系统尽量用 JS 开发，只在必要时用 C99。** 设计稿 `os/docs/js-first.md`。
这是 [[qzos-as-system]] 的实现形态约束，与 [[qzos-app-package]] 的授权模型直接相关。

### 写 C 只有两个正当理由，且必须能说清是哪一个

1. **JS 够不到的 syscall**：没有 `statvfs`、`poll/select`、`ioctl`、signal、
   进程创建。JS 的定时器与事件循环是宿主给的，不是内核接口。（渲染同理：
   JS 没有像素概念。）
2. **必须待在应用上下文之外的东西**：授权强制点。系统（shell）与应用跑在
   **同一个 QuickJS 上下文**里，JS 侧的检查能被应用自己绕过——放在 JS 里的
   权限检查等于没放。**这是 JS-first 唯一的安全例外。**

### 别再写 C：qzjs 已经把这些给了 JS

`qzjs.fs`（readFile / readFileBinary / writeFile / exists / readdir / unlink，
**全是 async**，`readFileSync` 直接抛异常）、`qzjs.storage`、`qzjs.http2` 客户端
与服务端、`pal.timeNow/hrtime/log/randomBytes/httpRequest/portCreate`、`postMessage`。
所以「设置服务」「应用包读写」「HTTP 同步」「日志」一行 C 都不需要。

### 分层

| 层 | 语言 |
| --- | --- |
| 桌面 / 窗口 / 应用生命周期 / 启动顺序 | **JS** |
| 应用模型：发现、manifest 校验、授权策略、包管理 | **JS** |
| 系统服务的**策略与组合**（阈值、格式、告警、状态机） | **JS** |
| 系统服务的**硬件原语**（statvfs / 波形 / 电源 ioctl） | **C** |
| 授权强制点（`bridge.c` 的 `op_rpc`） | **C**（判据 2） |
| 输入（evdev `uv_poll`+read）、渲染（LVGL）、传输（qzjs 邮箱 + uvrpc） | **C** |

### 推论：服务面应当「JS 优先、C 兜底」

`op:rpc` 现在的终点是 C 的 uvrpc server。JS-first 下应反过来：**先查 JS 服务表，
命中就 JS 处理；未命中才落到 uvrpc。** C 只提供窄到无法滥用的原语。两个实利：
改服务**不用碰交叉工具链、不用重推设备**（MIPS 交叉构建慢且挑 clang 版本）；
纯 JS 服务能**脱离宿主构建单测**，复用 `test-display.sh` / `test-keymap.sh`
已验证的「毫秒级纯逻辑层」红利（133+82 断言）。

### 已知代价（不是 bug，是取舍）

- C 变少**不等于**验证变少：JS 服务照样要效果层面断言（`sys.storage` 报的自由
  字节数要对上 `statvfs` 真值，不是「RPC 回了东西」）。
- 授权强制点必须留在 C；挪到 JS 会让整套 `perms` 变成装饰。
- **`qzjs.fs.writeFile`/`unlink` 对所有应用完全开放，无路径限制**——所以
  [[qzos-app-package]] 的 `perms` **只管住服务面，管不住文件系统**。同一上下文
  里藏不住全局对象，所以这是结构性的而非「忘了加检查」。结构性解法是**一应用一
  `qzContext`** + 受限 fs 绑定。过渡期诚实说法：`perms` 是服务面授权，不是沙箱。
  这条同时是上 per-app context 的最强论据——比 `power` 关机那种具体风险更根本。
- `shell.js` 会变大（现 171 行，加应用模型/授权策略/服务注册后 400+）。届时按
  职责拆模块，沿用 [[qzos-app-package]] 的 `api.require` 加载约定，别长成泥球。


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
