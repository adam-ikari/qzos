---
id: qzos-app-package
title: "qzos 应用包结构：manifest 契约 + 能力式 default-deny 授权"
category: decision
status: active
tags: [app, package, manifest, permission, security]
created: "2026-09-30T04:25:10"
updated: "2026-09-30T08:56:15"
---

<!-- compiled_truth -->
## 决策：qzos 应用包结构 = manifest 契约 + 授权（JS 遮蔽 + C 方法名边界）

**已实现**：`os/js/apkg.js`（校验/信任/能力）、`os/js/sandbox.js`（遮蔽）、
`os/js/shell.js` 接入、`os/src/bridge.c` 的 `op_app` + `app_allows`（C 侧边界）。
闸门 82 单测断言（`scripts/test-apkg.sh`，跑**真实 qzjs**）+ 9 项端到端
（`os/test/test_shell_apps.sh`）。设计稿 `os/docs/app-package.md`。

### 现状原本缺的五件事

1. 无 `version`、无「应用要求哪一版宿主 UI 桥」→ 宿主改 op 语义后旧应用静默出错。
2. **无任何授权**：`bridge.c` 的 `op_rpc` 拿任意 `method` 直接转发。
3. 只能单文件：应用无法 require 兄弟文件。
4. 生命周期靠 `globalThis.App` / `App_onExit` 全局约定，异常路径漏清。
5. 不区分信任：`discoverApps()` 扫 world-writable 的 `/storage`（c1pkg 已踩透
   同一类问题——**权限即信任**，见 [[c1ancher-app-integration]]）。

### 包结构与 manifest

```text
<apps-root>/<id>/app.json      # 唯一契约
                      app.js   # entry
                      lib/*.js # api.require
                      assets/  # P4(1bpp) 图标——显示链本就是 1bpp
```

必需：`schema` / `id`（**必须等于目录名**）/ `name` / `version` / `api`（宿主 UI 桥
主版本）/ `entry`；可选 `icon` / `perms`（**缺省空**）。五条启动前校验：id==目录名、
entry 不得逃逸、`api` 主版本 ≤ 宿主、perms 全在能力表、目录信任。任一条不过 →
**不装**，且在桌面以 `! <id>` 列出（静默隐藏会让人以为「装上了」，真机上没人读日志）。

### 授权：能力式、default-deny，两个执行点

`perms` 填**能力**不是方法名；能力 `X` 授予 `sys.X` 与 `sys.X.*`。
`info`/`storage`/`settings`/`net`/`power`，后两个等闸门 0（`power` 绝不默认授予）。
不在 `sys.` 下的方法应用永远调不到。宿主对**表外能力整条拒绝**（`op_app` 回 error），
不静默忽略——忽略会让应用带着残缺授权在系统里跑而作者不知情。

| 执行点 | 位置 | 挡什么 | 为什么在这层 |
| --- | --- | --- | --- |
| **JS** | `sandbox.js` launch 前装面 | fs / `__native__` 原生面 | 那些是 JS 全局对象，C 侧没有逐方法钩子 |
| **C** | `bridge.c` 的 `op_rpc` + `app_allows` | 越权方法名 | 应用能改写 shell 自己的 `ui.rpc`，JS 侧的方法名检查能被它撤销 |

宿主靠 `{"op":"app","id":…,"perms":[…]}` 知道当前应用，**缺省为空**（没发过就全拒）。

### 注入方案

`globalThis.__native__` 暴露 **57 个原生**（qzjs `src/context.c:189` 把
`__native_inject__` 删掉后以 `__native__` 重新挂上），含 `fsWrite`/`fsRemove`/
`fsWriteSync`、**`processSpawn`/`processTerminate`**、`contextSpawn`/`contextDestroy`、
`tcpConnect`/`tcpListen`、`nativeEvalScript`、`selfPath`。**只遮 `qzjs.fs` 是演戏。**

```js
// shell 启动一次
var realNative = globalThis.__native__;      // 真身只进闭包
var realFs     = globalThis.qzjs.fs;         // 读一次触发物化
globalThis.__native__ = nativeFacade(realNative);  // 白名单
qzjs.fs           = fsFacade(realFs);
// launch 只换 current；back 只 current=null，面永不还原
```

- **面永不还原**：`back()` 还原真身这个动作本身会开洞——应用先前排的
  `setTimeout` 回调在还原后触发就拿到真的 `qzjs.fs`。
- **不需要防 `delete`**：`lazyUnit` 物化时删 accessor（`polyfill/src/lazy.js`；
  实测 `qzjs.fs` 初始 `acc=true`），`delete` 后是 `undefined`，**不会**重新物化出
  真身。fail-closed。
- `nativeEvalScript` 是 `JS_EVAL_TYPE_GLOBAL`，同 context 求值，**不构成提权**。
- **闭包是唯一安全的藏法**（JS 枚举不到闭包变量）。这正是遮蔽有效的原因：
  「JS 检查等于没放」只对应用**持有引用**的检查成立。
- 原生面用**白名单**（default-deny），不是黑名单：黑名单漏一项就是敞开的口子，
  而漏项不会有人发现。

### 信任规则：分两路，不是一刀切

- **用户目录**（`/storage`）：`statMode` 原语还没实现（JS 侧没有 `stat`，
  `qzjs.fs` 与 `__native__` 都没有，实测）→ **fail-closed，perms 恒清空**。
  刻意的：静默当作可信等于给整个授权模型开后门。
- **内置目录**（`JS_DIR/apps`，随仓发布、只读 rootfs）：**可信**，其可信性来自
  「随仓发布」而不是目录权限位。

一刀切 fail-closed 是**错的**，已翻正：它会让内置的 hello 拿不到
`perms:["info"]`，其 `sys.info` 按钮静默失效——而 **11 项键盘端到端全绿**，
因为那条断言只看「画面变了没」，不看画面上是不是错误信息。

`dirTrusted` 的上界是 **apps-root**（有意）：本机 `/storage` 是 0777，若往上查到
`/` 则任何用户应用都永远拿不到 perms，模型在真机上全废。

### 通知类通道不能复用请求-应答通道

`op:app` 是通知，宿主处理完**不回**任何东西。早期写成 `ui.rpc('app', …)`，结果是
请求发出去后宿主找不到这个 method、**永不 settle**——`.catch()` 只处理 rejection，
不处理「永不 settle」，所以那个 `.catch(function(){})` 完全是摆设；每次
launch/back 各泄漏一个 `rpcPending` 条目。50 MiB 的设备上不能这么攒。
现在走独立的 `ui.setApp()`（`{"op":"app",…}`）。

### 剩下的洞

应用仍能读写 shell 的全局状态（同一 context）。**唯一结构性洞**。而
`__native__` 上的 `contextSpawn`/`contextDestroy` 表明**一应用一 `qzContext` 可由
shell 在 JS 层编排、不必改引擎**。遮蔽本身是合作式的：挡误用与写错的授权，不挡
蓄意攻击同上下文。

### API 与生命周期

`api` 取代全局约定：`dir` / `id` / `exit()` / `require(rel)`（归一化后须仍在
`api.dir` 内，async）/ `perms`（`Object.freeze` 只读副本，供 UI 隐藏做不到的按钮）/
`can(cap)` / `onExit(fn)`。`back()` 顺序：**先收权再跑收尾钩子**（反过来等于给
正在退出的应用多留授权窗口）；`launch()` 失败路径同样收权 + 清面。

`back()` 的「已在桌面」判据**不能**用 `ctx` 是否为空——坏包详情页正是 ctx 为空的
状态，用 ctx 判会让详情页的 back 按钮与系统 back 键全失灵，用户被卡住只能重启。
改用显式的 `s_on_detail`，并且已在桌面时按 back **不重绘**（e-ink 寿命）。

### 验证纪律

断言落在结果上：default-deny 断言「拿到拒绝」；能力前缀同时测放行与仍被拒；
**遮蔽类必须连 `__native__` 一起断言**，否则「遮了门面没遮后门」会全绿。

「坏包没执行」判**画面像素**而非日志（回执由 dispatch 无条件产出，与引擎有没有
真拒绝无关——qzjs 的 interrupt 测试就这么全绿的）。用 `os/test/pbm_view.py` 渲 PNG
供目检与多模态识别，另提供 `--region` 数区域墨量。

**否定项必须配正对照**：同一个包造两份、**只改 manifest**，合法那份必须出现标记，
非法那份必须没有。阈值取正对照的一半而非 0。标记区必须选在三种画面都不占用的行
（y≥126），否则会撞上 back 按钮下沿（第一次取 y=120 就撞上了，方向都反了）。

**测 C 侧边界的 fixture 必须放内置目录**：放用户目录会被 fail-closed 清空 perms，
测到的就成了「全被拒」而不是「按 perms 放行」。

已做 9 个变异测试（全部会红）：漏遮 `__native__`、绝对路径拼到 app.dir、同前缀
路径漏洞、缺 statMode 时 fail-open、静默忽略表外能力、校验形同虚设、详情页退不出、
拆掉 C 侧边界、内置应用也 fail-closed。

测试与实现自身抓出的真 bug：`sandbox.resolveIn` 把绝对路径拼到 `app.dir`（`/etc/passwd`
判成包内）；`scanDir` 里读未定义的 `g` 在 `'use strict'` 下抛 `ReferenceError`，
被 `listApps` 的 catch 吞成空列表（表现为「0 apps」，极其难查）。


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

- time: 2026-09-30T07:13:19
  kind: decision
  summary: "已实现：apkg.js + sandbox.js + shell 接入，82 单测断言（跑真实 qzjs）+ 5 项端到端（画面像素判据）；授权两个执行点 JS 遮蔽 + C 补方法名边界；面永不还原只换 current；原生面白名单 default-deny；statMode 恒 null 故用户应用 perms 恒清空（fail-closed）"
  source: brain update-truth
  affects: [qzos-app-package]

- time: 2026-09-30T07:13:37
  kind: evidence
  summary: "落地并闸门化：apkg.js（校验/信任/能力，纯逻辑无 qzjs 依赖）+ sandbox.js（launch 前装面）+ shell.js 接入（装面、五条校验、坏包以 ! 前缀列出不启动、api.require/onExit/perms/can、back 先收权再收尾）。scripts/test-apkg.sh 82 断言必须跑真实 qzjs（__native__ 是宿主注入的，node 上测等于没测）。os/test/test_shell_apps.sh 5 项端到端判画面像素。6 个变异测试全部会红。测试自身抓出两个真 bug：sandbox resolveIn 把绝对路径拼到 app.dir 导致 /etc/passwd 判成包内（7 条断言红）；以及我自己写的注释与代码不符（y+=2 vs 声称按 8 行条带抽样）"
  source: brain append-timeline
  affects: [qzos-app-package, qzos-js-first]

- time: 2026-09-30T07:14:05
  kind: decision
  summary: "目检通道：os/test/pbm_view.py 把 1bpp 帧渲成 PNG（最近邻放大——1bpp 上任何插值都会把 1px 笔画糊掉，字就认不出），供人眼与多模态识别；另提供 --region 数区域墨量以自动化「某块地方没有东西」这类判据。写它时踩到极性反了（1=黑却按 1=白渲染，整屏全黑）——墨水屏面板置位=黑"
  source: brain append-timeline
  affects: [qzos-app-package, port-verification]

- time: 2026-09-30T07:14:42
  kind: decision
  summary: "已实现并闸门化（82 单测 + 5 端到端）；补记目检通道 pbm_view.py 与测试自身抓出的真 bug（绝对路径拼到 app.dir）"
  source: brain update-truth
  affects: [qzos-app-package]

- time: 2026-09-30T08:55:30
  kind: reversal
  summary: "翻正「目录信任一律 fail-closed」这条：一刀切会让**内置**应用（随仓发布、只读 rootfs）也拿不到 perms，hello 的 sys.info 静默失效——而 11 项键盘端到端全绿，因为那条断言只看「画面变了没」。改为分两路：用户目录 statMode 恒 null 故 fail-closed（正确），内置目录可信性来自「随仓发布」而非目录权限位。同时补上 C 侧方法名边界（bridge.c 新增 op_app + app_allows，op_rpc 就地拒绝），此前宿主根本没有 op:app 这个 op，shell 用 ui.rpc('app') 发它会永不 settle（.catch 只处理 rejection，不处理永不 settle），每次 launch/back 泄漏一个 rpcPending"
  source: "brain append-timeline：9 项端到端闸门 + 3 个变异测试"
  affects: [qzos-app-package, qzos-js-first]

- time: 2026-09-30T08:56:15
  kind: decision
  summary: "补上 C 侧方法名边界（bridge.c op_app + app_allows）；信任分两路（用户目录 fail-closed、内置目录可信）；op:app 改走独立通知通道 ui.setApp（原 ui.rpc 会永不 settle 并泄漏 rpcPending）；back() 判据改用 s_on_detail 而非 ctx"
  source: brain update-truth
  affects: [qzos-app-package]
