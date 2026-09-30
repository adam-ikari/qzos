---
id: qzos-app-package
title: "qzos 应用包结构：manifest 契约 + 能力式 default-deny 授权"
category: decision
status: active
tags: [app, package, manifest, permission, security]
created: "2026-09-30T04:25:10"
updated: "2026-09-30T04:50:39"
---

<!-- compiled_truth -->
## 决策：qzos 应用包结构 = manifest 契约 + 授权（JS 遮蔽为主、C 补方法名边界）

设计稿：`os/docs/app-package.md`。这是 [[qzos-as-system]] 的落地前提——一旦 qzos
是系统，「应用」就是**要被审计和授权的第三方代码**。

### 现状缺的五件事

1. 无 `version`、无「应用要求哪一版宿主 UI 桥」→ 宿主改 op 语义后旧应用静默出错。
2. **无任何授权**：`os/src/bridge.c` 的 `op_rpc` 拿任意 `method` 直接转发。
3. 只能单文件：`shell.js` 的 `new Function(src)` 让应用无法 require 兄弟文件。
4. 生命周期靠 `globalThis.App` / `App_onExit` 全局约定，异常路径漏清。
5. 不区分信任：`discoverApps()` 扫 world-writable 的 `/storage`。c1pkg 已踩透
   同一类问题——**权限即信任**（[[c1ancher-app-integration]]）。

### 包结构与 manifest

```text
<apps-root>/<id>/app.json      # 唯一契约
                      app.js   # entry
                      lib/*.js # api.require
                      assets/  # P4(1bpp) 图标——显示链本就是 1bpp
```

必需：`schema` / `id`（**必须等于目录名**）/ `name` / `version` / `api`（宿主 UI 桥
主版本）/ `entry`；可选 `icon` / `perms`（**缺省空**）。五条启动前校验：id==目录名、
entry 不得逃逸、`api` 主版本 ≤ 宿主、perms 全在能力表、目录信任。

### 授权：能力式、default-deny，两个执行点

`perms` 填**能力**不是方法名；能力 `X` 授予 `sys.X` 与 `sys.X.*`。
`info`/`storage`/`settings`/`net`/`power`，后两个等闸门 0（`power` 绝不默认授予）。
不在 `sys.` 下的方法应用永远调不到。

| 执行点 | 位置 | 挡什么 |
| --- | --- | --- |
| **C** | `bridge.c` 的 `op_rpc` | 越权方法名 |
| **JS** | launch 前装面 | fs / `__native__` 原生面 |

C 那道只补方法名边界，因为**应用能改写 shell 自己的 `ui.rpc`**；而遮蔽必须放 JS
（`__native__` 是一次性挂上的一整对象，没有逐方法的宿主钩子）。

宿主靠新 op 知道当前应用：`{"op":"app","id":…,"perms":[…]}`，back 时发
`{"op":"app","id":null,"perms":[]}`。**缺省为空**。

### 注入方案（实测校正后的定论）

`globalThis.__native__` 暴露 **57 个原生**（qzjs `src/context.c:189` 把
`__native_inject__` 删掉后以 `__native__` 重新挂上），含 `fsWrite`/`fsRemove`/
`fsWriteSync`、**`processSpawn`/`processTerminate`**、`contextSpawn`/`contextDestroy`、
`tcpConnect`/`tcpListen`、`nativeEvalScript`、`selfPath`。**只遮 `qzjs.fs` 是演戏。**

```js
// shell 启动一次
var realNative = globalThis.__native__;      // 真身只进闭包
var realFs     = qzjs.fs;                    // 读一次触发物化
globalThis.__native__ = nativeFacade(realNative);
qzjs.fs           = fsFacade(realFs);
// launch 只换 current；back 只把 current 置 null，面永不还原
```

- **面永不还原**：`back()` 还原真身这个动作本身会开洞——应用先前排的 `setTimeout`
  回调在还原后触发就拿到真的 `qzjs.fs`。
- **不需要防 `delete`**：`lazyUnit` 物化时会删掉 accessor（`polyfill/src/lazy.js`；
  实测 `qzjs.fs` 初始 `acc=true`），所以 `delete` 后是 `undefined`，**不会**重新
  物化出真身。fail-closed。
- `nativeEvalScript` 是 `JS_EVAL_TYPE_GLOBAL`，同 context 求值，**不构成提权**。
- **闭包是唯一安全的藏法**（JS 枚举不到闭包变量）。这正是遮蔽有效的原因：
  应用够不到真身。

### 剩下的洞

应用仍能读写 shell 的全局状态（同一 context）。**唯一结构性洞**。而
`__native__` 上的 `contextSpawn`/`contextDestroy` 表明**一应用一 `qzContext` 可由
shell 在 JS 层编排、不必改引擎**，顺带关掉这条。遮蔽本身是合作式的：挡误用与写错
的授权，不挡蓄意攻击同上下文。

### 信任规则

目录 group/other 可写 → **清空 perms 而非拒绝启动**（示例与随手写的应用不该被
权限位藏起来；清空授权才是真正挡住误用的那步）。未知能力 → **拒绝启动整个应用**
而非忽略（忽略会让应用带着残缺授权在系统里跑而作者不知情）。

### API 与兼容

`api` 取代全局约定：`dir` / `exit()` / `require(rel)`（归一化后须仍在 `api.dir` 内）/
`perms`（只读副本，供 UI 隐藏做不到的按钮）/ `onExit(fn)`。旧 manifest 缺
`schema`/`id`/`version`/`api` 一律拒绝启动并显示原因；缺 `perms` 变无授权
（`hello` 需补 `["info"]`）。这是**有意的收紧**。v1 非目标：签名信任链（不碰
c1pkg 的 ed25519 锚）、一应用一 context、应用间依赖与通信、安装事务、热更新。

### 验证纪律

断言必须落在结果上：default-deny 要断言「拿到拒绝」；能力前缀要同时测放行与
仍被拒；世界可写目录里声明 `perms:["info"]` 的应用**必须调不到** `sys.info`。
**遮蔽类必须连 `__native__` 一起断言**（`typeof __native__.fsWrite === 'undefined'`、
`delete qzjs.fs` 后仍 `undefined`、`processSpawn`/`tcpConnect` 不可见、back 后
遗留定时器拿不到真身），否则「遮了门面没遮后门」会全绿。每条做变异测试。


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

- time: 2026-09-30T04:49:31
  kind: reversal
  summary: "翻正「perms 管不住文件系统」这条结论：不是结构性的。实测 globalThis.__native__ 暴露 57 个原生（qzjs/src/context.c 把 __native_inject__ 删掉后又以 __native__ 重新挂上），含 fsWrite/fsRemove/fsWriteSync、processSpawn/processTerminate、contextSpawn/contextDestroy、tcpConnect/tcpListen、nativeEvalScript、selfPath——只遮 qzjs.fs 是演戏。正确做法是用户提的方案：launch 前在 JS 侧注入遮蔽，且必须同时遮 __native__；面永不还原只换 current（否则 back() 后应用遗留定时器回调拿到真身）；delete 是 fail-closed（lazyUnit 物化时删 accessor，不会重新物化真身）；nativeEvalScript 是 JS_EVAL_TYPE_GLOBAL 不构成提权；真身只能藏闭包。授权主要执行点在 JS，C 的 op_rpc 只补方法名边界（应用能改写 shell 的 ui.rpc）。并发现 __native__ 上的 contextSpawn/contextDestroy 意味着一应用一 qzContext 可由 shell 在 JS 层编排，不必改引擎。"
  source: "brain append-timeline：实测 build-os/qzjs/qzjs 探针 + 读 qzjs/src/context.c:189、polyfill/src/lazy.js、src/bridge.c:2362+"
  affects: [qzos-app-package, qzos-js-first]

- time: 2026-09-30T04:50:39
  kind: decision
  summary: "授权两个执行点：JS 在 launch 前装遮蔽面（必须同时遮 __native__，它实测暴露 57 个原生含 processSpawn；面永不还原只换 current；delete 是 fail-closed；nativeEvalScript 不提权；真身藏闭包），C 的 op_rpc 只补方法名边界；一应用一 qzContext 可由 shell 用 contextSpawn 编排不必改引擎"
  source: brain update-truth
  affects: [qzos-app-package]
