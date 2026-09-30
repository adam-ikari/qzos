---
id: qzos-service-boundary
title: "JS→C 的唯一通道是系统服务面"
category: decision
status: active
tags: [architecture services authorization boundary]
created: "2026-09-30T14:54:52"
updated: "2026-09-30T15:58:04"
---

<!-- compiled_truth -->
---
title: "JS→C 的唯一通道是系统服务面：收口授权边界"
category: decision
status: active
tags: [architecture, services, authorization, boundary]
---

## 决策

**只有系统服务可以调用 C。** JS（shell 与应用）碰到宿主 C 代码的面**只有一条**：
uvrpc 系统服务面。UI 桥降级为**纯渲染命令通道**，不承载任何能力。

## 为什么这条比「JS 优先 C 兜底」正确

`qzos-js-first` 曾记「服务面 JS 优先、C 兜底」——`op:rpc` 先查 JS 服务表、
未命中才落 uvrpc。它与本决策**方向相反**，已翻正（见 timeline 的 reversal）。

「兜底」的含义是 C 原语层是**每个 JS 服务都能碰到**的。于是 JS 自己写的服务
天然成了绕过授权的后门：它在 C 原语之上再加一层自己的逻辑，而授权检查挂在
渲染桥的 `op_rpc` 上，对它无效。

而 uvrpc 服务面是**特权路径**：`services.c` 的注册表就是 C 能力的**完整清单**。
授权检查落在那里，才能做到「清单之外无 C 能力」。

## 三条推论

1. **授权检查的落点从渲染桥移到服务面。** 挂在渲染通道上是历史偶然，不是设计。
2. **「系统服务」有了明确外延** —— `services.c` 里注册的方法集合。
3. **C 侧增删能力不影响 JS 侧授权** —— 加一个能力 = 加一个 handler，
   不是改渲染协议。

## 当前违反项（2026-09-30 盘点）

| 违反 | 位置 | 严重度 |
| --- | --- | --- |
| `ui.rpc` 直接走 `bridge.c` 的 `op_rpc`，不经服务面注册表 | `bridge.c` | 高 |
| 授权上下文经渲染桥下发（`op:app` 走 UI 协议） | `bridge.c` | 中 |
| 服务面注册表只有 `sys.info` 一个能力 | `services.c` | 能力太窄，边界形同虚设 |
| 遮蔽只封了 `qzjs.fs` / `__native__`，**没封 uvrpc** | `sandbox.js` | 高 |

最后一条最要紧：**JS 能通过 `ui.rpc` 到达服务面，而遮蔽层对此一无所知。**
授权完全依赖 C 侧 `app_allows` 那一道 —— 也就是说 C 侧那一道是**唯一**的
防线，没有第二层。这不是错，但它意味着「服务面收口」必须做到**完整**，否则
等于没做。

## 收口后的目标形状

```text
JS 应用 ──ui.*────> 渲染桥（纯命令，无能力）
         │
         └─ui.rpc──> 服务面（唯一的能力边界）
                        ├─ 授权检查（当前应用 perms，缺省为空）
                        └─ 命中 C handler ──> 才碰到 C
```

「系统服务」= 服务面注册表里的方法 = C 能力的完整清单。

## 与 JS-first 的关系（不矛盾）

[[qzos-js-first]] 说的是**语言选择**：策略与组合写 JS，syscall / 必须待在应用
上下文之外的东西写 C。本决策说的是**边界形状**：JS 到 C 只有一条路。

所以「JS 优先」的落法是：**服务面优先命中 JS 实现的服务**（策略层仍是 JS），
而 uvrpc 只承载**确实是 C 的那少数几个**。不是「JS 优先 C 兜底」——那个措辞
暗示 C 可被任意 JS 服务触达，是错的表述。

## 验证纪律

收口完成的判据不是「代码看起来分了层」，而是：

- **服务面注册表之外无 C 能力**：从 JS 出发穷举可达路径，逐条对照注册表。
- **能力检查在服务面侧**：把 `op_rpc` 的检查移走之后，测试仍必须全绿——
  否则说明还有别的入口。
- **遮蔽层要认识 uvrpc**：JS 侧必须无法绕过服务面自己够到 C 原语。


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
