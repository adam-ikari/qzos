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

2. **没有任何授权。** `os/src/bridge.c` 的 `op_rpc` 拿到任意 `method` 字符串直接
   转发给 `qzos_services_rpc`，宿主不看、不问「谁在调」。今天唯一的 builtin 服务
   是只读的 `sys.info`，所以看不出问题；等 `sys.power.shutdown` 落地，**任何应用
   都能关机**。这是「把厂商守护进程的能力收进服务面」这件事必须先补的地基。

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

| 能力 | 覆盖方法 | v1 |
| --- | --- | --- |
| `info` | `sys.info` | 可用（只读） |
| `storage` | `sys.storage*` | 规划中（只读） |
| `settings` | `sys.settings*` | 规划中（只读） |
| `net` | `sys.net*` | 等闸门 0（brain `c1-wifi-stack`） |
| `power` | `sys.power*` | 等闸门 0——**关机绝不能默认授予** |

两条硬规则：

- **不在 `sys.` 命名空间下的方法，应用永远调不到。** 那是宿主自用/IPC 外部
  服务的面。一个应用要能调 `sys.storage`，只需要能力 `storage`。
- **default-deny。** 没声明就是没有。

### 强制点：必须落在 C 侧，不能落在 JS 侧

检查点是 `bridge.c` 的 `op_rpc`，因为那里才看得到 `method` 字符串，而 JS 侧的
任何检查都能被应用自己绕过。

但 bridge 不知道「当前是哪个应用」——那是 shell 的事。所以需要一条新的 op：

```json
{"op":"app","id":"notepad","perms":["storage"]}   // shell 在 launch 前发
{"op":"app","id":null,"perms":[]}                 // back 时清空
```

宿主把它存成「当前应用上下文」，`op_rpc` 逐次比对。**缺省上下文为空**，
即没有 `op:app` 的调用一律按无授权处理。

### 这条边界不是隔离（必须写清楚，否则会被当成安全沙箱）

所有应用跑在**同一个 qzjs-rt 进程、同一个 QuickJS 上下文**里（brain
`qzos-ui-architecture` 的三层结构）。所以：

- 它能挡住**误用**（顺手调了 `sys.power`）和**顺手写错的授权**（可审计的声明），
  检查在 C 侧，应用绕不过去；
- 它**挡不住**同上下文的恶意行为——应用能读写 shell 的全局状态、能读别的应用的
  内存。真隔离要一应用一 context（qzjs 有 `qzContext`），那是后面的事。

最尖锐的一例是**文件系统完全不受这套机制管**：`qzjs.fs.writeFile` / `unlink`
是对所有应用开放的全局能力，没有路径限制，应用可以直接改 `/storage` 下任何东西。
同一上下文里没法把一个全局对象藏起来，所以这不是「忘了加检查」，而是结构性的
（见 `os/docs/js-first.md` 的「已知代价」）。

v1 明确不做隔离，只做**声明 + 强制**。别把它当沙箱宣传。

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
- **能力前缀规则**：给 `storage` 之后 `sys.storage` 通、而 `sys.settings` 仍被拒。
  只测前者会漏掉「前缀写错就全放开」。
- **未知能力 = 拒绝启动**，不是静默忽略。
- **目录可写 → perms 被清空**：在 world-writable 目录里放一个声明
  `perms:["info"]` 的应用，断言它调 `sys.info` **失败**。这条挡的是
  「信任检查写了但没接线」——和 c1pkg 踩的「mock 那半边形同虚设」同一类。
- **路径逃逸**：`entry: "../other/app.js"` 与 `api.require('../../etc/passwd')`
  都被拒。
- **`id` 与目录名不符** → 拒绝启动。

每条都要做变异测试：把对应的检查删掉，确认它会红。
