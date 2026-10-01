---
id: zig-musl-cross-build
title: "交叉编译工具链：Zig 0.14.1 + mipsel-linux-musleabihf 静态"
category: decision
status: active
tags: [toolchain, build]
created: "2026-09-29T00:51:31"
updated: "2026-10-01T09:57:38"
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

- time: 2026-10-01T05:45:18
  kind: note
  summary: "**qzjs 的嵌套子模块补丁由 qzjs/CMakeLists.txt 自己在配置阶段打**（quickjs-ng-c99-atomics / quickjs-ng-drain-jobs / libuv-c99-atomics，带「已应用则跳过」的幂等分支），所以仓内 scripts/ 与 tools/ 里搜不到任何 git apply 属正常。曾据此误判为「fresh clone 不可复现」——把 libuv 与 quickjs-ng 两个嵌套子模块 checkout 回干净上游再构建，CMake 重新打上补丁，工作树与还原前逐行相同（101/331 行），全量闸门 rc=0。另有 5 个补丁文件未被构建引用（quickjs-ng-bc-reader-hardening / quickjs-ng-debugger），内容已含在现用补丁里或已被取代。推论：**判断可复现性问题要在正确的层找补丁应用点**，本仓的补丁层在子模块自己的 CMake 里，不在本仓脚本里；「本仓搜不到 patch」不等于「没人打 patch」"
  source: brain append-timeline
  affects: [zig-musl-cross-build, port-verification]

- time: 2026-10-01T07:05:21
  kind: note
  summary: "升级 uvrpc 暴露了一条**断掉的路**：`scripts/prepare-thirdparty.sh` 在产物缺失时提示「regenerate with scripts/gen-uvrpc-schema.sh」，而**那个脚本根本不存在**。也就是说「uvrpc 升级后生成头过期怎么办」这条路是断的——而它正是每次升级 uvrpc 都会撞上的第一件事。已补上：git submodule update --init deps/flatcc → 用 flatcc 自己的 CMake 编 flatcc_cli（手写 cc 命令会因漏掉 external/hash/*.c 而链接失败，且源文件列表会随版本烂掉）→ flatcc -c -v -w 生成 → 5 个头逐一检查。三个坑记下来：① 目标名是 flatcc_cli，flatcc 那个是静态库 libflatcc.a；② 产物落在**源码树**的 bin/ 下而不是 build 目录，找错位置时 cp 报 cannot stat，看起来像编译失败；③ -v 那个开关就是生成 rpc_verifier.h 的，少写它就少一个头，而缺头的报错（rpc_verifier.h: No such file or directory）完全看不出根因是「入库的生成产物过期了」"
  source: brain append-timeline
  affects: [zig-musl-cross-build, qzos-services-rpc]

- time: 2026-10-01T07:45:01
  kind: note
  summary: "**qzjs 85d056a9 → 960f24d5（25 提交）已评估，决定暂不升。** 公开头 qzjs.h 无变化、`qz_config_t.initial_script` 不变，所以对我们是纯 gitlink 升级；原生构建与全量闸门都过（rc=0），MIPS 端到端也过。**但那个 MIPS 全绿是假的**（见下一条）。升不升的理由：收益是 mbedtls 3.6.7 + timingSafeEqual 等内部修复（宿主不用 TLS，收益有限），代价是**交叉构建不再可能**——`821c3cf5 fix(build)!: 内嵌字节码改为纯构建产物——不再 committed` 把 src/polyfill_default.c 变成 gitignore 的产物，而重新生成它必须用**目标架构的 qjsc**（qzjs 的 CMake 把 QJSC 硬写成 $<TARGET_FILE:qjsc>），于是 x86 宿主上 Exec format error。`b383449d feat(runtime)!: 启动脚本改经管道传递` 本身对我们无感（只改了 --script PATH → --script-stdin 的内部传输）。**需要上游改的**：让 QJSC 可覆盖（例如 -DQZ_QJSC_HOST=<宿主 qjsc 路径>），或把 polyfill 字节码生成提成宿主侧步骤。qzjs 自己的注释已经警告过相邻风险：「撞上残留旧版 qjsc 会静默产出引擎拒读的字节码 → JS_ReadObject: invalid version → qz_create 返回 NULL」——对交叉构建来说「构建目录自己那个 qjsc」按定义就是错架构的那个"
  source: "brain append-timeline：升级评估 + 实测交叉构建失败"
  affects: [zig-musl-cross-build, qzos-as-system]

- time: 2026-10-01T09:57:38
  kind: evidence
  summary: "**qzjs 已升级到 bdbbb637（origin/master 960f24d5 + 一条上游改动），交叉构建恢复。** 上游那条是 821c3cf5「内嵌字节码改为构建产物、不再 committed」带来的阻塞的解法：新增 CMake 选项 QZ_QJSC_HOST，让交叉构建能给 polyfill 一个**能在宿主机上运行**的 qjsc（默认那个是 $<TARGET_FILE:qjsc>，交叉构建时是目标架构二进制，x86 宿主 Exec format error）。不设该选项时行为完全不变。已推到 qzjs 的 cross-build-qjsc 分支；本仓只记 gitlink SHA。**一致性由调用方负责**：那个宿主 qjsc 必须由同一份 quickjs-ng 编出，所以本仓 build-os.sh --mips 的顺序是先编宿主再编目标（两者共用 deps/quickjs-ng，BC_VERSION 一致）。**这个方案引入了一个新失效面**：宿主 qjsc 产出的字节码若与目标引擎的 BC_VERSION 不匹配 → 引擎拒读 → qz_create 返回 NULL → 桌面永不出现。所以 verify-mips-e2e 加了 assert_engine_up 显式断言「JS engine up」且无 boot failure——不显式断言的话，这个失效会以「渲染异常」的形式冒出来，排查时想到的是屏幕和主题，而不是字节码版本"
  source: "brain append-timeline：从零 rm -rf build-os-mips 重建通过 + 全量 rc=0 + MIPS e2e 11 断言"
  affects: [zig-musl-cross-build, port-verification]
