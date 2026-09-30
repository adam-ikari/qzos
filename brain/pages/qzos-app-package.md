---
id: qzos-app-package
title: "qzos 应用包结构：manifest 契约 + 能力式 default-deny 授权"
category: decision
status: active
tags: [app, package, manifest, permission, security]
created: "2026-09-30T04:25:10"
updated: "2026-09-30T07:14:42"
---

<!-- compiled_truth -->
## 决策：qzos 应用包结构 = manifest 契约 + 授权（JS 遮蔽为主、C 补方法名边界）

**已实现**：`os/js/apkg.js`（校验/信任/能力）、`os/js/sandbox.js`（遮蔽）、
`os/js/shell.js` 接入（launch 前装面、坏包列但不启动、`api.require`/`onExit`/`perms`）。
闸门 82 断言（`scripts/test-apkg.sh`，跑在**真实 qzjs** 上）+ 5 项端到端
（`os/test/test_shell_apps.sh`，判据是画面像素）。设计稿 `os/docs/app-package.md`。

### 现状原本缺的五件事

1. 无 `version`、无「应用要求哪一版宿主 UI 桥」→ 宿主改 op 语义后旧应用静默出错。
2. **无任何授权**：`os/src/bridge.c` 的 `op_rpc` 拿任意 `method` 直接转发。
3. 只能单文件：应用无法 require 兄弟文件。
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
entry 不得逃逸、`api` 主版本 ≤ 宿主、perms 全在能力表、目录信任。任一条不过 →
**不装**，且在桌面以 `! <id>` 列出（静默隐藏会让人以为「装上了」，真机上没人读日志）。

### 授权：能力式、default-deny，两个执行点

`perms` 填**能力**不是方法名；能力 `X` 授予 `sys.X` 与 `sys.X.*`。
`info`/`storage`/`settings`/`net`/`power`，后两个等闸门 0（`power` 绝不默认授予）。
不在 `sys.` 下的方法应用永远调不到。

| 执行点 | 位置 | 挡什么 |
| --- | --- | --- |
| **JS** | launch 前装面 | fs / `__native__` 原生面 |
| **C** | `bridge.c` 的 `op_rpc` | 越权方法名 |

C 那道只补方法名边界，因为**应用能改写 shell 自己的 `ui.rpc`**；遮蔽必须放 JS
（`__native__` 是一次性挂上的一整对象，没有逐方法的宿主钩子）。
宿主靠 `{"op":"app","id":…,"perms":[…]}` 知道当前应用，**缺省为空**。

### 注入方案（实测校正后的定论）

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

### 剩下的洞

应用仍能读写 shell 的全局状态（同一 context）。**唯一结构性洞**。而
`__native__` 上的 `contextSpawn`/`contextDestroy` 表明**一应用一 `qzContext` 可由
shell 在 JS 层编排、不必改引擎**。遮蔽本身是合作式的：挡误用与写错的授权，不挡
蓄意攻击同上下文。

### 信任规则与当前的 fail-closed

目录 group/other 可写 → **清空 perms 而非拒绝启动**（示例与随手写的应用不该被
权限位藏起来；清空授权才是真正挡住误用的那步）。未知能力 → **拒绝启动整个应用**
而非忽略（忽略会让应用带着残缺授权在系统里跑而作者不知情）。

`dirTrusted` 的上界是 **apps-root**（有意）：本机 `/storage` 是 0777，若往上查到
`/` 则任何用户应用都永远拿不到 perms，模型在真机上全废。代价是「能整体替换
apps-root 的人」不受约束——那是「可以往设备装任意应用」的另一个问题。

**当前 `statMode` 恒为 null**：JS 侧没有 `stat`（`qzjs.fs` 与 `__native__` 都没有，
实测），所以 `dirTrusted` 一律 fail-closed → **用户应用的 perms 恒被清空**。
这是有意的：静默当作可信等于给整个授权模型开后门。补 `statMode` 原语后自动变可信，
`shell.js` 不需要改。

### API

`api` 取代全局约定：`dir` / `id` / `exit()` / `require(rel)`（归一化后须仍在
`api.dir` 内，async）/ `perms`（`Object.freeze` 的只读副本，供 UI 隐藏做不到的
按钮）/ `can(cap)` / `onExit(fn)`。旧 manifest 缺 `schema`/`id`/`version`/`api`
一律拒绝启动并显示原因；缺 `perms` 变无授权（`hello` 已补 `["info"]`）。
`back()` 顺序：**先收权再跑收尾钩子**（反过来等于给正在退出的应用多留授权窗口）。
`launch()` 失败路径也必须收权 + 清面，否则残留授权会一直生效。

### 验证纪律

断言必须落在结果上：default-deny 断言「拿到拒绝」；能力前缀同时测放行与仍被拒；
世界可写目录里声明 `perms:["info"]` 的应用**必须调不到** `sys.info`。
**遮蔽类必须连 `__native__` 一起断言**，否则「遮了门面没遮后门」会全绿。

「坏包没执行」判**画面像素**而非日志（回执由 dispatch 无条件产出，与引擎有没有
真拒绝无关——qzjs 的 interrupt 测试就这么全绿的）。用 `os/test/pbm_view.py`
把 1bpp 帧渲成 PNG（最近邻；1bpp 上任何插值都会把 1px 笔画糊掉）供目检与多模态
识别，另提供 `--region` 数区域墨量。写它时踩到极性反了（1=黑却按 1=白渲染，
整屏全黑）——墨水屏面板置位=黑。

**否定项必须配正对照**：同一个包造两份、**只改 manifest**，合法那份必须出现标记
（证明区域判据测得动），非法那份必须没有。阈值取正对照的一半而非 0——取 0 的话
一颗散点就让断言飘红，而那种红不指向任何真问题。标记区必须选在**三种画面
（桌面/详情页/正常应用）都不占用**的行（实测 y≥126），否则会撞上 back 按钮下沿
（第一次取 y=120 就撞上了，正对照 170 / 负判据 581，方向都反了）。

已做变异测试（6 个 mutant 全部会红）：漏遮 `__native__`（13 条红）、绝对路径拼到
app.dir（7 条）、同前缀路径漏洞、缺 statMode 时 fail-open、静默忽略表外能力、
把校验结果一律当通过（端到端 2 条红）。

测试自身抓出两个真 bug：`sandbox.resolveIn` 把绝对路径拼到 app.dir，导致
`/etc/passwd` 归一化成 `<dir>/etc/passwd` 判成包内（7 条断言红）；以及注释与代码
不符（声称「按 8 行条带抽样」而代码是 `y += 2`）。


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
