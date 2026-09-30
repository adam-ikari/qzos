# qzos — qzjs 的 C1 Slim 移植

把 [qzjs](https://github.com/adam-ikari/qzjs)（嵌入式 WinterTC JavaScript 运行时）移植到
**C1 Slim / MP-D261 墨水屏设备**，在设备上运行交互式 REPL。
设备与构建方式参考 [Kasiin/C1-Slim-Ports](https://github.com/Kasiin/C1-Slim-Ports)。

## 目标设备

| 项 | 值 |
|---|---|
| SoC / ABI | MIPS32r2 little-endian，Linux hard-float（`mipsel-linux-musleabihf`） |
| 内存 | ~50 MiB |
| 屏幕 | 296×152 纯黑白墨水屏（`/dev/epaper_lcd`） |
| 键盘 | `/dev/input/event0`（matrix）+ `event1`（gpio keys） |
| 可写存储 | `/storage`；根分区只读 |
| 终端 | C1Terminal（Go 编写的 VT100/PTY 终端，桌面快捷键 `T`，49 列 × 18 行，内跑 `/bin/bash`） |

## 仓库结构

```text
qzjs/                      qzjs 源码（上游仓库，含全部 deps 子模块）
cmake/toolchains/          mipsel-zig.cmake — Zig 0.14.1 交叉工具链
.refs/C1-Slim-Ports/       参考仓库稀疏克隆（C1Terminal 源码，不入库）
scripts/fetch-tools.sh     下载 zig / ninja / qemu-user-static 到 .tools/
scripts/build-mips.sh      交叉编译 qzjs（Release + QZ_PROFILE=minimal）
scripts/qemu-verify.sh     无设备验证：qemu-user 下跑 REPL/加密/流/定时器冒烟测试
scripts/deploy-device.sh   adb push 到 /storage/c1/qzjs/ 并做设备端冒烟
scripts/c1term-install.sh  构建产物 C1Terminal 装机 + qzjs 挂 PATH + 屏上 REPL 启停桥
scripts/c1wifi             终端内的配网 CLI（随 c1term-install 装到 $D/bin）
scripts/c1term-launcher-app.sh  把终端注册成 C1ancher 的应用（launcher 里选它即开）
scripts/c1term-autostart.sh     可选：开机自启终端（默认未启用）
scripts/on-device-repl.sh  一键从 adb 拉起屏上 REPL（--stop 恢复桌面）
```

## 构建

```bash
scripts/fetch-tools.sh     # 首次：下载工具链（~50MB）
scripts/build-mips.sh      # → build-mips/qzjs、build-mips/qzjs-rt（~2.9MB 静态）
```

构建配置取 `QZ_PROFILE=minimal`：保留 crypto.subtle / CompressionStream /
TextCodec（即 WinterTC/ECMA-429 必选集），关闭 TLS 与 WAMR（见「已知限制」）。
REPL 本身不依赖这两项。

无真机时先做仿真验证：

```bash
scripts/qemu-verify.sh     # 期望输出 8 passed, 0 failed
```

## 部署与使用

```bash
ADB=adb scripts/deploy-device.sh        # 或 ADB=/mnt/c/.../adb.exe 走 Windows adb
ADB=adb scripts/c1term-install.sh       # C1Terminal + qzjs + c1wifi
ADB=adb scripts/c1term-launcher-app.sh --install   # 桌面 launcher 里的 terminal 应用
```

### 从 launcher 启动（日常用法）

桌面进入应用页（`c1pkg gui` / 我的应用），选 **terminal**：墨水屏变成终端，
`HOME` 退出后桌面自己接回去。这条路径不需要挂起桌面进程——见下面「launcher
集成契约」。

```bash
ADB=adb scripts/c1term-launcher-app.sh --launch   # 同样走 c1pkg launch，可从 adb 触发
ADB=adb scripts/c1term-launcher-app.sh --remove   # 撤掉这个应用
```

终端里可用的命令：

| 命令 | 作用 |
|---|---|
| `qzjs` | JS REPL（逐行求值，变量在会话内保持；`Ctrl-D` 退出） |
| `c1wifi` | 当前连接 / IP / 网关 / DNS |
| `c1wifi scan` | 扫描热点（射频关着时会先加载 atbm603x 模块，约数秒） |
| `c1wifi join <ssid> <psk>` | 连接并保存（开放网络省略 psk） |
| `c1wifi forget <ssid>` / `c1wifi off` | 删除已存网络 / 关掉射频 |

### 从 adb 启动（调试用）

```bash
ADB="adb -s <serial>" scripts/on-device-repl.sh
```

这条路用 SIGSTOP 挂起桌面进程，**启动与观察必须落在同一个 adb 会话内**：
adbd 会回收立即退出会话的子进程，`setsid ... &` 后单独结束会话会让 c1term
起来又消失。手动起时：

```bash
"$ADB" shell "setsid /usr/data/c1term/c1term-run.sh </dev/null >/tmp/c1term.log 2>&1 & sleep 4; pidof c1term"
```

两条路别同时开：终端实例会抢 `/dev/input`，launcher 里的入口已做重复启动拦截。
开机自启是可选项，默认**没有**装（`scripts/c1term-autostart.sh --enable` 才装，
`--disable` 用可写分区的 flag 关掉）。

卡住时执行 `"$ADB" shell /usr/data/c1term/c1term-stop.sh` 恢复桌面。

> 本测试机（MagicPen-206892）桌面是 **C1ancher** 而非原厂 `mpenMain`，
> 启停桥按 exe 路径挂起 `/usr/data/c1/core/*` 全部进程（见 README 结构中的
> `scripts/c1term-install.sh`）。
> ISOLATED 进程模型下 `qzjs` 会自动 fork 同目录的 `qzjs-rt`（经
> `/proc/self/exe` 解析），两个文件必须放在一起。

## launcher 集成契约（实测逆向结论）

C1ancher 自己不维护应用表：桌面应用页就是 `c1pkg gui`，它扫
`/storage/c1/apps/*/current` 并用 `c1pkg launch <id>` 启动。要让一个应用出现在
那里，需要满足两条 c1pkg 的隐式规则：

1. **权限即信任**：从 `/storage/c1/apps/<id>` 一路到 `versions/<ver>` 的每个目录
   都不能被 group/other 写（厂商为目录 0555、文件 0444）。世界可写的目录会被拒，
   报 `installed entry metadata is unavailable` ——这是信任校验，不是缺台账。
2. **交接靠锁**：`<ver>/.c1pkg-entry` 写入口文件名（无换行），`<ver>/.c1pkg-mode`
   写 `direct`。`c1pkg launch` 会 flock `/dev/shm/c1ancher-external-app.{lock,runlock}`
   并把 `direct` 写进 `.mode`，C1ancher 拿不到锁就不再碰 `/dev/epaper_lcd`；它全程
   处于 `S`（不是 SIGSTOP），退出后自动接回屏幕。不写 `.c1pkg-mode` 时默认
   `terminal` 模式，不适合自己抢屏的 C1Terminal。

包目录是只读的，所以里面只放一个转 exec 的薄壳，真身留在
`/usr/data/c1term/`，升级不用再动应用表。

限制：不在厂商签名索引里的应用，标题只能用 id（小写）；想要正式名称
"Terminal" 得进签名仓库（`/usr/data/c1/pkg/repository.{url,ed25519.pub}`，
索引格式 `C1PKG-INDEX 2`，缓存 `verified.v1` = 64B 签名 + 索引原文）——换掉信任锚
会废掉厂商应用商店，所以没做。

## 已知限制（移植笔记）

- **无 HTTPS / WebAssembly**：minimal 档位关闭了 mbedTLS TLS 与 WAMR；
  `fetch` 仅 http。需要时在 `build-mips.sh` 追加
  `-DQZ_WITH_TLS=ON` / `-DQZ_WITH_WAMR=ON`（WAMR 的 MIPS 目标尚未验证）。
- **配网的成功分支未跑过**：`c1wifi scan` / `join` 的新增-保存-删除-失败路径都是
  实测的（厂商已存的 3 个网络在操作前后数量不变），但设备周围没有我方持有凭据的
  AP，所以 `wpa_state=COMPLETED` 与拿到 DHCP 租约这条成功路径只按脚本逻辑推定。
  生效配置是 `/usr/resource/wpa_supplicant.conf`（`wifi_up.sh` 真正读的那个），
  不是桌面自己那份 `/usr/data/c1/wifi/wpa_supplicant.conf`。
- **REPL 不支持顶层 `await`**：qzjs 的 eval 通道按 classic script 求值；
  交互中用 `.then(...)` 或 `(async()=>{...})()`。
- **`fs` 不是全局**：WinterTC 模块表中列出的 `fs` 未以全局形式暴露（上游行为）。
- 屏幕仅 49×18 且墨水屏残影敏感：REPL 无光标闪烁，输出长行会换行堆叠，
  建议保持表达式简短。

## 验证结论（qemu-user, MIPS32r2）

`--version` / `-e` 求值 / REPL 多行会话 / `crypto.subtle` SHA-256 已知答案 /
`setTimeout` 异步 / `Response.text()` / `typeof fetch` 全部通过。
真机验证见 `scripts/deploy-device.sh` 的冒烟输出。
