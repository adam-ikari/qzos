---
id: qzos-app-package
title: "qzos 应用包结构：manifest 契约 + 能力式 default-deny 授权"
category: decision
status: active
tags: [app, package, manifest, permission, security]
created: "2026-09-30T04:25:10"
updated: "2026-09-30T04:25:10"
---

<!-- compiled_truth -->
## 决策：qzos 应用包结构 = manifest 契约 + 能力式授权（default-deny）

设计稿：`os/docs/app-package.md`。这是 [[qzos-as-system]] 的落地前提——一旦 qzos
是系统，「应用」就是**要被审计和授权的第三方代码**，而现状（一个目录 + 只有
`name`/`entry` 的 `app.json`）只够跑两个自带示例。

### 现状缺的五件事

1. 无 `version`、无「应用要求哪一版宿主 UI 桥」→ 宿主改 op 语义后旧应用静默出错，
   而不是被拒绝启动。
2. **无任何授权**：`os/src/bridge.c` 的 `op_rpc` 拿任意 `method` 直接转发，
   宿主不问「谁在调」。今天只有只读 `sys.info` 所以看不出来，等
   `sys.power.shutdown` 落地就是**任何应用都能关机**。
3. 只能单文件：`shell.js` 的 `new Function(src)` 让应用无法 require 兄弟文件。
4. 生命周期靠 `globalThis.App` / `App_onExit` 全局约定，`launch()` 的 catch
   分支就没清干净。
5. 不区分信任：`discoverApps()` 扫 world-writable 的 `/storage`，目录在就上桌。
   c1pkg 已经踩透同一类问题——**权限即信任**（[[c1ancher-app-integration]]）。

### 包结构

```text
<apps-root>/<id>/app.json      # 唯一契约，宿主不解释其他文件
                      app.js   # entry
                      lib/*.js # api.require
                      assets/  # P4(1bpp) 图标——显示链本来就是 1bpp，
                               # qzos_raster_encode_pbm 现成，不引解码器
```

manifest 必需字段：`schema` / `id`（**必须等于目录名**）/ `name` / `version` /
`api`（宿主 UI 桥主版本）/ `entry`；可选 `icon` / `perms`（**缺省空**）。

### 授权：能力式、default-deny、强制点在 C 侧

`perms` 填**能力**不是方法名；能力 `X` 授予 `sys.X` 与 `sys.X.*`。
`info` / `storage` / `settings` / `net` / `power`，其中 `net`、`power` 等闸门 0
（`power` 尤其**绝不能默认授予**）。不在 `sys.` 下的方法应用永远调不到。

**检查必须落在 `bridge.c` 的 `op_rpc`**（那里才看得到 `method`，JS 侧检查能被
应用自己绕过）。但 bridge 不知道当前是哪个应用，所以加一条 op：
`{"op":"app","id":…,"perms":[…]}` 由 shell 在 launch 前发、back 时清空；
**缺省上下文为空**，没发过就按无授权处理。

### 目录信任：可写则清空 perms，而不是拒绝启动

应用目录到 `<apps-root>` 之间 group/other 可写 → 应用照常能跑，但 `perms` 强制
清空。理由：内置示例和随手写的应用不该因为权限位就被藏起来；而清空授权才是
真正挡住误用的那一步。未知能力则**拒绝启动整个应用**（忽略它会让应用带着
残缺授权在系统里跑而作者不知情）。

### 这不是隔离——别当沙箱宣传

所有应用跑在**同一个 qzjs-rt 进程、同一个 QuickJS 上下文**。它挡得住误用和
「顺手写错的授权」（检查在 C 侧，绕不过去），**挡不住**同上下文的恶意行为：
应用能读写 shell 全局状态、能读别的应用的内存。真隔离要一应用一 `qzContext`。
v1 明确只做**声明 + 强制**。

### 其余要点

- 校验五条（`id`==目录名、entry 不得逃逸、`api` 主版本 ≤ 宿主、perms 全在能力表、
  目录信任），任一条不过即跳过该应用并在桌面标出原因——不猜、不硬填默认值。
- `api` 对象取代全局约定：`dir` / `exit()` / `require(rel)` / `perms`（只读副本，
  供 UI 隐藏做不到的按钮）/ `onExit(fn)`。`require` 归一化后必须仍在 `api.dir` 内。
- 旧 manifest 缺 `schema`/`id`/`version`/`api` 一律**拒绝启动**并显示原因；
  缺 `perms` 变成无授权。`hello` 需补 `["info"]`。这是**一次有意的收紧**。
- v1 非目标：签名信任链（明确不碰 c1pkg 的 ed25519 锚）、一应用一 context、
  应用间依赖与通信、安装事务/回滚、热更新。

### 验证纪律

新增断言必须落在**结果**上：default-deny 要断言「拿到拒绝」而不是「RPC 回了
东西」；能力前缀要同时测「`storage` 放行 `sys.storage`」与「`sys.settings` 仍被拒」
（只测前者会漏掉前缀写错就全放开）；世界可写目录里声明 `perms:["info"]` 的应用
**必须调不到 `sys.info`**——这条挡的正是「信任检查写了但没接线」，与 c1pkg 踩的
「mock 那半边形同虚设」同类。每条都做变异测试。


## Timeline

- time: 2026-09-30T04:25:10
  kind: decision
  summary: "Created this page: qzos 应用包结构：manifest 契约 + 能力式 default-deny 授权"
  source: "qzos-as-system 落地前提：应用成为需审计授权的第三方代码"
  affects: [qzos-app-package]

- time: 2026-09-30T04:25:10
  kind: decision
  summary: "应用包 = app.json 契约 + lib/assets；perms 填能力而非方法名，能力 X 授予 sys.X*；检查强制在 bridge.c 的 op_rpc（shell 用 op:app 告知当前应用授权，缺省为空）；目录 group/other 可写则清空 perms 而非拒绝启动；明确不是隔离（全应用同一 QuickJS 上下文）；v1 不做签名信任链"
  source: brain update-truth
  affects: [qzos-app-package]
