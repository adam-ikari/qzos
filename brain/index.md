# Brain Index

_Auto-generated. Last updated 2026-09-30T08:56:15.459Z._

- [c1-slim-device](pages/c1-slim-device.md) — category: reference | tags: [hardware, target, device] | 移植目标硬件：**C1 Slim / MP-D261**（快易典 MagicPen 系列电子纸学习机），资料与构建方式参考 [Kasiin/C1-Slim-Ports](https://github.com/Kasiin/C1-Slim-Ports)。
- [c1-wifi-stack](pages/c1-wifi-stack.md) — category: reference | tags: [wifi, network, device] | 射频是 **atbm603x SDIO**，模块 `/etc/firmware/atbm603x_wifi_sdio.ko` **按需 insmod**——默认不开 WiFi，`lsmod` 空、`/sys/class/net/wlan0` 不存在是正常状态。
- [c1ancher-app-integration](pages/c1ancher-app-integration.md) — category: reference | tags: [launcher, c1ancher, c1pkg] | C1ancher **自己不维护应用表**。
- [port-verification](pages/port-verification.md) — category: project | tags: [verification, device, perf] | ## 两条线：qzjs 移植（已上真机）/ qzos 宿主（自动化已建立，未上真机）
- [qemu-isolated-trampoline](pages/qemu-isolated-trampoline.md) — category: decision | tags: [test, qemu, build] | qzjs 默认 ISOLATED 进程模型：宿主 `qzjs` fork+exec 同目录 `qzjs-rt`（解析链：显式路径 → `QZ_RT_SERVER` 环境变量 → `/proc/self/exe` 同目录 → 编译期 `QZ_RT_PATH`，见 `qzjs/src
- [qzos-app-package](pages/qzos-app-package.md) — category: decision | tags: [app, package, manifest, permission, security] | ## 决策：qzos 应用包结构 = manifest 契约 + 授权（JS 遮蔽 + C 方法名边界）
- [qzos-as-system](pages/qzos-as-system.md) — category: decision | tags: [system, boot, powerservice, takeover] | ## 决策：qzos 作为「系统」接管设备，逐项替换原有程序
- [qzos-js-first](pages/qzos-js-first.md) — category: decision | tags: [layering, js, c99, architecture, services] | ## 决策：JS 优先分层，C99 只在必要时
- [qzos-services-rpc](pages/qzos-services-rpc.md) — category: decision | tags: [uvrpc rpc services ipc libuv] | ## 决策：uvrpc 承担系统服务通讯面
- [qzos-ui-architecture](pages/qzos-ui-architecture.md) — category: decision | tags: [ui lvgl architecture bridge] | ## 决策：三层结构
- [zig-musl-cross-build](pages/zig-musl-cross-build.md) — category: decision | tags: [toolchain, build] | 用 **Zig 0.14.1 的 `zig cc` / `zig c++` -target mipsel-linux-musleabihf`** 作为交叉编译器（与 C1-Slim-Ports 同路线：musl 静态、单下载即用），CMake + Ninja 1.12 驱动；全部
