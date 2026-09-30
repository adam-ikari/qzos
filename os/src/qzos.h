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

#endif /* QZOS_H */
