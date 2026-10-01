---
id: qzos-app-package
title: "qzos 应用包结构：manifest 契约 + 能力式 default-deny 授权"
category: decision
status: active
tags: [app, package, manifest, permission, security]
created: "2026-09-30T04:25:10"
updated: "2026-10-01T06:48:29"
---

<!-- compiled_truth -->
## 契约

manifest `app.json`：`schema` / `id`（必须 == 目录名）/ `name` / `version` /
`api`（主版本 ≤ 宿主）/ `entry`（不得逃逸）；可选 `icon` / `perms`（**缺省空**）。
任一条校验不过 → 被**列出来但拒绝启动**（详情页给出原因）。

## 授权：JS 可以点名一个应用，不能决定它能做什么

**判定的位置**和**判定的输入可不可信**是两个正交的轴，缺一不可：

- 判定必须落在**系统服务面**（`os/src/services.c` 的注册表 + 能力闸门）。挂在
  渲染桥的 `op_rpc` 上只挡住了那一条通道，而 JS 还能走别的路碰到 C ——
  挂错位置的检查等于没有检查。
- 能力的**唯一**合法来源是磁盘：`<QZ_JS_DIR>/apps/<id>/app.json`，由
  `os/src/appauth.c` 推导。消息里带什么都不看。

`op:app` 只转发 **id**（`{"op":"app","id":"notepad"}` / `{"op":"app","id":null}`）。
**早期设计是消息里带 `perms` 数组、宿主照单收下 —— 那是完整的提权漏洞**：
应用与 shell 共享同一个 QuickJS 全局、`ui` 是全局对象，于是任何应用都能
`ui.setApp('self', ['storage'])` 给自己授权。实测 `perms: []` 的应用真的调通了
`sys.storage.statfs`，不需要外部进程、不需要 socket。只修前一个轴的话，
`perms` 数组仍然由攻击者自己填。

声称与推导不一致时启动日志打出 `claimed N cap(s), host derived M` ——
静默忽略会让「为什么我的能力没生效」完全不可见。

**缺省为空**（没发过 / `id: null`），即一切能力方法都被拒。

`appauth.c` 三条规则：id 必须能当单级目录名用（挡 `../`、前导 `.`）；manifest 里
的 `id` 必须与目录名一致（否则把 victim 的 manifest 拷进自己目录就能继承权限）；
表外能力整条作废。读不到就是读不到，绝不猜、绝不沿用上一次的授权。

manifest 解析有**两份**实现，方向相反：`os/js/apkg.js` 用于**发现与展示**，
`os/src/appauth.c` 用于**强制**。JS 那份坏掉最多让界面显示出错的按钮；C 那份坏掉
才是安全问题。同步靠闸门，不靠约定。

## 三个执行点 + 一个残余风险

| 执行点 | 位置 | 挡什么 | 能被应用撤销吗 |
| --- | --- | --- | --- |
| **C** | `services.c` 注册表 + 能力闸门 | 越权方法名、不在注册表内的方法 | 不能（方法在 C 的表里） |
| **C** | `appauth.c` 从磁盘 manifest 推导 | 「自己给自己授权」 | 不能（输入是磁盘） |
| **JS** | `sandbox.js` launch 前装面 | fs / `__native__` 原生面 | 不能（面在应用代码加载前装好） |

JS 侧那层不能省：应用能改写 shell 自己的 `ui.rpc`，所以放在 JS 的方法名检查能被
它撤销；而 fs/native **必须**放 JS —— 那是 JS 全局对象，C 侧没有逐方法钩子。

**残余风险：身份可以被借用。** 应用不能超出**它点名的那个应用**在磁盘上声明的
能力；但 id 本身由 JS 断言，运行中的应用可以点名另一个已安装应用并借用它的能力。
JS-first + 单 QuickJS 上下文下没有便宜的修法：「已激活时拒绝改指向」挡不住（先
`setApp(null)` 再 `setApp(别人)` 即可），按 uid 无意义（单用户 root 盒），真解法是
每应用独立 qzjs-rt 进程 —— 而 `sandbox.js` 的整个形状都建立在共享上下文上。
缓解面：受信根只有 `<QZ_JS_DIR>/apps`，用户目录里的应用一律零能力，可借用的集合
仅限随系统发布的内置应用。

## 目录信任：受信根只有一个

用户可写目录里的应用**一律零能力**。机制不是「逐级 stat 检查目录可写性」，
而是**受信根只有 `<QZ_JS_DIR>/apps`**，用户目录根本不在查找范围内。后者更简单
也更难绕过：前者要判断一整条路径链，后者只需要一个根。

推论（别搞反）：JS 侧 `statMode` 原语未实现**不再是**用户应用拿不到能力的原因。
补上它不会、也不该让用户目录的应用拿到能力。

## 能力表：5 项，覆盖 3 项

| 能力 | 方法 | 状态 |
| --- | --- | --- |
| `info` | `sys.info` | 可用。**唯一 `cap=NULL`**：公开只读元信息，谁都能调 |
| `storage` | `sys.storage.statfs` | 可用（只读）。**没有读/写文件的方法** |
| `settings` | — | **未实现** |
| `net` | — | **未实现**，等闸门 0（要先在真机上查清厂商栈） |
| `power` | `sys.power.state` / `.request` | 可用；归属未确认时全拒 |

「未实现」写在 `os/test/test_services.c` 的显式清单里，不是靠记忆：加了能力忘了
实现方法，那条断言会红。目的是让能力表和实际可达的方法集不漂移。

能力 `X` 授予 `sys.X` 与 `sys.X.*`。注册表是**穷举**的而非前缀匹配 —— 前缀式
匹配曾让 `sys.` 下任何方法名都可达，而实际 handler 只有一个。

## 外部 IPC 是公开面，不是特权面

`ipc://$QZ_RPC_SOCK` 上**没有可用的调用方身份**（unix socket；设备是单用户 root
盒，uid 区分不出谁是谁），所以那里**补不了授权**，只能划清暴露面：需能力的方法
一律**不上 IPC**，socket 权限收紧到 0600。曾经把 handler 直接注册进 IPC，实测
宿主里一个应用都没跑，外部进程就调通了 `sys.storage.statfs` 并拿到挂载信息。

## 验证纪律

断言落在结果上；**否定项必须配正对照**；**遮蔽类必须连 `__native__` 一起断言**，
否则「遮了门面没遮后门」会全绿。

- 提权那条必须配正对照：磁盘上声明 `["storage"]` 的应用，即使自己声明零能力，
  仍要放行。否则「全都拒了」能让否定与肯定两条一起绿。
- 「坏包没执行」判**画面像素**而非日志（回执由 dispatch 无条件产出）。
- 授权类闸门跑在真实宿主上（`scripts/test-appauth.sh` 9 断言、
  `scripts/test-ipc-surface.sh` 8 断言），单测层（`scripts/test-services.sh`
  78 断言）只测规则。
- 测「按 perms 放行」的 fixture 必须放**内置**目录：用户目录恒零能力，放那儿
  测到的就成了「全被拒」。


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

- time: 2026-10-01T06:48:29
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: "brain update-truth：授权来源改为磁盘 manifest 之后全面重写"
  affects: [qzos-app-package]
