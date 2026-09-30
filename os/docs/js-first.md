# qzos 分层：JS 优先，C99 只在必要时

> 状态：设计稿。约束来源：用户定的方向「系统尽可能是使用 js 开发，只在必要时使用 c99」。
> 相关：[[app-package]] 的授权模型、`qzos-as-system` 的接管计划。

## 原则

系统逻辑尽量写在 JS 里。写 C 只有两个正当理由，而且**必须能说清是哪一个**：

1. **JS 够不到的 syscall**：没有 `statvfs`、没有 `poll/select`、没有 `ioctl`、
   没有 signal、没有进程创建。JS 的定时器与事件循环是宿主给的，不是内核接口。
2. **必须待在应用上下文之外的东西**：授权强制点。因为系统（shell）和应用跑在
   **同一个 QuickJS 上下文**里，JS 侧的任何检查都能被应用自己绕过——放在 JS
   里的权限检查等于没放。这是 JS-first 的**唯一安全例外**。

渲染（LVGL）归入第 1 类的延伸：JS 没有像素概念。

## qzjs 已经给了 JS 什么（所以别再写 C）

JS 侧现成就有（`polyfill/src/fs.js` 等，`globalThis.qzjs.*` / `pal.*`）：

| 能力 | 接口 |
| --- | --- |
| 文件读 | `qzjs.fs.readFile` / `readFileBinary`（ArrayBuffer） |
| 文件写/删 | `qzjs.fs.writeFile` / `unlink` |
| 目录 | `qzjs.fs.exists` / `readdir` |
| 键值存储 | `qzjs.storage.get/set/del`、`localStorage` |
| 网络 | `qzjs.http2` 客户端与服务端、`pal.httpRequest` |
| 时间/日志/随机 | `pal.timeNow` / `hrtime` / `log` / `randomBytes` |
| 消息与端口 | `postMessage`、`pal.portCreate` |

所以「设置服务」「应用包读写」「HTTP 同步」「日志」**一行 C 都不需要**。
qzjs.fs 是 async 的；`readFileSync` 直接抛异常，别指望同步文件 IO。

## 分层

| 层 | 语言 | 内容 |
| --- | --- | --- |
| 桌面、窗口、应用生命周期、启动顺序 | **JS** | `os/js/shell.js` |
| 应用模型：发现、manifest 校验、授权策略、包管理 | **JS** | 待实现（[[app-package]]） |
| 系统服务：**策略与组合** | **JS** | 阈值、格式、告警、状态机 |
| 系统服务：硬件原语 | **C** | `sys.statvfs` / 波形 ioctl / 电源 ioctl |
| **授权强制点** | **C** | `bridge.c` 的 `op_rpc`（判据 2） |
| 输入 | **C** | evdev `uv_poll` + read（JS 无 poll） |
| 渲染 | **C** | LVGL + 显示四段分层（见 `os/README.md`） |
| 传输 | **C** | qzjs 邮箱 fd + uvrpc（已是 C，不动） |

## 推论：服务面应当「JS 优先、C 兜底」

现在 `op:rpc` 的终点是 C 的 uvrpc server（`os/src/services.c`）。JS-first 下应该
**反过来**：

```text
JS: ui.rpc(method)
      │
      ▼
C bridge: op_rpc ── 授权检查（判据 2，必须留在这里）
      │
      ├─ 命中 JS 服务表 ──► JS 处理（多数情况）
      └─ 未命中          ──► uvrpc（C 的硬件服务）
```

即 C 只提供**窄到无法滥用**的原语（`sys.statvfs` 之类），服务语义全在 JS。两个
实打实的好处：

1. **改服务不用碰交叉工具链**。MIPS 交叉构建慢，工具链还挑 clang 版本；一个
   波形阈值从 C 挪到 JS，改完直接重启 rt 进程就行，不用重推设备。
2. **服务能被 headless 测**。纯 JS 服务可以脱离宿主构建单测——这正是
   `test-display.sh` / `test-keymap.sh` 已经用过的红利（133 + 82 断言、毫秒级）。

## 已知代价（写下来免得日后当成 bug）

1. **C 变少不等于验证变少。** JS 服务照样要效果层面断言：`sys.storage` 报的
   自由字节数要能对上 `statvfs` 真值，不是「RPC 回了东西」。
2. **授权强制点必须留在 C。** 这是判据 2 的直接后果，也是 JS-first 唯一的
   安全例外；把它挪到 JS 会让整套 perms 变成装饰。
3. **`qzjs.fs.writeFile` / `unlink` 现在对所有应用完全开放**，没有路径限制。
   所以 [[app-package]] 的 `perms` **只管住服务面，管不住文件系统**：应用可以
   直接改 `/storage` 下任何东西。这不是「忘了加检查」——同一上下文里没法把一个
   全局对象对应用藏起来，所以它是结构性的。
   - 结构性解法：**一应用一 `qzContext`**（qzjs 已支持），配受限的 fs 绑定。
   - 过渡期的诚实说法：`perms` 是**服务面的授权，不是沙箱**。文档、UI、命名
     都不要暗示后者。
   - 副作用：这条同时是「上 per-app context」的最强论据——它比 `power` 关机
     那种具体风险更根本。
4. **JS-first 会让 `shell.js` 变大。** shell 已经 171 行，再加应用模型、授权策略、
   服务注册会到 400+ 行。届时按职责拆模块（用 [[app-package]] 的
   `api.require` 同一套加载约定），别让 shell 变成一个大泥球。

## 怎么验

- 纯 JS 服务/授权策略：脱离宿主构建的 JS 单测（毫秒级），放
  `os/test/` 或 `os/js/` 下随包发布。
- 授权强制点：C 侧断言（那条 default-deny：应用声明 `perms: []` 调 `sys.info`
  必须拿到**拒绝**），走 `verify-input.sh` 现有的 services 往返场景。
- 硬件原语：仍需真机，不进无头闸门。
