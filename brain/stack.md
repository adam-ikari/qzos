---
slug: stack
title: Tech stack
role: tech-stack choices
updated: "2026-09-29T00:52:29"
---

# Tech stack

## Technology choices

| domain | candidates | decision | rationale |
|---|---|---|---|
| 交叉编译器 | musl.cc gcc / Android NDK clang / **Zig 0.14.1** | Zig `zig cc -target mipsel-linux-musleabihf` | 单下载即用、与 C1-Slim-Ports 同路线、clang 前端对 qzjs 全 C99 依赖友好 |
| 链接形态 | 动态 glibc / **静态 musl** | 静态 musl（hardfloat） | 根分区只读、无 sysroot 依赖，`adb push` 即跑 |
| 剥离方式 | objcopy / 宿主 strip / **lld 链接期** | `-Wl,--strip-all -Wl,--build-id=none` | zig cc 无条件带 DWARF，其余两种都不可用，见 [[zig-musl-cross-build]] |
| 构建驱动 | make / **CMake+Ninja** | CMake 3.28 + Ninja 1.12（`.tools/` 内自带） | qzjs 上游即 CMake/Ninja |
| 功能档位 | standard / **minimal** | `QZ_PROFILE=minimal`（Release） | XBurst 对未对齐敏感，避开 WAMR；ECMA-429 必选集仍满足（TLS/WAMR 为后续评估） |
| 无设备验证 | 跳过 / **qemu-user** | `qemu-mipsel-static` + QZ_RT_SERVER trampoline | ISOLATED 双进程也能在 x86 上回归，见 [[qemu-isolated-trampoline]] |
| 部署通道 | serial / **root ADB** | Windows `adb.exe -s MagicPen-206892`（经 WSL 调用） | 设备已开 root ADB，`/storage` 可执行 |
| 终端 UI | 自研墨水屏 REPL / **C1Terminal** | 复用 C1-Slim-Ports 的 C1Terminal（Go，PTY+VT100，键 `T`） | 已在上游验证的 49×18 文本终端；本测试机尚未安装 |

## Open items

- `-DQZ_WITH_TLS=ON`（HTTPS fetch）与 `-DQZ_WITH_WAMR=ON`（WebAssembly）在 MIPS 上的可用性评估。
- C1Terminal 是否纳入本仓库统一构建/安装。
