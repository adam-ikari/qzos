---
id: c1-wifi-stack
title: "C1 Slim Wi-Fi 栈：厂商脚本、生效配置与 CLI 配网约束"
category: reference
status: active
tags: [wifi, network, device]
created: "2026-09-29T02:32:45"
updated: "2026-09-29T02:33:18"
---

<!-- compiled_truth -->
射频是 **atbm603x SDIO**，模块 `/etc/firmware/atbm603x_wifi_sdio.ko` **按需 insmod**——默认不开 WiFi，`lsmod` 空、`/sys/class/net/wlan0` 不存在是正常状态。上下线一律复用厂商脚本，别自己拼命令。

## 厂商脚本（只读 rootfs 的 /bin，可直接调用）

| 脚本 | 实际做的事 |
|---|---|
| `wifi_up.sh` | insmod（最多 5 轮，失败 rmmod 重试）→ `rfkill unblock wifi` → `wpa_supplicant -B -i wlan0 -c /usr/resource/wpa_supplicant.conf` → `udhcpc -R -S -b -t 10 -T 2 -i wlan0 -x hostname:C1-Slim` |
| `wifi_down.sh` | `wpa_cli disconnect` → `ifconfig wlan0 0.0.0.0/down` → `wpa_cli terminate` → `killall -9 udhcpc wpa_supplicant` → `rfkill block wifi` → `rmmod atbm603x_wifi_sdio` |
| `wifi_download_fw_and_up.sh` | 等 `/sys/class/net/wlan0/address`、写 MAC、按 `env_wifi_enable_when_system_up` 决定是否 `wifi_up.sh` |
| `wifi_ap_mode_start.sh` / `_stop.sh` | AP 模式 |

## 生效的配置文件是哪一个（关键）

`wifi_up.sh` 硬编码读 **`/usr/resource/wpa_supplicant.conf`**（mmcblk0p5，ext4 rw 可执行，`ctrl_interface=/var/run/wpa_supplicant`、`update_config=1`、`country=GB`）。

`/usr/data/c1/wifi/wpa_supplicant.conf` 是桌面另一份拷贝（`ctrl_interface=/run/c1/wpa_ctrl`），**不是** `wifi_up.sh` 用的那份。要改凭据就改前者，否则改了不生效。

## CLI 配网的做法（scripts/c1wifi）

结论：**优先用 `wpa_cli`，不要文本编辑 conf**。`update_config=1` 让 `wpa_cli save_config` 自己原子回写同一份文件，比 sed 拼 network 块可靠，也天然处理转义。

- 凭据操作要求 supplicant 在跑，所以任何写操作前先 `radio_up`（无 `wlan0` 就调 `wifi_up.sh`，再等 `/var/run/wpa_supplicant` 套接字可用）。
- 按 ssid 找 network id：`wpa_cli list_networks | awk -F'\t' '$2==ssid {print $1}'`；没有就 `add_network`。
- 密码：64 位十六进制按 raw 传，其余用 `psk="\"...\""` 的引号形式；开放网络 `key_mgmt NONE`。**不支持 WEP**；含双引号的 ssid/psk 必须拒绝（conf 里引号会结束字段本身，是注入面）。
- 扫描别用 `iwlist`（慢、阻塞），用 `wpa_cli scan` + 轮询 `scan_results`；**加密要看 FLAGS 列**（`WPA|RSN`/`WEP`），`ESS` 是基础设施网络标志、不代表开放。
- 真机没有 `timeout` applet，所有等待都要自己写有界循环。

## 验证到什么程度

`scan`（含真实射频冷启动）、`list`、`join` 的新增-保存-超时失败、`forget` 的删除，全部实测通过；操作前后厂商已存的 3 个 network 块数量不变（不碰别人的凭据）。**成功分支未跑过**：设备周围没有我方持有凭据的 AP，所以 `wpa_state=COMPLETED` → 拿到 DHCP 租约这段只是按脚本逻辑推定。

相关：[[c1-slim-device]]、[[c1ancher-app-integration]]、[[port-verification]]。


## Timeline

- time: 2026-09-29T02:32:45
  kind: decision
  summary: "Created this page: C1 Slim Wi-Fi 栈：厂商脚本、生效配置与 CLI 配网约束"
  source: "真机 adb 实测 2026-09-29"
  affects: [c1-wifi-stack]

- time: 2026-09-29T02:33:18
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: "真机 adb 实测 2026-09-29"
  affects: [c1-wifi-stack]
