---
id: qzos-service-boundary
title: "JS→C 的唯一通道是系统服务面"
category: decision
status: active
tags: [architecture services authorization boundary]
created: "2026-09-30T14:54:52"
updated: "2026-10-01T05:45:18"
---

<!-- compiled_truth -->
# JS 可以点名一个应用，不能决定它能做什么

`op:app` **只转发应用 id**，`perms` 字段一律不看 —— 即使它带着、即使格式正确。
能力唯一合法来源是磁盘上的 `<QZ_JS_DIR>/apps/<id>/app.json`，由 C 侧
`os/src/appauth.c` 推导。

## 为什么这条必须单独立一条

授权的**判定位置**和判定的**输入可信性**是两件事。上一轮把判定从渲染桥搬到
服务面是对的，但**输入仍然由攻击者提供**，于是完整提权成立。

实测（`perms: []` 的应用，不需要外部进程、不需要 socket）：

```
DECLARED=[]                      ← manifest 只声明零能力
SETAPP ACCEPTED
qzos-services: app 'escaper' authorized (1 caps)
STORAGE *** ALLOWED ***
```

一行 `ui.setApp('escaper', ['storage'])` 就够了。根因：**应用与 shell 共享同一个
QuickJS 全局**，`ui` 是全局对象，所以「谁能声明授权」这个问题在应用上下文里
是可以被回答的——回答者就是攻击者自己。

## appauth.c 的三条规则

1. **id 必须能当单级目录名用**：拒绝 `/`、前导 `.`、空串、超长。必须在**拼路径
   之前**判断——拼完之后再 `realpath` 检查太晚。
2. **manifest 里的 `id` 必须与目录名一致**：否则把 victim 的 manifest 拷进自己
   目录就能继承它的授权。目录名来自路径（不可伪造），manifest 里的 id 来自文件
   （可改）。
3. **表外能力整条作废**，不部分放行。静默忽略会让应用带着残缺授权在系统里跑而
   作者不知情。

另外：读不到就是读不到，**绝不猜、绝不沿用上一次的授权**。后者是「静默给错」的
另一种形状——不是给多，是给错的那个。

## 两份实现，方向相反

`os/js/apkg.js` 有一份同规则的实现，但只用于**发现与展示**（决定要不要把应用列
出来、UI 上显不显示按钮）；`appauth.c` 只用于**强制**。JS 那份被改坏时最多让界面
显示出错的按钮，C 这份被改坏才是安全问题。同步靠闸门，不靠约定。

## 残余风险：身份可以被借用

**修掉了**：应用不能超出**它点名的那个应用**在磁盘上声明的能力。

**没修掉**：id 本身仍由 JS 断言。运行中的应用可以点名另一个已安装的应用，借用
它的能力。宿主没有任何独立于 JS 的「现在跑的是谁」。

在 JS-first + 单 QuickJS 上下文下没有便宜的修法：

- 「已激活时拒绝改指向」挡不住——先 `setApp(null)` 再 `setApp(别人)` 即可
- 按 uid 授权无意义——设备是单用户 root 盒
- 真解法是**每个应用一个 qzjs-rt 进程**，而 sandbox.js 的整个形状都建立在
  「共享上下文」这个前提上

缓解面：受信根只有 `<QZ_JS_DIR>/apps`，所以用户目录里的应用**一律零能力**
（与两路信任一致），可被借用的集合仅限随系统发布的内置应用。


## Timeline

- time: 2026-09-30T14:54:52
  kind: decision
  summary: "Created this page: JS→C 的唯一通道是系统服务面"
  source: "用户定约束「只有系统服务可以调用 c」"
  affects: [qzos-service-boundary]

- time: 2026-09-30T14:54:52
  kind: decision
  summary: "JS 碰 C 只有 uvrpc 服务面一条路，UI 桥降为纯渲染命令；授权检查从渲染桥移到服务面；服务面注册表即 C 能力完整清单。翻正 qzos-js-first 的「JS 优先 C 兜底」——那个措辞暗示 C 可被任意 JS 服务触达，等于开后门"
  source: brain update-truth
  affects: [qzos-service-boundary]

- time: 2026-09-30T15:20:57
  kind: evidence
  summary: "收口落地：授权检查从 bridge.c 的 op_rpc 移到 services.c；引入穷举式注册表 s_registered[]（此前是「以 sys. 开头 + 前缀取 cap」的匹配，那等于给未实现的方法留了扇没锁的门）；新增 sys.storage.statfs（第一个真碰 OS 的服务，需要 storage 能力）作为收口是否真的收住的试金石；bridge.c 只转交 op:app、不再保存授权状态。测试自身抓出一个会让授权**静默给错**的 bug：qzos_services_set_app_perms 里写 s_app_caps[n] 而 n 是输入总数而非下标，授 2 个能力会两次都写进 index 2，结果是 3 个槽位里两个空串 + 一个 storage。另发现 default-deny 这条最关键的不变量**此前没有任何直接测试**（caps_allow 是 static，测试只能查存了几个能力、查不到未设授权时会不会放行），补了 qzos_services_would_allow 可观测钩子。3 个变异全部会红：服务面不检查 / 缺省放行 / 写错下标"
  source: "brain append-timeline：30 断言 + 3 变异测试 + 端到端 10 项 + MIPS 端到端 6 项"
  affects: [qzos-service-boundary, qzos-app-package]

- time: 2026-09-30T15:57:58
  kind: evidence
  summary: "收口后自查抓到一个真洞并修掉：服务面有**两个入口**而我只守了一个。授权检查住在 qzos_services_rpc()（INPROC，JS 走的那条），而 IPC 监听器曾把 handler **直接**注册进去，完全绕过授权——实测宿主里一个应用都没跑，外部进程连上 /storage 上的 socket 就调通了 sys.storage.statfs 并拿到挂载信息；设备上 /storage 是 0777、socket 被 uvrpc 建成 0755，等于任何应用都能读存储布局，也让 power 能力那句「绝不默认授予」彻底作废。修法不是给 IPC 补授权（unix socket 没有可用的调用方身份，设备又是单用户 root 盒，uid 区分不出谁是谁），而是划清暴露面：需能力的方法一律不上 IPC，socket 收紧到 0600，扣下的每个方法都在启动日志里点名（否则「某服务在 IPC 上调不通」会被当 bug 查）。同时补 s_bound[]：uvrpc client 恒填 status=OK，注册失败时表里有、uvrpc 里没绑上，调用进空洞会返回「成功」+一坨二进制，现在改成明确的 service not bound"
  source: "brain append-timeline：新闸门 scripts/test-ipc-surface.sh 8 断言 + 单测增至 38 断言 + 7 个变异全有归属"
  affects: [qzos-service-boundary, qzos-app-package]

- time: 2026-09-30T15:58:04
  kind: note
  summary: "**uvrpc 客户端无法分辨错误回执**（第三方限制，仓内修不了）：third_party/uvrpc 的 client 恒填 status=UVRPC_OK、error_code=0（uvrpc_client.c:158），而 server 对「handler 不存在」是把 int32 错误码塞进 result 头 4 字节 + 消息串（uvrpc_server.c:207）——线上没有标签可区分。于是任何 uvrpc 客户端都把失败看成成功。派生物：① 曾在 tools/qzos-rpc-client.c 按「头 4 字节非零即错误」解码，结果 sys.info 的 {\"se 被读成错误码 1702044283，**猜比不猜更糟**，已回退；② 判据改取宿主日志的 Handler not found（服务端权威记录）；③ 本仓免疫方式是派发前自己 svc_find() 且注册/派发同源于 s_registered[]，所以 handler 缺失不可达。**推论：任何基于 uvrpc 的判据都不能只读客户端回执**"
  source: brain append-timeline
  affects: [qzos-service-boundary, port-verification]

- time: 2026-10-01T05:45:00
  kind: evidence
  summary: "修掉一个实测的提权漏洞：op:app 曾把 JS 侧填的 perms 数组当授权收下，而应用与 shell 共享同一个 QuickJS 全局、ui 是全局对象，于是任何应用都能 ui.setApp('self',['storage']) 给自己加能力（实测：DECLARED=[] → SETAPP ACCEPTED → authorized (1 caps) → sys.storage.statfs ALLOWED）。修法：授权只从磁盘 manifest 推导（appauth.c 三条规则：id 可当单级目录名/manifest 的 id 必须与目录名一致/表外能力整条作废），bridge.c 的 op:app 退化成只转发 id 并在 claimed≠derived 时记账。新增 scripts/test-appauth.sh（9 断言，含正对照：磁盘声明 storage 的应用即使自报零能力仍放行）与 test-services.sh 的 36 条新断言（38→74）。7 个变异全部有归属（其中 3 个双红）"
  source: brain append-timeline
  affects: [qzos-service-boundary, qzos-app-package]

- time: 2026-10-01T05:45:18
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: brain update-truth
  affects: [qzos-service-boundary]
