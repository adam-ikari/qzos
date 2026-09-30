/* qzos-host common declarations */
#ifndef QZOS_H
#define QZOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- display (display.c + panel.c + display_policy.c + raster_1bpp.c) ----
 * 显示已按四层拆分，便于移植到其他 SoC：
 *   raster_1bpp     纯栅格（1bpp 布局、脏区）——无状态可单测
 *   display_policy  纯决策（刷不刷、什么波形）——无 I/O 可单测
 *   panel           面板描述符 + 设备 I/O——唯一知道硬件细节的一层
 *   display         LVGL 驱动 + 编排
 * 换屏/换 SoC 只需改 panel.c 的描述符或设 QZ_PANEL* 环境变量。 */
#include "display.h"

/* ---- input (input.c) ---- */
struct uv_loop_s;
struct _lv_group_t; /* lv_group_t in lv_group.h */
int qzos_input_init(struct uv_loop_s *loop);
/* Keypad group used for focus navigation (created in input_init). */
struct _lv_group_t *qzos_input_group(void);
/* Internal: bridge uses this to push system-key JSON to JS. */
void qzos_bridge_send_key(const char *key);
/* 引擎死/活时切输入。停输入的理由：JS 引擎死后没有任何东西能消费按键，
 * 而画面还停在旧桌面上——用户会一直按下去。宁可不响应，不要骗人。 */
void qzos_input_enable(int on);

/* ---- services: uvrpc system-services plane (services.c) ---- */
typedef void (*qzos_rpc_done_t)(int ok, const char *result, size_t len, void *u);
int qzos_services_init(struct uv_loop_s *loop);
/* Async JSON-in/JSON-out RPC over uvrpc INPROC client (JS-facing bridge). */
void qzos_services_rpc(const char *method, const char *params_json,
                       qzos_rpc_done_t done, void *u);
const char *qzos_services_addr(void);

/* ---- bridge: JSON UI protocol JS <-> LVGL (bridge.c) ---- */
struct qz_t; /* qz_t */
void qzos_bridge_set_rt(struct qz_t *rt);
void qzos_bridge_handle(const char *json, size_t len);
/* Send a JSON message to JS (safe from the loop thread). */
void qzos_bridge_sendf(const char *fmt, ...);

/* ---- 引擎崩溃恢复（main.c 编排）----
 * bridge.c 识别到 qzjs 的 {"type":"error",...} 帧时调 qzos_host_on_rt_death，
 * 由宿主停输入 + 排一次带退避的 rt 重建。 */
void qzos_host_on_rt_death(const char *reason);
/* 重启期间显示/撤下提示。画在屏幕上是因为「什么都不显示」正是要修的病症：
 * 用户需要一个明确的「系统正在重启」，而不是一块看着正常的死画面。 */
void qzos_show_rt_dead(uint32_t retry_ms);
void qzos_hide_rt_dead(void);

#endif /* QZOS_H */
