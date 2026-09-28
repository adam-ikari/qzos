# Brain Index

_Auto-generated. Last updated 2026-09-28T17:27:28.938Z._

- [c1-slim-device](pages/c1-slim-device.md) — category: reference | tags: [hardware, target, device] | 移植目标硬件：**C1 Slim / MP-D261**（快易典 MagicPen 系列电子纸学习机），资料与构建方式参考 [Kasiin/C1-Slim-Ports](https://github.com/Kasiin/C1-Slim-Ports)。
- [port-verification](pages/port-verification.md) — category: project | tags: [verification, device, perf] | ## 验证状态（2026-09-29）
- [qemu-isolated-trampoline](pages/qemu-isolated-trampoline.md) — category: decision | tags: [test, qemu, build] | qzjs 默认 ISOLATED 进程模型：宿主 `qzjs` fork+exec 同目录 `qzjs-rt`（解析链：显式路径 → `QZ_RT_SERVER` 环境变量 → `/proc/self/exe` 同目录 → 编译期 `QZ_RT_PATH`，见 `qzjs/src
- [zig-musl-cross-build](pages/zig-musl-cross-build.md) — category: decision | tags: [toolchain, build] | 用 **Zig 0.14.1 的 `zig cc -target mipsel-linux-musleabihf`** 作为交叉编译器（与 C1-Slim-Ports 同路线：musl 静态、单下载即用），CMake 3.28 + Ninja 1.12 驱动 qzjs 构建；全部
