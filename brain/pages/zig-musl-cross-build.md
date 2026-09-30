---
id: zig-musl-cross-build
title: "交叉编译工具链：Zig 0.14.1 + mipsel-linux-musleabihf 静态"
category: decision
status: active
tags: [toolchain, build]
created: "2026-09-29T00:51:31"
updated: "2026-09-30T02:55:25"
---

<!-- compiled_truth -->
用 **Zig 0.14.1 的 `zig cc` / `zig c++` -target mipsel-linux-musleabihf`** 作为交叉编译器（与 C1-Slim-Ports 同路线：musl 静态、单下载即用），CMake + Ninja 1.12 驱动；全部产物静态链接，直接 `adb push` 到 `/storage` 运行。

- toolchain 文件：`cmake/toolchains/mipsel-zig.cmake`（编译器包装脚本在 `.tools/bin/zig-{cc,cxx}-mipsel`，由 `scripts/fetch-tools.sh` 幂等生成，`CMAKE_SYSTEM_PROCESSOR=mips`）
- 档位：`-DQZ_PROFILE=minimal -DCMAKE_BUILD_TYPE=Release`（crypto/compress/textcodec ON；TLS、WAMR OFF），`qzjs`/`qzjs-rt` 各约 2.9 MB，`qzos-host` 约 4.3 MB
- 交叉目标不止 qzjs：`os/` 顶层（qzjs + LVGL + Fusion Pixel + uvrpc）走同一个 toolchain 文件，`scripts/build-os.sh --mips`

## 四个已踩平的工具链坑

按"症状 → 根因 → 修法"记，每条都是**症状与真因看起来不像**，所以记下来省得下次重新怀疑一遍自己。

1. **`zig cxx` 不是子命令**（是 `c++`）。生成的 C++ wrapper 一跑就退出码 2 + 一屏 zig usage，CMake 那边看起来像"工具链坏了/编译器不能用"。修法在 `fetch-tools.sh`：`cc:cc` / `cxx:c++` 显式映射，并在生成后对两个 wrapper 各跑一次 `--version` —— 静默坏掉的 wrapper 否则只会在 compiler-id 检测阶段炸出一条看不懂的报错。

2. **MIPS32 没有 lock-free 8 字节原子**，`-Watomic-alignment` 在 qzjs **自己的源码**上触发，而 qzjs 的 `-Werror`（`qz_enable_warnings`）把警告升成错误，第一次 MIPS 构建停在 `msgq.c`。这不是代码缺陷：qzjs 邮箱/OOM 计数器是 `int64_t`、QuickJS 的 BigInt 原子按语言就是 64 位，任何源级改写都消不掉；clang 会降级成 compiler-rt/libatomic 调用（正确、只是慢）。修法在 toolchain 层按目标放宽：`set(CMAKE_C_FLAGS_INIT "-Wno-atomic-alignment")`（CXX 同）—— 逐文件 `-Wno` 是散落的猜测，目标级才表达"这是 32 位目标的固有属性"。

3. **CMake >= 3.31 的 linker depfile 探测被 zig 的 lld 骗过**。CMake 会 `ld --help` 找 `--dependency-file` 字符串来决定是否走"linker 原生 depfile"；zig 自带 lld 在 `--help` 里列了它（它属于 lld-link），但 ELF 链接真会拒绝，于是**每个可执行文件链接**都死在 `error: unsupported linker arg: --dependency-file=.../link.d`。修法：`set(CMAKE_C_LINKER_DEPFILE_SUPPORTED FALSE)`（CXX 同），回到 zig cc 支持的 `-MD -MF link.d`。症状（只有可执行文件链接挂、静态库正常）指向链接器而不是配置，猜错方向会浪费很久。

4. **zig cc 无条件嵌入 DWARF**：即使 `-g0`，`zig objcopy --strip-all` 只删 symtab 不删 debug 段，宿主 x86 `strip` 不认 MIPS ELF。解法是 toolchain 里链接期剥离：`set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--strip-all -Wl,--build-id=none")`（SHARED 同样）。

## 验证通道

- `scripts/test-display.sh` —— 显示纯逻辑层单测（133 断言），不依赖宿主构建也不依赖设备。
- `scripts/verify-frames.sh` —— **原生 / MIPS 双平台帧一致性**：`/dev/epaper_lcd` 只写、真机抓不到画面，所以把同一段 JS 喂给两个平台的宿主、逐字节比较它们吐出的 P4 帧。显示逻辑全是整数与位运算，帧相同即等价性回归的强证据（顺带挡住 MIPS 上的对齐/端序意外）。MIPS 侧经 qemu-user 跑，rt 需 trampoline（见 [[qemu-isolated-trampoline]]）。视觉本身仍只能人眼在屏上看。
- `scripts/qemu-verify.sh` —— qzjs 自身的 8 项冒烟。


## Timeline

- time: 2026-09-29T00:51:31
  kind: decision
  summary: "Created this page: 交叉编译工具链：Zig 0.14.1 + mipsel-linux-musleabihf 静态"
  source: "移植方案拍板 2026-09-29"
  affects: [zig-musl-cross-build]

- time: 2026-09-29T00:51:56
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: brain update-truth
  affects: [zig-musl-cross-build]

- time: 2026-09-30T02:55:25
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: brain update-truth
  affects: [zig-musl-cross-build]
