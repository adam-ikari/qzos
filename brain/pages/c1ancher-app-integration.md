---
id: c1ancher-app-integration
title: "C1ancher 应用/launcher 集成契约（c1pkg 权限信任 + external-app 交接）"
category: reference
status: active
tags: [launcher, c1ancher, c1pkg]
created: "2026-09-29T02:32:44"
updated: "2026-09-29T02:33:02"
---

<!-- compiled_truth -->
C1ancher **自己不维护应用表**。桌面应用页就是内嵌终端里跑的 `clear; /usr/data/c1/bin/c1pkg gui`，它扫 `/storage/c1/apps/*/current` 列「我的应用」，选中后执行 `c1pkg launch <id>`。所以要往 launcher 里加一个应用，只需要满足 c1pkg 的隐式契约——不需要重编译、不需要写只读 rootfs。

## 应用目录契约

```
/storage/c1/apps/<id>/current -> versions/<ver>          （相对符号链接）
/storage/c1/apps/<id>/versions/<ver>/.c1pkg-entry         入口文件名，无换行
/storage/c1/apps/<id>/versions/<ver>/.c1pkg-mode          可选：direct
/storage/c1/apps/<id>/versions/<ver>/<entry>              可执行入口
```

`c1pkg list` 只要目录在就认（`terminal 1.0.0` 立刻出现）。

## 坑一：权限即信任（不是缺台账）

从 `/storage/c1/apps/<id>` 一直到 `versions/<ver>` 的**每一个目录**都不能被 group/other 写。世界可写时 `c1pkg launch` 报：

```
c1pkg: installed entry metadata is unavailable
```

逐项实验结论：入口文件本身 0666 无影响；**version 目录** 0777 → 报错；**上层 id/versions 目录** 0777（version 目录已 0555）→ 同样报错。厂商形态是目录 0555、文件 0444、root:root。看到这条错不要去翻 WAL/台账——`C1PKG-WAL-1` 与 `/usr/data/c1/pkg/highest-sequence` 是仓库索引回滚状态，与 launch 无关。

## 坑二：屏幕/输入交接靠 advisory lock，桌面不是被 SIGSTOP

`.c1pkg-mode` 决定 `/dev/shm/c1ancher-external-app.mode` 的值：不写该文件默认 `terminal`，写了 `direct`（C1ancher 二进制里 `terminal` 与 `direct` 两个字符串相邻，确实按值分支）。`c1pkg launch` 在 exec 前 flock 住 `.lock` 与 `.runlock`（advisory WRITE，`/proc/locks` 里可见 00:12:2 / 00:12:3），exec 后入口进程继承 fd 因而继续持锁。C1ancher 全程 `S`（`do_sys_poll`），**不被暂停**，只是拿不到锁就不再去碰 `/dev/epaper_lcd`；应用退出、锁释放，桌面自己接回屏幕。

自己抢屏、自己 grab `/dev/input` 的程序（如 C1Terminal）必须用 `direct`。

另：`.mode` 是普通文件，进程退出后值会残留。入口脚本应在**仍持锁时**把它清空（`: > /dev/shm/c1ancher-external-app.mode`）再退出，避免桌面按残留值分支。

## 未解锁的部分：正式标题

标题/作者只来自签名索引 `C1PKG-INDEX 2` 的 `P\t<id>\t<ver>\t<Title>\t…` 字段（第 4 列）。不在索引里的应用照常列出，但用 id 的小写形式当标签。

索引缓存 `/usr/data/c1/pkg/cache/verified.v1` = `[64B 签名][索引原文]`；信任锚是 `/usr/data/c1/pkg/repository.url` + 32 字节 `repository.ed25519.pub`；`highest-sequence`（当前 174）会拒绝序号更低的索引。自签仓库在 CLI 上可用 `--repo/--key` 走通，但**正在运行的 launcher 读的是存储的信任锚**，要拿到正式标题就得替换 `/usr/data/c1/pkg/repository.ed25519.pub`——那会废掉厂商应用商店的验签，本移植明确不做。

## 与旧写法的关系

[[c1-slim-device]] 里记的「SIGSTOP 掉 `/usr/data/c1/core/*` 全部进程」是**从 adb 侧**抢屏的做法（`scripts/c1term-install.sh` 生成的 `c1term-run.sh`）。从 launcher 走时不需要它，两条路不要同时开：两个 c1term 会抢 `/dev/input`（入口脚本已加 `pidof c1term` 拦截）。

参考仓库 C1-Slim-Ports 的做法（覆盖 `/usr/bin/d261/mpenMain` + 换一张 `ic_desktop_*.png` 图标 + `/usr/data/h` wrapper）是 **mpenMain 时代**的机制，在 C1ancher 上完全不适用。


## Timeline

- time: 2026-09-29T02:32:44
  kind: decision
  summary: "Created this page: C1ancher 应用/launcher 集成契约（c1pkg 权限信任 + external-app 交接）"
  source: "真机逆向实验 2026-09-29"
  affects: [c1ancher-app-integration]

- time: 2026-09-29T02:33:02
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: "真机逆向实验 2026-09-29"
  affects: [c1ancher-app-integration]
