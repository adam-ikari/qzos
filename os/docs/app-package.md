# qzos 应用包结构（app package format）

> 状态：设计稿，未实现。目标读者是实现者——每条规则都应能直接写成断言。
> 决策记录见 brain `qzos-app-package`；定位见 brain `qzos-as-system`。

## 为什么现在定这个

qzos 要作为**系统**替换设备上原有的程序（brain `qzos-as-system`）。一旦它是系统，
「应用」就不再是「几个自己写的示例」，而是**要被审计、被授权、被长期维护的第三方
代码**。现在的包结构只够跑 `hello` / `notepad` 两个自带示例。

## 现状与它缺什么

当前契约（`os/js/shell.js` 的 `scanDir` + `os/js/apps/*/app.json`）是：
一个目录 + 一个只有 `name`/`entry` 的 manifest。照抄现状会带进五个问题，
其中第 2 条是**安全边界缺失**：

1. **没有版本、没有 API 版本。** manifest 里没有 `version`，也没有「这个应用
   要求哪一版宿主 UI 桥」。宿主给某个 op 改了语义，旧应用会静默出错，而不是
   被拒绝启动。

2. ~~**没有任何授权。**~~ **已修（2026-09-30 ~ 10-01）。**
   原文：`op_rpc` 拿到任意 `method` 字符串直接转发，宿主不看、不问「谁在调」，
   「等 `sys.power.shutdown` 落地，**任何应用都能关机**」。

   修的过程分两步，缺一不可 —— **判定的位置**和**判定的输入可不可信**是两个
   正交的轴：

   - 判定从渲染桥搬到**服务面**（`services.c` 的注册表 + 能力闸门）。挂在
     `op_rpc` 上只挡住了那一条通道，而 JS 还能走别的路碰到 C。
   - 能力来源从**消息**换成**磁盘**（`appauth.c` 读 `<QZ_JS_DIR>/apps/<id>/app.json`）。
     只做前一步的话，`perms` 数组仍然由应用自己填 —— 实测 `perms: []` 的应用
     调一次 `ui.setApp('self', ['storage'])` 就把 `sys.storage.statfs` 调通了。

   详细形状见下面「授权有三个执行点」一节；闸门是 `scripts/test-appauth.sh`
   与 `scripts/test-ipc-surface.sh`。

3. **只能单文件。** `shell.js` 的 `launch()` 用 `new Function(src)` 把入口源码当
   字符串求值，应用没法 `require` 同目录的兄弟文件。记事本这类应用一旦超过
   ~200 行就会被迫塞成一个文件。（`api.dir` 已经传进去了，但只是让应用能自己
   `qzjs.fs.readFile`，没有加载器。）

4. **靠全局变量传生命周期。** 约定 `globalThis.App` / `globalThis.App_onExit`，
   `back()` 再 `delete` 它们。这是隐式契约，异常路径上会漏（`launch()` 的 catch
   分支就没有清 `App_onExit`）。

5. **不区分信任。** `discoverApps()` 扫 `/storage`，而 `/storage` 是 world-writable
   的。目录在、manifest 合法就上桌。c1pkg 那边已经踩过同一类问题并且踩得很准：
   **权限即信任**——从应用 id 到 version 目录，每一个目录 group/other 可写，
   launcher 就拒绝启动（brain `c1ancher-app-integration`）。qzos 现在等于对
   `/storage` 上任何目录无条件信任。

## 包结构

```text
<apps-root>/<id>/
  app.json        # manifest（必需）——唯一的契约，其余文件宿主不解释
  app.js          # 入口（必需，由 manifest 的 entry 指定）
  lib/*.js        # 可选：api.require('./lib/x.js')
  assets/*.pbm    # 可选：1bpp 图标等
```

`<apps-root>` = `QZ_APP_DIR`（默认 `/storage`）与 `QZ_JS_DIR/apps`（内置）合并扫描，
内置优先。

图标用 **P4 (PBM, 1bpp)**，不是 PNG：显示链已经是 1bpp 条带帧
（brain `qzos-ui-architecture`），`qzos_raster_encode_pbm()` 现成可用，
不必给 296×152 的屏引入任何解码器。

## manifest

```json
{
  "schema": 1,
  "id": "notepad",
  "name": "Notepad",
  "version": "1.0.0",
  "api": 1,
  "entry": "app.js",
  "icon": "assets/icon.pbm",
  "perms": ["storage"]
}
```

| 字段 | 必需 | 说明 |
| --- | --- | --- |
| `schema` | 是 | manifest 格式版本，当前 `1`。不认识就拒绝，不猜。 |
| `id` | 是 | **必须等于目录名**。不一致 = 拒绝启动（见下）。 |
| `name` | 是 | 桌面显示名。296px 宽的屏上按 12 字截断。 |
| `version` | 是 | 语义版本，仅供显示与人工排障，宿主不据此做兼容判断。 |
| `api` | 是 | 应用要求的**宿主 UI 桥主版本**。`api > 宿主支持` → 拒绝启动。 |
| `entry` | 是 | 相对 `app.json` 的入口路径。 |
| `icon` | 否 | 相对路径的 P4 图标。 |
| `perms` | 否 | 能力清单，**缺省为空**（default-deny）。 |

### 校验规则（宿主启动前逐条查，任一条不过就跳过该应用并在桌面标出来）

1. `id` == 目录名。**不接受**「目录叫 A、manifest 说自己是 B」——两处不一致时
   没有任何权威来源可依，而 `perms` 是按 id 审计的，id 漂移就等于审计对象漂移。
2. `entry` 归一化后必须落在应用目录内（禁绝对路径、禁 `..` 逃逸）。
3. `api` 的主版本 ≤ 宿主支持的主版本。
4. `perms` 每一项都在宿主的**已知能力表**里。未知能力 = 拒绝启动整个应用，
   而不是「忽略这一项」——忽略会让应用带着残缺的授权在系统里跑，而作者不知情。
5. 目录信任：应用目录及其所有祖先到 `<apps-root>` 之间，group/other **不可写**，
   应用才被授予 `perms`。世界可写的目录里的应用**照常能跑，但 `perms` 强制清空**
   （不是拒绝启动：内置示例和随手写的应用不该因为权限位就被藏起来）。

## 授权模型

`perms` 里是**能力**，不是方法名。能力 `X` 授予 `sys.X` 与 `sys.X.*` 全部方法。

| 能力 | 覆盖方法 | 状态 |
| --- | --- | --- |
| `info` | `sys.info` | 可用（只读）。**唯一 `cap=NULL` 的方法**——公开只读元信息，谁都能调 |
| `storage` | `sys.storage.statfs` | 可用（只读）。**注意：没有读/写文件的方法**，应用拿不到任何持久化 |
| `settings` | — | **未实现**。声明了没有方法：能声明、能拿到非空 perms，然后调不到东西 |
| `net` | — | **未实现**，等闸门 0（brain `c1-wifi-stack` 要先在真机上查清厂商栈） |
| `power` | `sys.power.state` / `sys.power.request` | 可用。**关机绝不能默认授予**；归属未确认时全拒 |

「未实现」这件事写在 `os/test/test_services.c` 的显式清单里，不是靠记忆：加了
能力忘了实现方法，那条断言会红。目的是让能力表和实际可达的方法集不漂移。

三条硬规则：

- **不在 `sys.` 命名空间下的方法，应用永远调不到。** 那是宿主自用/IPC 外部
  服务的面。一个应用要能调 `sys.storage`，只需要能力 `storage`。
- **注册表是穷举的，不是前缀匹配。** 见下面「授权有三个执行点」一节。
- **default-deny。** 没声明就是没有。

### 宿主怎么知道「当前是哪个应用」

shell 在 launch 前发一条 op，back 时清空：

```json
{"op":"app","id":"notepad"}     ← 只有 id
{"op":"app","id":null}          ← back 时清空
```

> ⚠️ **这条消息里没有 `perms`，而且即使带上也不会被读。**
>
> 早期设计是 `{"op":"app","id":"notepad","perms":["storage"]}`，宿主照单收下当
> 授权用。那是**完整的提权漏洞**：应用与 shell 共享同一个 QuickJS 全局、`ui` 是
> 全局对象，于是任何应用都能自己调 `ui.setApp('self', ['storage'])` 给自己授权。
> 实测（`perms: []` 的应用）：
>
> ```
> DECLARED=[]                      ← manifest 只声明零能力
> SETAPP ACCEPTED
> qzos-services: app 'escaper' authorized (1 caps)
> sys.storage.statfs *** ALLOWED ***
> ```
>
> 不需要外部进程、不需要 socket。**JS 可以点名一个应用，不能决定它能做什么。**

所以授权的**唯一**合法来源是磁盘：`<QZ_JS_DIR>/apps/<id>/app.json`，由
`os/src/appauth.c` 解析。`perms` 字段被忽略，且**会记账**——声称与推导不一致时
启动日志里打出 `claimed N cap(s), host derived M`，否则「为什么我的能力没生效」
会完全不可见。

**缺省上下文为空**（`id: null`），即没发过 `op:app` 的调用一律按无授权处理。

manifest 解析因此有**两份**实现，方向相反：`os/js/apkg.js` 那份用于**发现与展示**
（决定要不要把应用列出来、UI 上显不显示按钮），`os/src/appauth.c` 那份用于
**强制**。JS 那份被改坏时最多让界面显示出错的按钮；C 那份被改坏才是安全问题。
同步靠闸门（`scripts/test-services.sh` + `scripts/test-appauth.sh`），不靠约定。

### 授权有三个执行点，加一个独立的信任轴

| 层 | 位置 | 挡什么 | 能不能被应用撤销 |
| --- | --- | --- | --- |
| C | `services.c` 的服务注册表 + 能力闸门 | 越权方法名、不在注册表内的方法 | **不能**（方法在 C 的表里，不是消息带来的） |
| C | `appauth.c` 从磁盘 manifest 推导能力 | 「自己给自己授权」 | **不能**（输入是磁盘，不是消息） |
| JS | launch 前装面（上一节） | 文件系统、`processSpawn`、TCP 等原生面 | 不能（面在应用代码加载前就装好） |

**判定的位置**和**判定的输入可不可信**是两个正交的轴，缺一不可：

- 判定挂在渲染桥的 `op_rpc` 上时，只挡住了那一条通道——挂错位置的检查等于
  没有检查。判定必须落在服务面（JS 碰 C 的唯一通道）。
- 判定在服务面、但输入仍由 JS 提供时，等于没判——`perms` 数组是应用自己填的。
  输入必须来自磁盘。

为什么三层**不能**只放 JS：shell 与应用共享同一个 QuickJS 上下文，应用可以改写
shell 自己的 `ui.rpc`，所以放在 JS 的方法名检查能被应用自己撤销。而遮蔽面
（fs/native）**必须**放 JS——那些是 JS 的全局对象，C 侧没有对应的过滤点
（`__native__` 是一次性挂上的一整个对象，没有逐方法的宿主钩子）。

分工因此是：**C 守方法名边界与能力来源，JS 守全局对象面。** 两者都要有。

### 这条边界不是隔离：JS 注入能挡误用，挡不住蓄意攻击

所有应用跑在**同一个 qzjs-rt 进程、同一个 QuickJS 上下文**里。分两层看：

- **JS 注入能挡住**（下一节）：误用（顺手调了 `sys.power`、顺手写了包外路径）、
  以及写错的授权。检查与遮蔽都在应用代码加载**之前**装好，应用绕不过去。
- **挡不住**：同上下文的蓄意攻击——应用能读写 shell 的全局状态、能读别的应用
  在同一堆里的内存。

### 注入点：launch 之前装面，且永不还原

授权的主要执行点放在 **JS 侧、加载应用之前**（`shell.js`，纯 JS，符合
`js-first.md`）：

```js
// shell 启动时一次：
var realNative = globalThis.__native__;   // 真身只进闭包
var realFs     = qzjs.fs;                 // 读一次触发物化
globalThis.__native__ = nativeFacade(realNative);   // 白名单式
qzjs.fs           = fsFacade(realFs);               // 按 app 根 + 能力

// launch(app) 时只换 current，不重建面：
current = { dir: app.dir, perms: app.perms };
// back() 时：current = null（面**留着**）
```

三条设计要点，都是被实测/读码逼出来的，不是风格选择：

1. **必须同时遮 `__native__`，只遮 `qzjs.fs` 是演戏。** 实测
   `globalThis.__native__` 暴露 **57 个原生**，含 `fsWrite`/`fsRemove`/`fsList`/
   `fsWriteSync`、**`processSpawn`/`processTerminate`**、`contextSpawn`/`contextDestroy`、
   `tcpConnect`/`tcpListen`、`httpRequest*`、`nativeEvalScript`、`selfPath`。
   只换 `qzjs.fs` 的话，应用一句 `__native__.fsWrite(p, d)` 就过去了。

2. **面永不还原，只换 `current`。** 若 `back()` 把真身放回去，应用先前排的
   `setTimeout` 回调在还原之后触发，就会拿到真的 `qzjs.fs` —— 这就是"还原"这个
   动作本身开的洞。面常驻、只换指向哪个应用，窗口消失。

3. **不需要防 `delete`。** `lazyUnit` 首次物化时会把 accessor 整个删掉
   （`polyfill/src/lazy.js`；实测 `qzjs.fs` 初始 `acc=true`、`configurable=true`），
   所以应用 `delete qzjs.fs` 得到的是 `undefined`，**不会**重新物化出未遮蔽的真身。
   fail-closed。

补充两条已核实的边界：

- `nativeEvalScript` 用 `JS_EVAL_TYPE_GLOBAL`，在**同一个 context 的 global** 里
  求值，遮过的 global 它一样看得见，**不构成提权**。
- 真身只能待在 shell 的闭包里：JS 枚举不到闭包变量，这是唯一安全的藏法。
  `__native__` 本身是 `writable/configurable` 的普通数据属性，所以替换合法。

### 剩下的洞（诚实写下来）

- 应用仍能读写 shell 的全局状态（同一个 context）。**只有 per-app context 能解**。
- 遮蔽是**合作式**的：它挡误用与写错的授权，不挡蓄意攻击同上下文。

### 顺带发现：真隔离不必改引擎

`__native__` 上有 `contextSpawn` / `contextSuspend` / `contextResume` /
`contextDestroy`。也就是说**一应用一 QuickJS context 可以由 shell 在 JS 层编排**，
不用改 qzjs：每个应用在自己的 context 里跑，shell 用
`processPost`/`processOnMessage` 通信。这同时关掉上面唯一那条结构性洞
（同上下文互相读写），并让「应用碰不到 shell 的面」变成天然成立。

所以「`perms` 不是沙箱」不是 v1 认命，是有可达路径——但要先修 fs/`processSpawn`
这两个实测确认的口子。

## 应用 API

`launch()` 传给入口函数的 `api`，取代现有的全局变量约定：

| 成员 | 说明 |
| --- | --- |
| `api.dir` | 应用目录绝对路径 |
| `api.exit()` | 回桌面 |
| `api.require(rel)` | 加载同包内模块，见下 |
| `api.perms` | 已授予能力的只读副本（UI 据此隐藏做不到的按钮） |
| `api.onExit(fn)` | 注册收尾回调，取代 `globalThis.App_onExit` |

入口约定：定义并导出 `start(api)`。`app.js` 的顶层代码在 `start` 之前求值。

### api.require

- 相对 `api.dir` 解析，归一化后必须仍在 `api.dir` 之内（禁 `..` 逃逸、禁绝对路径）
- 每次 launch 独立缓存，back 时清空
- 返回模块的 `exports`

## 向后兼容

现有两个示例的 manifest 只有 `name`/`entry`。规则：

- `schema`/`id`/`version`/`api` 缺失 → **拒绝启动**，并把原因显示在桌面
  （这几项缺失时无法安全推断，硬填默认值正是「静默出错」的来源）
- `perms` 缺失 → 空能力集，行为改变是刻意的：以前 `ui.rpc` 无限制，
  现在必须显式声明

所以两个示例应用需要同步改 manifest（`hello` 要 `["info"]` 才能保住它的
`sys.info` 按钮）。这是**一次有意的收紧**，不是回归。

## v1 非目标

- **不做签名/信任链。** c1pkg 那套 ed25519 信任锚明确不碰（brain
  `c1ancher-app-integration`）；另起一套签名体系是独立工程。v1 的信任只到
  「目录权限」这一层。
- 不做一应用一进程 / 一应用一 QuickJS context（真隔离）。
- 不做应用间依赖、不做应用间通信。
- 不做安装事务（安装 = 拷目录；不写台账，不做回滚）。
- 不做热更新/版本并存。
- 不做 `sys.` 之外的任何应用可见方法。

## 验证方式（照既有纪律：效果层面，不是回执层面）

- **default-deny 真的生效**：应用声明 `perms: []` 调 `sys.info` 必须拿到拒绝，
  而不是「RPC 回了东西」。断言要落在**结果**上。
- **能力来源是磁盘不是消息**：一个 `perms: []` 的应用调
  `ui.setApp('self', ['storage'])` 之后调 `sys.storage.statfs`，必须**失败**。
  这条挡的就是那个提权漏洞。只测「manifest 声明的能力生效」完全测不到它——
  实测里声明 `["storage"]` 的应用和声明 `[]` 然后自己加权的应用，perms
  数组长得一模一样。**必须配正对照**：磁盘上声明了 `["storage"]` 的应用，
  即使自己声明零能力，仍然放行——否则「全都拒了」也能让两条都绿。
  闸门：`scripts/test-appauth.sh`。
- **注册表是穷举的，不是前缀匹配**：`sys.storage`（未实现）必须得到
  `no such service`，而不是「有权限就能调」。前缀式匹配曾让 `sys.` 下的
  任何方法名都可达，而实际 handler 只有一个——那是给未来留了一扇没锁的门。
- **未知能力 = 拒绝启动**，不是静默忽略。
- **用户目录的应用一律零能力**：在 world-writable 目录里放一个声明
  `perms:["info"]` 的应用，断言它调 `sys.info` **失败**。这条挡的是
  「信任检查写了但没接线」——和 c1pkg 踩的「mock 那半边形同虚设」同一类。
  注意机制不是「逐级 stat 检查目录可写性」，而是**受信根只有
  `<QZ_JS_DIR>/apps`**，用户目录根本不在查找范围内。后者更简单也更难绕过：
  前者要判断一整条路径链，后者只需要一个根。
- **外部 IPC 碰不到需能力的方法**：`tools/qzos-rpc-client` 连
  `ipc://$QZ_RPC_SOCK` 调 `sys.storage.statfs`，宿主日志里必须出现
  `Handler not found`。unix socket 上没有可用的调用方身份（设备是单用户 root
  盒，uid 区分不出谁是谁），所以那里**补不了授权**，只能划清暴露面。
  闸门：`scripts/test-ipc-surface.sh`。
- **路径逃逸**：`entry: "../other/app.js"` 与 `api.require('../../etc/passwd')`
  都被拒。
- **`id` 与目录名不符** → 拒绝启动。
- **遮蔽真的挡住了（这几条是实测确认的口子，必须钉成回归）**：
  - `typeof globalThis.__native__.fsWrite` 在应用里必须是 `undefined`
  - 应用 `delete qzjs.fs` 之后 `qzjs.fs` 仍是 `undefined`（不是重新物化出真身）
  - `__native__.processSpawn` / `tcpConnect` / `nativeEvalScript` 均不可见
  - back 之后应用遗留的 `setTimeout` 回调**拿不到**真 `qzjs.fs`
    （「面常驻」这条设计的回归）
  - **只测 `qzjs.fs` 被遮是不够的**：必须同时断言 `__native__` 那一层，
    否则「遮了门面没遮后门」会全绿

每条都要做变异测试：把对应的检查删掉，确认它会红。
