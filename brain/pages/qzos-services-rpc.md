---
id: qzos-services-rpc
title: "QZ OS 系统服务通讯：uvrpc (libuv loop 注入 + FlatBuffers)"
category: decision
status: active
tags: [uvrpc rpc services ipc libuv]
created: "2026-09-30T00:24:44"
updated: "2026-09-30T07:59:57"
---

<!-- compiled_truth -->
## 决策：uvrpc 承担系统服务通讯面

- **定位**：qzos 的系统服务（显示/输入/存储/网络/电量等）之间及服务与宿主之间的 RPC 基础设施用 uvrpc（github.com/adam-ikari/uvrpc，MIT，1.0.0a）。应用 UI 桥仍走 qzjs JSON postMessage——uvrpc 只承担**系统服务面**，不替换 UI 桥。
- **与宿主模型契合**：uvrpc 是 loop 注入式（不自己跑 uv_run），零线程零锁；qzos-host 恰好已有单线程 uv loop 泵（qzjs 宿主 + lv_timer_handler），uvrpc server/client 共享该 loop，天然满足。
- **传输选型**：
  - `IPC`（Unix socket，如 `/storage/qzos/rpc.sock`）：宿主 ↔ 独立服务进程；
  - `INPROC`/`SAMELOOP`：宿主进程内模块直连（零拷贝/虚表旁路）；
  - TCP/UDP 留给将来（如访问网络服务）。
- **序列化**：FlatBuffers（flatcc runtime + 预生成 reader/builder 头），协议 schema 在 `third_party/uvrpc/schema/rpc.fbs`；qzos 自有服务的 .fbs 放 `os/schema/`，生成头随仓提交（避免交叉编译时依赖宿主 flatcc 编译器）。

## 依赖与构建约束（硬性）

- **libuv 单实例**：uvrpc 必须编译并链接 qzjs 的 vendored libuv（`qzjs/deps/libuv`，target `uv_a`）——qzjs 在 uv_loop_t 上按值嵌入并要求宿主链接同一份 libuv；**禁止**使用 uvrpc 自带 deps/libuv 或系统 libuv。uvrpc 顶层 CMake（mimalloc 默认、强制输出目录、deps 查找顺序）不用于集成，改由 `os/CMakeLists.txt` 自建 uvrpc 静态库目标（源清单 = 其顶层 CMakeLists `UVRPC_SOURCES`，20 个 .c）。
- **allocator**：`-DUVRPC_DEFAULT_ALLOCATOR=0`（system malloc）；不引入 mimalloc（musl 静态交叉编译无收益、增加体积与风险）。
- **flatcc**：runtime（builder.c/emitter.c/refmap.c 等）与 `include/flatcc` vendor 进仓；生成器 flatcc 只需原生机跑一次（脚本 `scripts/gen-uvrpc-schema.sh`），生成头提交。
- **uthash**：单头文件 `third_party/uvrpc/deps/uthash/src/uthash.h`。
- **编译标准**：gnu99（纯 c99 缺 POSIX 宏会报 pthread_rwlock_t / CLOCK_MONOTONIC 错误）。

## 验证基线（2026-09-30，原生 x86_64）

- 20/20 uvrpc 源文件原生编译通过（system allocator + qzjs libuv 头）。
- IPC 单 loop 往返：server 注册 "Add" → `connect_with_callback` → `call` → 响应回调，`UV_RUN_NOWAIT` 泵法，结果 30 正确，`roundtrip OK`。
- 上游 example scenario_1 用 `UV_RUN_DEFAULT` 会因 listening server 永不返回——宿主集成一律用 NOWAIT/定时泵（与 lv_timer_handler 节奏一致）。

## 关键 API 速查

- 服务端：`uvrpc_config_new/set_loop/set_address` → `uvrpc_server_create` → `uvrpc_server_register(server, "method", handler, ctx)` → `uvrpc_server_start`；handler 内 `uvrpc_request_send_response(req, UVRPC_OK, buf, len)`。
- 客户端：同 config → `uvrpc_client_create` → `uvrpc_client_connect_with_callback` → `uvrpc_client_call(client, "method", params, n, cb, ctx)`；回调收 `uvrpc_response_t{status, result, result_size}`。
- 传输由 address 前缀决定（tcp:// udp:// ipc:// inproc:// sameloop://），`uvrpc_config_set_transport` 需显式设置（UVBUS_TRANSPORT_IPC=2）。
- 客户端有 stream 模式（`send_response_more` + `uvrpc_response_is_stream_more/is_stream_end`），可用于服务主动推送事件。

## Timeline

- time: 2026-09-30T00:30:00
  kind: decision
  summary: "采用 uvrpc 作为系统服务通讯基础设施（loop 注入 + IPC/INPROC + FlatBuffers）；UI JSON 桥不变，uvrpc 只管系统服务面；libuv 单实例复用 qzjs vendored，system allocator，gnu99；原生 IPC 往返冒烟通过（NOWAIT 泵法）。"


## Timeline

- time: 2026-09-30T00:24:44
  kind: decision
  summary: "Created this page: QZ OS 系统服务通讯：uvrpc (libuv loop 注入 + FlatBuffers)"
  source: created via brain create-page
  affects: [qzos-services-rpc]

- time: 2026-09-30T00:24:44
  kind: decision
  summary: "采用 uvrpc (github.com/adam-ikari/uvrpc) 作为系统服务通讯基础设施：loop 注入宿主 uv loop（零线程/零锁，与 qzos-host 单线程泵模型一致），IPC 传输用于跨进程服务，INPROC/SAMELOOP 用于宿主内模块；序列化 FlatBuffers(flatcc)。依赖方案：libuv 复用 qzjs vendored libuv（严禁第二份 libuv 实例），flatcc/uthash vendor 进 third_party/uvrpc/deps，allocator 用 system(-DUVRPC_DEFAULT_ALLOCATOR=0)，不引 mimalloc。原生 IPC 往返冒烟已通过。UI JSON 桥保持不变，uvrpc 只承担系统服务面。"
  affects: [qzos-services-rpc]

- time: 2026-09-30T00:25:37
  kind: decision
  summary: "uvrpc 作为系统服务通讯基础设施的完整决策与构建约束"
  source: brain update-truth
  affects: [qzos-services-rpc]

- time: 2026-09-30T07:59:57
  kind: decision
  summary: "third_party 转 submodule 待办：lvgl=v9.6.0(80ca777) 与 uvrpc=main(ac450c5) 已核对与本地副本一致；brain 原记的 uvrpc 1.0.0a 版本有误（仓库无此 tag）；vendor 已挪到 os/third_party/"
  source: "提交 540eb5a 时的网络阻塞"
  affects: [qzos-services-rpc]
