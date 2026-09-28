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
scripts/on-device-repl.sh  一键拉起屏上 REPL（--stop 恢复桌面）
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
```

设备上（已装 C1Terminal 时）：

1. 启动屏上终端（临时挂起桌面进程，HOME 退出后自动恢复）：

   ```bash
   ADB="adb -s <serial>" scripts/on-device-repl.sh
   ```

   手动起时，**启动与观察必须落在同一个 adb 会话内**：adbd 会回收立即退出
   会话的子进程，`setsid ... &` 后单独结束会话会让 c1term 起来又消失。

   ```bash
   "$ADB" shell "setsid /usr/data/c1term/c1term-run.sh </dev/null >/tmp/c1term.log 2>&1 & sleep 4; pidof c1term"
   ```

2. 屏幕出现 `c1slim#` 后输入 `qzjs`（`/usr/data/c1term/bin` 已入 PATH）；
3. 逐行输入 JS 求值（变量在同一会话内保持），`Ctrl-D` 退出 REPL，`HOME` 退出终端。

卡住时执行 `"$ADB" shell /usr/data/c1term/c1term-stop.sh` 恢复桌面。

> 本测试机（MagicPen-206892）桌面是 **C1ancher** 而非原厂 `mpenMain`，
> 启停桥按 exe 路径挂起 `/usr/data/c1/core/*` 全部进程（见 README 结构中的
> `scripts/c1term-install.sh`）。
> ISOLATED 进程模型下 `qzjs` 会自动 fork 同目录的 `qzjs-rt`（经
> `/proc/self/exe` 解析），两个文件必须放在一起。

## 已知限制（移植笔记）

- **无 HTTPS / WebAssembly**：minimal 档位关闭了 mbedTLS TLS 与 WAMR；
  `fetch` 仅 http。需要时在 `build-mips.sh` 追加
  `-DQZ_WITH_TLS=ON` / `-DQZ_WITH_WAMR=ON`（WAMR 的 MIPS 目标尚未验证）。
- **REPL 不支持顶层 `await`**：qzjs 的 eval 通道按 classic script 求值；
  交互中用 `.then(...)` 或 `(async()=>{...})()`。
- **`fs` 不是全局**：WinterTC 模块表中列出的 `fs` 未以全局形式暴露（上游行为）。
- 屏幕仅 49×18 且墨水屏残影敏感：REPL 无光标闪烁，输出长行会换行堆叠，
  建议保持表达式简短。

## 验证结论（qemu-user, MIPS32r2）

`--version` / `-e` 求值 / REPL 多行会话 / `crypto.subtle` SHA-256 已知答案 /
`setTimeout` 异步 / `Response.text()` / `typeof fetch` 全部通过。
真机验证见 `scripts/deploy-device.sh` 的冒烟输出。
